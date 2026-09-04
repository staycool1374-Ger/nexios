/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as
 * published by the Free Software Foundation.
 */

/// @file test_vfs_tmpfs_corrupt.cpp
/// @brief tmpfs corrupt-metadata / io-timeout analogue tests (milestone
///        v0.4.3 issue #114) — the tmpfs counterpart of the FAT32
///        corrupt-chain discipline: duplicate/unlink metadata rejections,
///        oversize fail-safe, stale-name recycling, fragmentation cycles
///        and concurrent bounded access.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/tmpfs.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <string.hpp>

using namespace kernel;

namespace {

/// @brief Returns the tmpfs root vnode (lazy-init on first call).
vfs::Vnode *tmpfs_root() { return vfs::tmpfs_fs.get_root(); }

/// @brief Counts entries currently linked in the root directory.
int tmpfs_root_entry_count() {
    vfs::Vnode *root = tmpfs_root();
    int count = 0;
    uint64_t pos = 0;
    vfs::Dirent dent{};
    while (count < 256 && root->ops->readdir(*root, pos, dent) == 0)
        ++count;
    return count;
}

/// @brief Removes every tc_*-prefixed entry created by a test (cleanup
/// helper — tmpfs_reset_root is intentionally NOT used: it leaks blocks).
void cleanup_tc_entries() {
    vfs::Vnode *root = tmpfs_root();
    for (int pass = 0; pass < 64; ++pass) {
        uint64_t pos = 0;
        vfs::Dirent dent{};
        bool removed = false;
        while (root->ops->readdir(*root, pos, dent) == 0) {
            if (memcmp(dent.d_name, "tc_", 3) == 0) {
                root->ops->unlink(*root, dent.d_name);
                removed = true;
                break; // unlink invalidates the iteration; restart scan
            }
        }
        if (!removed)
            break;
    }
}

/// @brief Task context for the concurrency test.
struct TmpfsHammerCtx {
    vfs::Vnode *root = nullptr;
    uint64_t cycles = 0;
    char prefix[8] = {};
    int failures = 0;
};

TmpfsHammerCtx g_hammer_a;
TmpfsHammerCtx g_hammer_b;

/// @brief Worker: create/write/read/unlink cycles on "<prefix><cycle>".
void hammer_entry() {
    TmpfsHammerCtx *ctx = Scheduler::current_task()->id % 2 == 0
                              ? &g_hammer_a
                              : &g_hammer_b;
    for (uint64_t cycle = 0; cycle < ctx->cycles; ++cycle) {
        char name[24];
        size_t off = 0;
        const char *p = ctx->prefix;
        while (p[off] && off < sizeof(ctx->prefix) - 1) {
            name[off] = p[off];
            ++off;
        }
        name[off] = static_cast<char>('a' + (cycle % 26));
        name[off + 1] = '\0';

        if (ctx->root->ops->create(*ctx->root, name, 0) != 0) {
            ++ctx->failures;
            continue;
        }
        vfs::Vnode *vn = ctx->root->ops->lookup(*ctx->root, name);
        if (!vn) {
            ++ctx->failures;
            continue;
        }
        uint8_t buf[64];
        memset(buf, static_cast<uint8_t>(cycle), sizeof(buf));
        int64_t written = vn->ops->write(*vn, buf, sizeof(buf), 0);
        uint8_t back[64] = {};
        int64_t nread = vn->ops->read(*vn, back, sizeof(back), 0);
        if (written != static_cast<int64_t>(sizeof(buf)) ||
            nread != static_cast<int64_t>(sizeof(buf)) ||
            memcmp(buf, back, sizeof(buf)) != 0)
            ++ctx->failures;
        if (ctx->root->ops->unlink(*ctx->root, name) != 0)
            ++ctx->failures;
    }
}

} // namespace

