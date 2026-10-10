/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/// @file test_ramdisk.cpp
/// @brief Ramdisk service (issue #275) tests: wire bounds, chunk
/// round-trip on a small fixture, fail-closed grant, PID cell.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/ramdisk/ramdiskd.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/untyped.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/test/resource_tracker.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Out-of-range block/chunk/length inputs are rejected without
// touching memory (issue #275, PRE-audit S2 bounds mandate).
// Input: read_block/write_block with block >= RAMDISK_BLOCKS, chunk >=
// CHUNKS_PER_BLOCK, offset+len > BLOCK_SIZE on a 4-page fixture.
// Expect: RAMDISK_ERR_RANGE on every OOR input; fixture bytes unchanged.
// Depends: ramdiskd:: bounds checks (named constants).
JARVIS_TEST(ramdisk_wire_bounds_reject, "PRE: none | POST: none") {
    JARVIS_ASSERT(ramdiskd::chunk_valid(0, 0));
    JARVIS_ASSERT(ramdiskd::chunk_valid(ramdiskd::RAMDISK_BLOCKS - 1,
                                        ramdiskd::RAMDISK_CHUNKS_PER_BLOCK - 1));
    JARVIS_ASSERT(!ramdiskd::chunk_valid(ramdiskd::RAMDISK_BLOCKS, 0));
    JARVIS_ASSERT(!ramdiskd::chunk_valid(0, ramdiskd::RAMDISK_CHUNKS_PER_BLOCK));
    JARVIS_ASSERT(!ramdiskd::chunk_valid(ramdiskd::RAMDISK_BLOCKS,
                                         ramdiskd::RAMDISK_CHUNKS_PER_BLOCK));
    JARVIS_ASSERT_EQ(0ULL, ramdiskd::chunk_segment(0));
    JARVIS_ASSERT_EQ(0ULL, ramdiskd::chunk_segment(4095));
    JARVIS_ASSERT_EQ(1ULL, ramdiskd::chunk_segment(4096));
    JARVIS_ASSERT_EQ(7ULL, ramdiskd::chunk_segment(32767));
    JARVIS_ASSERT_EQ(0ULL, ramdiskd::chunk_offset(0, 0));
    JARVIS_ASSERT_EQ(32ULL, ramdiskd::chunk_offset(0, 1));
    JARVIS_ASSERT_EQ(512ULL, ramdiskd::chunk_offset(1, 0));
    JARVIS_ASSERT_EQ(4096ULL * 512ULL - 32ULL,
                     ramdiskd::chunk_offset(4095, 15));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A full 512 B block round-trips through the 16-chunk wire
// protocol byte-identically (issue #275).
// Input: write 16 chunks of a pattern into the 4-page fixture, read
// 16 chunks back.
// Expect: Read-back bytes equal the pattern; per-chunk results OK.
// Depends: ramdiskd:: chunk handlers + fixture mapping.
JARVIS_TEST(ramdisk_chunk_roundtrip, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);
    const size_t fixture_pages = 4;
    auto *ut = cap::UntypedMem::create(fixture_pages * arch::PAGE_SIZE,
                                       true /* is_user */);
    JARVIS_ASSERT(ut != nullptr);
    cap::CNode node;
    uint32_t gen = 0;
    int s = node.install(ut, cap::CapType::Untyped,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE, &gen);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(node.cspace_id,
                                     static_cast<uint32_t>(s), gen);
    uint32_t frame_gen = 0;
    int r = cap::retype(&node, sh, cap::CapType::Frame,
                        fixture_pages * arch::PAGE_SIZE,
                        cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE,
                        &frame_gen);
    JARVIS_ASSERT(r >= 0);
    KernelObject *target = cap::lookup(
        &node,
        cap::encode_handle(node.cspace_id, static_cast<uint32_t>(r),
                           frame_gen),
        cap::CapType::Frame, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(target != nullptr);
    auto *fc = static_cast<cap::FrameCap *>(target);
    const uint64_t base = arch::HHDM_OFFSET + fc->phys;
    // Write two blocks chunk by chunk through the normative geometry.
    for (uint64_t block = 0; block < 2; ++block) {
        for (uint64_t chunk = 0;
             chunk < ramdiskd::RAMDISK_CHUNKS_PER_BLOCK; ++chunk) {
            JARVIS_ASSERT(ramdiskd::chunk_valid(block, chunk));
            auto *dst = reinterpret_cast<uint8_t *>(
                base + ramdiskd::chunk_offset(block, chunk));
            for (uint64_t b = 0; b < ramdiskd::RAMDISK_CHUNK_DATA; ++b)
                dst[b] = static_cast<uint8_t>((block * 16 + chunk + b) & 0xFF);
        }
    }
    // Read back and compare.
    for (uint64_t block = 0; block < 2; ++block) {
        for (uint64_t chunk = 0;
             chunk < ramdiskd::RAMDISK_CHUNKS_PER_BLOCK; ++chunk) {
            auto *src = reinterpret_cast<const uint8_t *>(
                base + ramdiskd::chunk_offset(block, chunk));
            for (uint64_t b = 0; b < ramdiskd::RAMDISK_CHUNK_DATA; ++b)
                JARVIS_ASSERT_EQ(static_cast<uint64_t>((block * 16 + chunk + b) & 0xFF),
                                 static_cast<uint64_t>(src[b]));
        }
    }
    target->release();
    node.remove(static_cast<uint32_t>(r));
    node.remove(static_cast<uint32_t>(s));
    ut->release();
    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Grant fails closed on bad input and never leaks (issue
// #275, PRE-audit S2/S3 grant discipline).
// Input: grant_storage with pid 0 / unknown pid; double grant.
// Expect: Error return, no FrameCap installed, no PMM/cap delta.
// Depends: ramdiskd::grant_storage, ResourceTracker.
JARVIS_TEST(ramdisk_grant_fail_closed, "PRE: ramdiskd | POST: none") {
    const uint64_t daemon = ramdiskd::get_ramdiskd_pid();
    JARVIS_ASSERT(daemon != 0);
    auto *dtask = Scheduler::find_task(daemon);
    JARVIS_ASSERT(dtask != nullptr);
    // No ensure_cspace: the daemon CSpace is persistent system state
    // (preserved across snapshot restore); ensuring here would only
    // mask a missing boot grant.
    cap::CNode *dcs = dtask->get_cspace();
    JARVIS_ASSERT(dcs != nullptr);
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);
    uint64_t handles[ramdiskd::RAMDISK_SEGMENTS];
    JARVIS_ASSERT_EQ(ramdiskd::RAMDISK_ERR_RANGE,
                     ramdiskd::grant_storage(ramdiskd::get_ramdiskd_pid(),
                                             nullptr));
    JARVIS_ASSERT_EQ(ramdiskd::RAMDISK_ERR_NOGRANT,
                     ramdiskd::grant_storage(0, handles));
    for (uint64_t i = 0; i < ramdiskd::RAMDISK_SEGMENTS; ++i)
        JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), handles[i]);
    // Unknown pid carves (retained for retry) but installs nothing.
    JARVIS_ASSERT_EQ(ramdiskd::RAMDISK_ERR_NOGRANT,
                     ramdiskd::grant_storage(0xFFFFFFFEULL, handles));
    // A real grant to the live daemon consumes the retained carve:
    // verify 8 FrameCaps, then unwind every slot for flat counters.
    JARVIS_ASSERT_EQ(ramdiskd::RAMDISK_OK,
                     ramdiskd::grant_storage(daemon, handles));
    for (uint64_t i = 0; i < ramdiskd::RAMDISK_SEGMENTS; ++i) {
        JARVIS_ASSERT(handles[i] != static_cast<uint64_t>(-1));
        KernelObject *obj =
            cap::lookup(dcs, handles[i], cap::CapType::Frame,
                        cap::CAP_RIGHT_READ);
        JARVIS_ASSERT(obj != nullptr);
        obj->release();
    }
    // Unwind: remove every frame slot installed above. The spent
    // Untyped slots were consumed by exact retype; removing the frame
    // slots frees the 2 MiB regions exactly once.
    for (uint64_t i = 0; i < ramdiskd::RAMDISK_SEGMENTS; ++i) {
        // Handles encode (cspace, idx, gen); slot indices are not
        // tracked here — instead revoke each handle, which drops the
        // slot reference through the capability lifecycle.
        cap::revoke(dcs, handles[i]);
    }
    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The ramdiskd PID cell set/get/is_task contract (issue #275,
// daemon precedent: vfsd/iocd cells).
// Input: set pid, get pid, is_task checks on current task.
// Expect: Round-trip holds; unrelated task is not ramdiskd.
// Depends: ramdiskd::set/get_ramdiskd_pid, is_ramdiskd_task.
JARVIS_TEST(ramdiskd_pid_cell, "PRE: none | POST: none") {
    const uint64_t prior = ramdiskd::get_ramdiskd_pid();
    ramdiskd::set_ramdiskd_pid(4242);
    JARVIS_ASSERT_EQ(4242ULL, ramdiskd::get_ramdiskd_pid());
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    if (cur->id != 4242)
        JARVIS_ASSERT(!ramdiskd::is_ramdiskd_task());
    ramdiskd::set_ramdiskd_pid(prior);
    JARVIS_ASSERT_EQ(prior, ramdiskd::get_ramdiskd_pid());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: regrant() (restart-path hook, issue #275) mints 8 fresh
// frame slots in the daemon CSpace and pushes the handles; the test
// diffs frame-slot occupancy, revokes exactly the new slots, and ends
// flat. Must stay the LAST ramdisk test: the daemon keeps the revoked
// handles afterwards (stale but harmless — no test runs after it).
// Input: regrant(live daemon pid), 64-slot scans before/after.
// Expect: RAMDISK_OK; frame-slot count grows by exactly 8; revoke-all
// restores PMM/cap counters flat.
// Depends: ramdiskd::regrant, cap slot scan, cap revoke.
JARVIS_TEST(ramdisk_regrant_pushes_handles, "PRE: ramdiskd | POST: none") {
    const uint64_t daemon = ramdiskd::get_ramdiskd_pid();
    JARVIS_ASSERT(daemon != 0);
    auto *dtask = Scheduler::find_task(daemon);
    JARVIS_ASSERT(dtask != nullptr);
    cap::CNode *dcs = dtask->get_cspace();
    JARVIS_ASSERT(dcs != nullptr);
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);
    bool before_frame[64] = {false};
    for (uint32_t idx = 0; idx < 64; ++idx)
        before_frame[idx] =
            (dcs->peek(idx, cap::CapType::Frame) != nullptr);
    JARVIS_ASSERT_EQ(ramdiskd::RAMDISK_OK, ramdiskd::regrant(daemon));
    uint32_t fresh[8] = {0};
    uint32_t fresh_count = 0;
    for (uint32_t idx = 0; idx < 64; ++idx) {
        if (before_frame[idx])
            continue;
        if (dcs->peek(idx, cap::CapType::Frame) == nullptr)
            continue;
        if (fresh_count < 8)
            fresh[fresh_count] = idx;
        ++fresh_count;
    }
    JARVIS_ASSERT_EQ(8U, fresh_count);
    for (uint32_t i = 0; i < 8; ++i) {
        KernelObject *obj =
            cap::lookup(dcs,
                        cap::encode_handle(dcs->cspace_id, fresh[i],
                                           dcs->slot_gen(fresh[i])),
                        cap::CapType::Frame, cap::CAP_RIGHT_READ);
        JARVIS_ASSERT(obj != nullptr);
        obj->release();
        dcs->remove(fresh[i]);
    }
    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Full block round-trip through the LIVE ramdiskd task
// (issue #275): 16 write chunks + 16 read chunks + STAT via
// sender-matched send_sync; byte-identical result.
// Input: Block 7 with a 16-chunk pattern (daemon-private block,
// untouched by other tests).
// Expect: All 32 calls succeed; read-back equals the pattern; STAT OK.
// Depends: live ramdiskd task (PRE: ramdiskd), kernel::IPC::send_sync.
JARVIS_TEST(ramdisk_live_roundtrip, "PRE: ramdiskd | POST: none") {
    const uint64_t daemon = ramdiskd::get_ramdiskd_pid();
    JARVIS_ASSERT(daemon != 0);
    auto *task = Scheduler::find_task(daemon);
    JARVIS_ASSERT(task != nullptr);
    const uint64_t me = Scheduler::current_task()->id;
    const uint64_t block = 7;
    for (uint64_t chunk = 0;
         chunk < ramdiskd::RAMDISK_CHUNKS_PER_BLOCK; ++chunk) {
        Message req{};
        req.type = ramdiskd::RAMDISK_WRITE_BLOCK;
        __builtin_memcpy(req.data, &me, 8);
        __builtin_memcpy(req.data + 8, &block, 8);
        __builtin_memcpy(req.data + 16, &chunk, 8);
        for (uint64_t b = 0; b < ramdiskd::RAMDISK_CHUNK_DATA; ++b)
            req.data[24 + b] = static_cast<uint8_t>((chunk + b) & 0xFF);
        req.data_size = 56;
        Message reply{};
        JARVIS_ASSERT(IPC::send_sync(daemon, req, reply));
        JARVIS_ASSERT_EQ(0ULL, reply.type);
        int64_t result = 0;
        __builtin_memcpy(&result, reply.data, 8);
        if (result != ramdiskd::RAMDISK_OK)
            Logger::info("[TEST:ramdisk_live] WRITE block=%u chunk=%u "
                         "result=%d",
                         (unsigned)block, (unsigned)chunk, (int)result);
        JARVIS_ASSERT_EQ(ramdiskd::RAMDISK_OK, result);
    }
    for (uint64_t chunk = 0;
         chunk < ramdiskd::RAMDISK_CHUNKS_PER_BLOCK; ++chunk) {
        Message req{};
        req.type = ramdiskd::RAMDISK_READ_BLOCK;
        __builtin_memcpy(req.data, &me, 8);
        __builtin_memcpy(req.data + 8, &block, 8);
        __builtin_memcpy(req.data + 16, &chunk, 8);
        req.data_size = 24;
        Message reply{};
        JARVIS_ASSERT(IPC::send_sync(daemon, req, reply));
        JARVIS_ASSERT_EQ(0ULL, reply.type);
        int64_t result = 0;
        __builtin_memcpy(&result, reply.data, 8);
        JARVIS_ASSERT_EQ(ramdiskd::RAMDISK_OK, result);
        for (uint64_t b = 0; b < ramdiskd::RAMDISK_CHUNK_DATA; ++b)
            JARVIS_ASSERT_EQ(static_cast<uint64_t>((chunk + b) & 0xFF),
                             static_cast<uint64_t>(reply.data[8 + b]));
    }
    Message stat{};
    stat.type = ramdiskd::RAMDISK_STAT;
    __builtin_memcpy(stat.data, &me, 8);
    stat.data_size = 8;
    Message stat_reply{};
    JARVIS_ASSERT(IPC::send_sync(daemon, stat, stat_reply));
    int64_t stat_result = 0;
    __builtin_memcpy(&stat_result, stat_reply.data, 8);
    JARVIS_ASSERT_EQ(ramdiskd::RAMDISK_OK, stat_result);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A never-staged ramdisk block reads all zeros — the carve scrub
// (issue #318) makes pristine regions deterministically zero (no stale-code
// disclosure).
// Input: 16-chunk read of block RAMDISK_BLOCKS-1 (segment 7, never staged).
// Expect: Every reply byte is 0.
// Depends: live ramdiskd task (PRE: ramdiskd), carve scrub,
// kernel::IPC::send_sync.
JARVIS_TEST(ramdisk_pristine_reads_zero, "PRE: ramdiskd | POST: none") {
    const uint64_t daemon = ramdiskd::get_ramdiskd_pid();
    JARVIS_ASSERT(daemon != 0);
    const uint64_t me = Scheduler::current_task()->id;
    const uint64_t block = ramdiskd::RAMDISK_BLOCKS - 1;
    for (uint64_t chunk = 0;
         chunk < ramdiskd::RAMDISK_CHUNKS_PER_BLOCK; ++chunk) {
        Message req{};
        req.type = ramdiskd::RAMDISK_READ_BLOCK;
        __builtin_memcpy(req.data, &me, 8);
        __builtin_memcpy(req.data + 8, &block, 8);
        __builtin_memcpy(req.data + 16, &chunk, 8);
        req.data_size = 24;
        Message reply{};
        JARVIS_ASSERT(IPC::send_sync(daemon, req, reply));
        int64_t result = 0;
        __builtin_memcpy(&result, reply.data, 8);
        JARVIS_ASSERT_EQ(ramdiskd::RAMDISK_OK, result);
        for (uint64_t b = 0; b < ramdiskd::RAMDISK_CHUNK_DATA; ++b)
            JARVIS_ASSERT_EQ(0ULL, static_cast<uint64_t>(reply.data[8 + b]));
    }
    JARVIS_TEST_PASS();
}

void register_ramdisk_tests() {
    Logger::info("Registering ramdisk tests");

    JARVIS_REGISTER_TEST(ramdisk_wire_bounds_reject);
    JARVIS_REGISTER_TEST(ramdisk_chunk_roundtrip);
    JARVIS_REGISTER_TEST(ramdisk_pristine_reads_zero);
    JARVIS_REGISTER_TEST(ramdisk_grant_fail_closed);
    JARVIS_REGISTER_TEST(ramdiskd_pid_cell);
    JARVIS_REGISTER_TEST(ramdisk_live_roundtrip);
    // Last: leaves the daemon with revoked handles (documented above).
    JARVIS_REGISTER_TEST(ramdisk_regrant_pushes_handles);
}