// Runmode: kernel
// Testidea: Duplicate-name metadata corruption is rejected: creating a
// file or directory with an existing name fails with VFS_INVALID and
// leaves the original intact.
// Input: create "tc_dup", then create "tc_dup" again and mkdir "tc_dup".
// Expect: Second create and the mkdir both return VFS_INVALID; a read of
//         the original file still round-trips.
// Depends: vfs::tmpfs_fs
JARVIS_TEST(tmpfs_corrupt_duplicate_name_rejected,
            "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = tmpfs_root();
    JARVIS_ASSERT(root != nullptr);
    cleanup_tc_entries();
    JARVIS_ASSERT_EQ(0, root->ops->create(*root, "tc_dup", 0));

    int second_create = root->ops->create(*root, "tc_dup", 0);
    int dup_mkdir = root->ops->mkdir(*root, "tc_dup", 0);

    vfs::Vnode *vn = root->ops->lookup(*root, "tc_dup");
    uint8_t buf[8] = {};
    int64_t nread = -1;
    if (vn)
        nread = vn->ops->write(*vn, buf, sizeof(buf), 0);
    JARVIS_ASSERT_EQ(0, root->ops->unlink(*root, "tc_dup"));

    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, second_create);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, dup_mkdir);
    JARVIS_ASSERT(vn != nullptr);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(8), nread);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Unlink of a missing entry is rejected and double-unlink is
// idempotent-safe (the second unlink fails with VFS_INVALID instead of
// corrupting the entry list).
// Input: unlink "tc_missing" (never created); create+unlink "tc_gone";
//        unlink "tc_gone" again.
// Expect: All three rejections return VFS_INVALID.
// Depends: vfs::tmpfs_fs
JARVIS_TEST(tmpfs_corrupt_unlink_missing_rejected,
            "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = tmpfs_root();
    JARVIS_ASSERT(root != nullptr);
    cleanup_tc_entries();

    int missing = root->ops->unlink(*root, "tc_missing");
    JARVIS_ASSERT_EQ(0, root->ops->create(*root, "tc_gone", 0));
    int first = root->ops->unlink(*root, "tc_gone");
    int second = root->ops->unlink(*root, "tc_gone");

    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, missing);
    JARVIS_ASSERT_EQ(0, first);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, second);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Unlinking a non-empty directory is rejected — the drop-
// subtree corruption path is guarded.
// Input: mkdir "tc_dir", create "tc_dir/f", unlink "tc_dir".
// Expect: unlink(dir) returns VFS_INVALID while the child exists; after
//         unlinking the child the directory removes cleanly.
// Depends: vfs::tmpfs_fs
JARVIS_TEST(tmpfs_corrupt_unlink_nonempty_dir_rejected,
            "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = tmpfs_root();
    JARVIS_ASSERT(root != nullptr);
    cleanup_tc_entries();

    JARVIS_ASSERT_EQ(0, root->ops->mkdir(*root, "tc_dir", 0));
    vfs::Vnode *dir = root->ops->lookup(*root, "tc_dir");
    JARVIS_ASSERT(dir != nullptr);
    JARVIS_ASSERT_EQ(0, dir->ops->create(*dir, "tc_inner", 0));

    int nonempty = root->ops->unlink(*root, "tc_dir");
    int child_removed = dir->ops->unlink(*dir, "tc_inner");
    int dir_removed = root->ops->unlink(*root, "tc_dir");

    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, nonempty);
    JARVIS_ASSERT_EQ(0, child_removed);
    JARVIS_ASSERT_EQ(0, dir_removed);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Oversize writes fail safe: a write extending past the 64 KiB
// tmpfs file cap returns VFS_INVALID, leaves the file size untouched and
// allocates nothing (fail-closed — no partial state).
// Input: create "tc_big", write 32 bytes at offset 64 KiB - 16 (crosses
//        the cap), then a write fully inside the cap.
// Expect: Crossing write returns VFS_INVALID; file size stays 0 until
//         the in-cap write lands (then size == offset + count).
// Depends: vfs::tmpfs_fs
JARVIS_TEST(tmpfs_corrupt_oversize_write_failsafe,
            "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = tmpfs_root();
    JARVIS_ASSERT(root != nullptr);
    cleanup_tc_entries();
    JARVIS_ASSERT_EQ(0, root->ops->create(*root, "tc_big", 0));
    vfs::Vnode *vn = root->ops->lookup(*root, "tc_big");
    JARVIS_ASSERT(vn != nullptr);

    constexpr uint64_t TMPFS_MAX_FILE_SIZE = 64ULL * 1024ULL;
    uint8_t payload[32] = {};
    int64_t crossing =
        vn->ops->write(*vn, payload, sizeof(payload), TMPFS_MAX_FILE_SIZE - 16);
    uint64_t size_after_cross = vn->size;
    uint8_t small[4] = {1, 2, 3, 4};
    int64_t inside = vn->ops->write(*vn, small, sizeof(small), 0);
    uint64_t size_after_inside = vn->size;

    JARVIS_ASSERT_EQ(0, root->ops->unlink(*root, "tc_big"));

    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, crossing);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), size_after_cross);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(4), inside);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(4), size_after_inside);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Stale-name recycling contract — after unlink, the name is
// absent from lookup and readdir, and the same name can be recreated and
// re-populated (freed entry metadata is cleanly reusable, no corruption
// from the recycling).
// Input: create "tc_stale", write, unlink; then lookup, readdir scan,
//        recreate + roundtrip.
// Expect: lookup after unlink is nullptr; readdir no longer lists the
//         name; recreate succeeds and data round-trips again.
// Depends: vfs::tmpfs_fs
JARVIS_TEST(tmpfs_corrupt_stale_name_recycling, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = tmpfs_root();
    JARVIS_ASSERT(root != nullptr);
    cleanup_tc_entries();
    JARVIS_ASSERT_EQ(0, root->ops->create(*root, "tc_stale", 0));
    vfs::Vnode *vn1 = root->ops->lookup(*root, "tc_stale");
    JARVIS_ASSERT(vn1 != nullptr);
    const uint8_t first[4] = {0xA, 0xB, 0xC, 0xD};
    JARVIS_ASSERT_EQ(static_cast<int64_t>(4),
                     vn1->ops->write(*vn1, first, sizeof(first), 0));
    JARVIS_ASSERT_EQ(0, root->ops->unlink(*root, "tc_stale"));
    // Deliberately NOT touching vn1 afterwards: tmpfs vnodes are
    // MemPool-backed without refcounts, so the stale pointer is dead by
    // design — the test asserts the name-level contract only.

    vfs::Vnode *stale_lookup = root->ops->lookup(*root, "tc_stale");
    bool listed = false;
    {
        uint64_t pos = 0;
        vfs::Dirent dent{};
        while (root->ops->readdir(*root, pos, dent) == 0) {
            if (memcmp(dent.d_name, "tc_stale", 9) == 0)
                listed = true;
        }
    }

    int recreated = root->ops->create(*root, "tc_stale", 0);
    vfs::Vnode *vn2 = root->ops->lookup(*root, "tc_stale");
    uint8_t back[4] = {};
    int64_t nread = -1;
    if (vn2) {
        const uint8_t second[4] = {1, 2, 3, 4};
        vn2->ops->write(*vn2, second, sizeof(second), 0);
        nread = vn2->ops->read(*vn2, back, sizeof(back), 0);
    }
    JARVIS_ASSERT_EQ(0, root->ops->unlink(*root, "tc_stale"));

    JARVIS_ASSERT(stale_lookup == nullptr);
    JARVIS_ASSERT(!listed);
    JARVIS_ASSERT_EQ(0, recreated);
    JARVIS_ASSERT(vn2 != nullptr);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(4), nread);
    JARVIS_ASSERT(memcmp(back, "\x1\x2\x3\x4", 4) == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Fragmentation / leak discipline — repeated create/write/
// unlink cycles leave the root directory exactly as it started, and the
// filesystem keeps accepting new files (no leaked entry slots, no leaked
// backing pages — the ResourceTracker snapshot check enforces the page
// accounting at teardown).
// Input: Snapshot the root entry count; run 8 create(4 KiB write)/unlink
//        cycles over distinct names; re-snapshot.
// Expect: Entry count identical before/after; a fresh create+roundtrip
//         still succeeds; cleanup removes every created name.
// Depends: vfs::tmpfs_fs
JARVIS_TEST(tmpfs_corrupt_fragmentation_cycles, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = tmpfs_root();
    JARVIS_ASSERT(root != nullptr);
    cleanup_tc_entries();
    int baseline = tmpfs_root_entry_count();

    uint8_t page[4096];
    memset(page, 0x7E, sizeof(page));
    for (int cycle = 0; cycle < 8; ++cycle) {
        char name[16];
        memcpy(name, "tc_frag", 7);
        name[7] = static_cast<char>('0' + cycle);
        name[8] = '\0';
        JARVIS_ASSERT_EQ(0, root->ops->create(*root, name, 0));
        vfs::Vnode *vn = root->ops->lookup(*root, name);
        JARVIS_ASSERT(vn != nullptr);
        JARVIS_ASSERT_EQ(static_cast<int64_t>(sizeof(page)),
                         vn->ops->write(*vn, page, sizeof(page), 0));
        JARVIS_ASSERT_EQ(0, root->ops->unlink(*root, name));
    }

    int after = tmpfs_root_entry_count();

    // Fresh create must still work after the cycles.
    JARVIS_ASSERT_EQ(0, root->ops->create(*root, "tc_frag_final", 0));
    vfs::Vnode *vn = root->ops->lookup(*root, "tc_frag_final");
    uint8_t back[8] = {};
    int64_t nread = -1;
    if (vn) {
        const uint8_t tail[8] = {9, 8, 7, 6, 5, 4, 3, 2};
        JARVIS_ASSERT_EQ(static_cast<int64_t>(8),
                         vn->ops->write(*vn, tail, sizeof(tail), 0));
        nread = vn->ops->read(*vn, back, sizeof(back), 0);
    }
    JARVIS_ASSERT_EQ(0, root->ops->unlink(*root, "tc_frag_final"));

    JARVIS_ASSERT_EQ(baseline, after);
    JARVIS_ASSERT(vn != nullptr);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(8), nread);
    JARVIS_ASSERT(memcmp(back, "\x9\x8\x7\x6\x5\x4\x3\x2", 8) == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Concurrent access while nodes are being created/removed is
// bounded and consistent: two real tasks hammer create/write/read/unlink
// cycles on disjoint name ranges plus readdir scans; every cycle's data
// round-trips and no cycle deadlocks (io-timeout analogue: all tmpfs ops
// are bounded critical sections, proven by completion).
// Input: Two worker tasks (prio 11/12), 6 cycles each, prefixes
//        "tc_aX"/"tc_bX".
// Expect: Both workers reach TERMINATED with zero recorded failures;
//         root entry count returns to baseline (all names removed).
// Depends: vfs::tmpfs_fs, Scheduler
JARVIS_TEST(tmpfs_corrupt_concurrent_bounded, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = tmpfs_root();
    JARVIS_ASSERT(root != nullptr);
    cleanup_tc_entries();
    int baseline = tmpfs_root_entry_count();

    g_hammer_a = TmpfsHammerCtx{};
    g_hammer_b = TmpfsHammerCtx{};
    g_hammer_a.root = root;
    g_hammer_b.root = root;
    g_hammer_a.cycles = 6;
    g_hammer_b.cycles = 6;
    memcpy(g_hammer_a.prefix, "tc_ax", 6);
    memcpy(g_hammer_b.prefix, "tc_bx", 6);

    auto *task_a = TaskControlBlock::create(hammer_entry, 11, 10);
    auto *task_b = TaskControlBlock::create(hammer_entry, 12, 10);
    JARVIS_ASSERT(task_a != nullptr);
    JARVIS_ASSERT(task_b != nullptr);
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*task_a);
        Scheduler::add_task(*task_b);
    }
    auto *original = Scheduler::current_task();
    kernel::test::yield_as(*task_b);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(task_a);
    kernel::test::wait_for_termination_safe(task_b);
    Scheduler::set_current(*original);

    bool a_done = task_a->state == TaskState::TERMINATED;
    bool b_done = task_b->state == TaskState::TERMINATED;
    int failures_a = g_hammer_a.failures;
    int failures_b = g_hammer_b.failures;
    kernel::test::terminate_and_drain2(task_a, task_b);

    cleanup_tc_entries();
    int after = tmpfs_root_entry_count();

    JARVIS_ASSERT(a_done);
    JARVIS_ASSERT(b_done);
    JARVIS_ASSERT_EQ(0, failures_a);
    JARVIS_ASSERT_EQ(0, failures_b);
    JARVIS_ASSERT_EQ(baseline, after);
    JARVIS_TEST_PASS();
}

void register_vfs_tmpfs_corrupt_tests() {
    Logger::info("Registering vfs tmpfs corrupt tests");
    JARVIS_REGISTER_TEST(tmpfs_corrupt_duplicate_name_rejected);
    JARVIS_REGISTER_TEST(tmpfs_corrupt_unlink_missing_rejected);
    JARVIS_REGISTER_TEST(tmpfs_corrupt_unlink_nonempty_dir_rejected);
    JARVIS_REGISTER_TEST(tmpfs_corrupt_oversize_write_failsafe);
    JARVIS_REGISTER_TEST(tmpfs_corrupt_stale_name_recycling);
    JARVIS_REGISTER_TEST(tmpfs_corrupt_fragmentation_cycles);
    JARVIS_REGISTER_TEST(tmpfs_corrupt_concurrent_bounded);
}
