/*
 * NexIOS RTOS — Capability-Based Access Control (CSpace)
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

/// @file test_cap_syscall.cpp
/// @brief SYS_CAP_* syscall dispatch tests.  Every syscall is invoked by a
///        REAL kernel task via Syscall::handle() so syscall_task() resolves
///        to the genuinely-running task (v0.3.10 driven-test discipline).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/cap_errors.hpp>
#include <kernel/cap/derivation.hpp>
#include <kernel/cap/untyped.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/test/resource_tracker.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

/// @brief Minimal shared-heap KernelObject used as a capability target.
///        Pool-backed in tests (allocated via MemPool), so dispose() returns
///        the block and balances the ResourceTracker counter.
struct TestTarget : public KernelObject {
    static uint32_t g_dispose_count;
    void dispose() noexcept override {
        ++g_dispose_count;
        if (is_pool_backed()) {
            kernel::test::ResourceTracker::instance().track_cap_object_remove();
            MemPool::free(this);
        }
    }
    bool is_shared() const noexcept override {
        return true;
    }
};

uint32_t TestTarget::g_dispose_count = 0;

/// @brief Runs @p entry as a real task, waits for termination, drains zombies
///        and returns the task (owned by the zombie list until drained).
TaskControlBlock *run_cap_task(void (*entry)(), uint64_t prio = 11,
                               uint64_t period = 10) {
    auto *t = TaskControlBlock::create(entry, prio, period);
    if (t == nullptr)
        return nullptr;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();
    return t;
}

/// @brief Installs a CNode capability into @p t's CSpace so the task can
///        address a destination for grant/copy/mint.  Returns the dest slot
///        index via @p slot.
void install_dest_cspace(TaskControlBlock *t, cap::CNode **dst_out,
                         int *slot_out) {
    t->ensure_cspace();
    cap::CNode *cspace = t->get_cspace();
    auto *dst = static_cast<cap::CNode *>(MemPool::alloc(sizeof(cap::CNode)));
    if (!dst)
        return;
    new (dst) cap::CNode;
    dst->mark_pool_backed();
    dst->cspace_id = 0x99;
    kernel::test::ResourceTracker::instance().track_cap_object_add();
    int s = cspace->install(dst, cap::CapType::CNode,
                            cap::CapRights::GRANT | cap::CapRights::COPY);
    if (s < 0) {
        dst->release();
        return;
    }
    *dst_out = dst;
    *slot_out = s;
}

/// @brief Destroys the dest CNode and its slot in @p t's CSpace.
void teardown_dest_cspace(TaskControlBlock *t, cap::CNode *dst, int slot) {
    cap::CNode *cspace = t->get_cspace();
    if (cspace && slot >= 0)
        cspace->remove(static_cast<uint32_t>(slot));
    if (dst)
        dst->release();
}

} // namespace

// Runmode: kernel
// Testidea: SYS_CAP_GRANT installs the source capability into the
//           destination CNode (addressed by a CapCNode handle).
// Input: task sets up its CSpace + a dest CNode cap + a task target cap;
//        dispatches SYS_CAP_GRANT
// Expect: syscall returns the new opaque handle (issue #266: directly
// usable, never a raw slot index); dest CNode holds the target
// Depends: kernel::Syscall, kernel::cap
JARVIS_TEST(sys_cap_grant_dispatch, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_dst_occ = 0;

    g_ret = 0;
    g_dst_occ = 0;

    auto *t = run_cap_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cspace = cur->get_cspace();
        auto *tgt =
            static_cast<TestTarget *>(MemPool::alloc(sizeof(TestTarget)));
        if (!tgt)
            return;
        new (tgt) TestTarget;
        tgt->mark_pool_backed();
        kernel::test::ResourceTracker::instance().track_cap_object_add();

        int ss = cspace->install(tgt, cap::CapType::Task,
                                 cap::CapRights::READ | cap::CapRights::GRANT);
        cap::CNode *dst = nullptr;
        int ds = -1;
        install_dest_cspace(cur, &dst, &ds);
        if (ss < 0 || ds < 0) {
            tgt->release();
            return;
        }
        uint64_t sh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ss),
                               cspace->slot_gen(static_cast<uint32_t>(ss)));
        uint64_t dh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ds),
                               cspace->slot_gen(static_cast<uint32_t>(ds)));
        g_ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::CAP_GRANT),
                                sh, dh, 0, 0, nullptr);
        g_dst_occ = cap::occupied_count(dst);
        teardown_dest_cspace(cur, dst, ds);
        cspace->remove(static_cast<uint32_t>(ss));
        tgt->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(g_ret != static_cast<uint64_t>(-1));
    JARVIS_ASSERT_EQ(1ULL, g_dst_occ);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_CAP_COPY duplicates the capability into the destination.
// Input: task sets up CSpace + dest CNode cap + target cap; dispatches
//        SYS_CAP_COPY
// Expect: syscall returns the new opaque handle (issue #266: directly
// usable, never a raw slot index); dest CNode holds the target
// Depends: kernel::Syscall, kernel::cap
JARVIS_TEST(sys_cap_copy_dispatch, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_dst_occ = 0;

    g_ret = 0;
    g_dst_occ = 0;

    auto *t = run_cap_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cspace = cur->get_cspace();
        auto *tgt =
            static_cast<TestTarget *>(MemPool::alloc(sizeof(TestTarget)));
        if (!tgt)
            return;
        new (tgt) TestTarget;
        tgt->mark_pool_backed();
        kernel::test::ResourceTracker::instance().track_cap_object_add();

        int ss = cspace->install(tgt, cap::CapType::Task,
                                 cap::CapRights::READ | cap::CapRights::COPY);
        cap::CNode *dst = nullptr;
        int ds = -1;
        install_dest_cspace(cur, &dst, &ds);
        if (ss < 0 || ds < 0) {
            tgt->release();
            return;
        }
        uint64_t sh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ss),
                               cspace->slot_gen(static_cast<uint32_t>(ss)));
        uint64_t dh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ds),
                               cspace->slot_gen(static_cast<uint32_t>(ds)));
        g_ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::CAP_COPY),
                                sh, dh, 0, 0, nullptr);
        g_dst_occ = cap::occupied_count(dst);
        teardown_dest_cspace(cur, dst, ds);
        cspace->remove(static_cast<uint32_t>(ss));
        tgt->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(g_ret != static_cast<uint64_t>(-1));
    JARVIS_ASSERT_EQ(1ULL, g_dst_occ);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_CAP_DELETE removes the source slot (N6, F2 #555). The
// "release my cap" half of the old revoke contract moved here: install a
// target cap, DELETE it, expect return 0 and no occupied slots. DELETE never
// marks the target revoked (caller ref still held => object alive,
// revoked()==0).
// Input: task installs a target cap; dispatches SYS_CAP_DELETE
// Expect: syscall returns 0; CSpace has no occupied slots; target NOT revoked
// Depends: kernel::Syscall, kernel::cap
JARVIS_TEST(sys_cap_revoke_dispatch, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_occ = 0;
    static uint64_t g_revoked = 0;

    g_ret = 0;
    g_occ = 0;
    g_revoked = 0;

    auto *t = run_cap_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cspace = cur->get_cspace();
        auto *tgt =
            static_cast<TestTarget *>(MemPool::alloc(sizeof(TestTarget)));
        if (!tgt)
            return;
        new (tgt) TestTarget;
        tgt->mark_pool_backed();
        kernel::test::ResourceTracker::instance().track_cap_object_add();

        int ss = cspace->install(tgt, cap::CapType::Task, cap::CapRights::READ);
        if (ss < 0) {
            tgt->release();
            return;
        }
        uint64_t sh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ss),
                               cspace->slot_gen(static_cast<uint32_t>(ss)));
        g_ret =
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::CAP_DELETE),
                            sh, 0, 0, 0, nullptr);
        g_occ = cap::occupied_count(cspace);
        g_revoked = tgt->revoked() ? 1 : 0;
        tgt->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_ASSERT_EQ(0ULL, g_occ);
    JARVIS_ASSERT_EQ(0ULL, g_revoked);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Issue #266 — syscall-produced handles are opaque encoded values
// usable with no manual encoding: frame_create's return feeds revoke
// directly. Pre-fix the producers returned raw slot indices, which lookup/
// revoke reject (cspace/gen mismatch) — the round trip failed.
// Input: SYS_FRAME_CREATE(1) -> take the returned handle as-is ->
// SYS_CAP_DELETE(handle).
// Expect: create returns non-minus-1; delete of the returned handle is 0
// (slot actually gone afterwards).
// Depends: kernel::Syscall, kernel::cap encode/lookup/delete contract.
JARVIS_TEST(sys_cap_returned_handle_roundtrip, "PRE: none | POST: none") {
    static uint64_t g_created = 0;
    static uint64_t g_revoked = 0;
    static uint64_t g_occ = 0;

    g_created = 0;
    g_revoked = 0;
    g_occ = 0;

    auto *t = run_cap_task([]() {
        g_created = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::FRAME_CREATE), 1, 0, 0, 0,
            nullptr);
        if (g_created == static_cast<uint64_t>(-1))
            return;
        g_revoked = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_DELETE), g_created, 0,
            0, 0, nullptr);
        auto *cur = Scheduler::current_task();
        g_occ = (cur != nullptr && cur->get_cspace() != nullptr)
                    ? cap::occupied_count(cur->get_cspace())
                    : 999;
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(g_created != static_cast<uint64_t>(-1));
    JARVIS_ASSERT_EQ(0ULL, g_revoked);
    JARVIS_ASSERT_EQ(0ULL, g_occ);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_CAP_MINT copies the capability with a reduced rights mask.
// Input: task installs READ|WRITE cap; dispatches SYS_CAP_MINT with WRITE
//        only into the dest CNode
// Expect: syscall returns the opaque handle (issue #266); dest slot carries
// only WRITE
// Depends: kernel::Syscall, kernel::cap
JARVIS_TEST(sys_cap_mint_dispatch, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_dst_occ = 0;

    g_ret = 0;
    g_dst_occ = 0;

    auto *t = run_cap_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cspace = cur->get_cspace();
        auto *tgt =
            static_cast<TestTarget *>(MemPool::alloc(sizeof(TestTarget)));
        if (!tgt)
            return;
        new (tgt) TestTarget;
        tgt->mark_pool_backed();
        kernel::test::ResourceTracker::instance().track_cap_object_add();

        int ss = cspace->install(tgt, cap::CapType::Task,
                                 cap::CapRights::READ | cap::CapRights::WRITE |
                                     cap::CapRights::COPY);
        cap::CNode *dst = nullptr;
        int ds = -1;
        install_dest_cspace(cur, &dst, &ds);
        if (ss < 0 || ds < 0) {
            tgt->release();
            return;
        }
        uint64_t sh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ss),
                               cspace->slot_gen(static_cast<uint32_t>(ss)));
        uint64_t dh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ds),
                               cspace->slot_gen(static_cast<uint32_t>(ds)));
        g_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_MINT), sh, dh,
            static_cast<uint64_t>(cap::CapRights::WRITE), 0, nullptr);
        g_dst_occ = cap::occupied_count(dst);
        teardown_dest_cspace(cur, dst, ds);
        cspace->remove(static_cast<uint32_t>(ss));
        tgt->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(g_ret != static_cast<uint64_t>(-1));
    JARVIS_ASSERT_EQ(1ULL, g_dst_occ);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #171 P4 creator-seeding: a fresh task owns no caps; everything
//        in a seeded CSpace arrives through an explicit grant the creator
//        performs itself (mirroring what a creator does before first
//        dispatch — the loader grants nothing, exec inherits, see the
//        finalize_loaded_task / exec_into_current rule comments).
// Input: fresh create_user task asserts null CSpace; creator installs a
//        dest CNode + grants a Task cap via SYS_CAP_GRANT; sibling fresh
//        task asserts null CSpace (no cross-task leakage).
// Expect: newborn cspace null; grant returns a usable opaque handle;
//        dest holds exactly the granted cap (occupied_count == 1,
//        lookup resolves the granted target); sibling still null.
// Depends: finalize_loaded_task (no seeding), ensure_cspace (lazy),
//          SYS_CAP_GRANT dispatch, cap::lookup.
JARVIS_TEST(sys_cap_creator_seeding_exact, "PRE: none | POST: none") {
    static uint64_t g_grant = 0;
    static uint64_t g_occ = 0;
    static bool g_resolved = false;
    static bool g_sibling_null = false;

    g_grant = 0;
    g_occ = 0;
    g_resolved = false;
    g_sibling_null = false;

    auto *t = run_cap_task([]() {
        auto *cur = Scheduler::current_task();
        // Birth state (P4 binding rule): the loader seeds nothing, the
        // CSpace is null until first use.
        if (cur->get_cspace() != nullptr)
            return;
        cur->ensure_cspace();
        cap::CNode *cspace = cur->get_cspace();
        auto *tgt =
            static_cast<TestTarget *>(MemPool::alloc(sizeof(TestTarget)));
        if (!tgt)
            return;
        new (tgt) TestTarget;
        tgt->mark_pool_backed();
        kernel::test::ResourceTracker::instance().track_cap_object_add();

        int ss = cspace->install(tgt, cap::CapType::Task,
                                 cap::CapRights::READ | cap::CapRights::GRANT);
        cap::CNode *dst = nullptr;
        int ds = -1;
        install_dest_cspace(cur, &dst, &ds);
        if (ss < 0 || ds < 0) {
            tgt->release();
            return;
        }
        uint64_t sh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ss),
                               cspace->slot_gen(static_cast<uint32_t>(ss)));
        uint64_t dh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ds),
                               cspace->slot_gen(static_cast<uint32_t>(ds)));
        // The creator's explicit grant is the only way the dest CSpace
        // gains a cap (loader magic: none).
        g_grant = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_GRANT), sh, dh, 0, 0,
            nullptr);
        g_occ = cap::occupied_count(dst);
        KernelObject *pinned = cap::lookup(
            dst, g_grant, cap::CapType::Task, cap::CapRights::READ);
        g_resolved = (pinned == static_cast<KernelObject *>(tgt));
        if (pinned)
            pinned->release();
        teardown_dest_cspace(cur, dst, ds);
        cspace->remove(static_cast<uint32_t>(ss));
        tgt->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(g_grant != static_cast<uint64_t>(-1));
    JARVIS_ASSERT_EQ(1ULL, g_occ);
    JARVIS_ASSERT(g_resolved);

    // A sibling newborn owns nothing: no cross-task leakage.
    auto *t2 = run_cap_task([]() {
        auto *cur = Scheduler::current_task();
        g_sibling_null = (cur->get_cspace() == nullptr);
    });
    JARVIS_ASSERT(t2 != nullptr);
    JARVIS_ASSERT(g_sibling_null);
    JARVIS_TEST_PASS();
}


// Runmode: kernel
// Testidea: A bad (out-of-range / stale) handle makes the syscall return -1.
// Input: task dispatches SYS_CAP_REVOKE with slot == CONFIG_CSLOT_COUNT
// Expect: -1, no crash, CSpace unchanged
// Depends: kernel::Syscall, kernel::cap
JARVIS_TEST(sys_cap_bad_handle_returns_minus1, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;

    g_ret = 0;

    auto *t = run_cap_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cspace = cur->get_cspace();
        uint64_t bad = cap::encode_handle(cspace->cspace_id, 0, 0);
        g_ret =
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::CAP_REVOKE),
                            bad, 0, 0, 0, nullptr);
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A handle of the wrong type is rejected by the syscall.
// Input: task installs a Task cap; dispatches SYS_CAP_GRANT treating it as a
//        CNode destination
// Expect: -1 (type mismatch on the destination lookup)
// Depends: kernel::Syscall, kernel::cap
JARVIS_TEST(sys_cap_wrong_type_returns_minus1, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;

    g_ret = 0;

    auto *t = run_cap_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cspace = cur->get_cspace();
        auto *tgt =
            static_cast<TestTarget *>(MemPool::alloc(sizeof(TestTarget)));
        if (!tgt)
            return;
        new (tgt) TestTarget;
        tgt->mark_pool_backed();
        kernel::test::ResourceTracker::instance().track_cap_object_add();

        int ss = cspace->install(tgt, cap::CapType::Task, cap::CapRights::READ);
        if (ss < 0) {
            tgt->release();
            return;
        }
        uint64_t sh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ss),
                               cspace->slot_gen(static_cast<uint32_t>(ss)));
        // dst_handle aliases the same Task cap -> destination type mismatch.
        g_ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::CAP_GRANT),
                                sh, sh, 0, 0, nullptr);
        cspace->remove(static_cast<uint32_t>(ss));
        tgt->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A capability lacking the required right is rejected.
// Input: task installs a Task cap WITHOUT COPY; dispatches SYS_CAP_COPY
// Expect: -1 (CapRights::COPY missing on destination)
// Depends: kernel::Syscall, kernel::cap
JARVIS_TEST(sys_cap_rights_denied_returns_minus1, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;

    g_ret = 0;

    auto *t = run_cap_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cspace = cur->get_cspace();
        auto *tgt =
            static_cast<TestTarget *>(MemPool::alloc(sizeof(TestTarget)));
        if (!tgt)
            return;
        new (tgt) TestTarget;
        tgt->mark_pool_backed();
        kernel::test::ResourceTracker::instance().track_cap_object_add();

        int ss = cspace->install(tgt, cap::CapType::Task, cap::CapRights::READ);
        if (ss < 0) {
            tgt->release();
            return;
        }
        uint64_t sh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ss),
                               cspace->slot_gen(static_cast<uint32_t>(ss)));
        // dst_handle aliases the Task cap (lacks COPY right).
        g_ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::CAP_COPY),
                                sh, sh, 0, 0, nullptr);
        cspace->remove(static_cast<uint32_t>(ss));
        tgt->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Revoking via the syscall leaves zero ResourceTracker delta.
// Input: task installs + revokes a cap via SYS_CAP_REVOKE, frees target
// Expect: cap_objects/cap_slots back to baseline
// Depends: kernel::Syscall, kernel::cap
JARVIS_TEST(sys_cap_revoke_cleanup_zero_delta, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    auto *t = run_cap_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cspace = cur->get_cspace();
        auto *tgt =
            static_cast<TestTarget *>(MemPool::alloc(sizeof(TestTarget)));
        if (!tgt)
            return;
        new (tgt) TestTarget;
        tgt->mark_pool_backed();
        kernel::test::ResourceTracker::instance().track_cap_object_add();

        int ss = cspace->install(tgt, cap::CapType::Task, cap::CapRights::READ);
        if (ss < 0) {
            tgt->release();
            return;
        }
        uint64_t sh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ss),
                               cspace->slot_gen(static_cast<uint32_t>(ss)));
        Syscall::handle(static_cast<uint64_t>(SyscallNumber::CAP_REVOKE), sh, 0,
                        0, 0, nullptr);
        tgt->release();
    });
    JARVIS_ASSERT(t != nullptr);

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #171 P3 grant/copy/revoke lifecycle through the syscall
//        boundary under F2 semantics (#555): CAP_REVOKE destroys the
//        derivations of the named slot (the granted + copied handles die)
//        while the NAMED slot survives; CAP_DELETE would remove the slot.
//        A granted handle copies; after REVOKE the named source still
//        copies, the killed derivation fails closed on use, and a second
//        REVOKE succeeds idempotently (nothing left, named valid).
// Input: task installs a Task cap + dest CNode; dispatches GRANT, COPY,
//        REVOKE of the source slot, COPY-after-revoke (source alive),
//        USE-after-revoke of the killed derivation, REVOKE-again via
//        Syscall::handle.
// Expect: grant/copy return opaque handles (!= -1); revoke returns 0;
//        post-revoke source copy succeeds; killed-derivation use is -1;
//        second revoke returns 0.
// Depends: kernel::Syscall, kernel::cap
JARVIS_TEST(sys_cap_grant_copy_revoke_lifecycle, "PRE: none | POST: none") {
    static uint64_t g_grant = 0;
    static uint64_t g_copy = 0;
    static uint64_t g_revoke = 0;
    static uint64_t g_copy_after = 0;
    static uint64_t g_grant_dead = 0;
    static uint64_t g_revoke_again = 0;

    g_grant = g_copy = g_revoke = 0;
    g_copy_after = g_grant_dead = g_revoke_again = 0;

    auto *t = run_cap_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cspace = cur->get_cspace();
        auto *tgt =
            static_cast<TestTarget *>(MemPool::alloc(sizeof(TestTarget)));
        if (!tgt)
            return;
        new (tgt) TestTarget;
        tgt->mark_pool_backed();
        kernel::test::ResourceTracker::instance().track_cap_object_add();

        int ss = cspace->install(tgt, cap::CapType::Task,
                                 cap::CapRights::READ | cap::CapRights::GRANT |
                                     cap::CapRights::COPY);
        cap::CNode *dst = nullptr;
        int ds = -1;
        install_dest_cspace(cur, &dst, &ds);
        if (ss < 0 || ds < 0) {
            tgt->release();
            return;
        }
        uint64_t sh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ss),
                               cspace->slot_gen(static_cast<uint32_t>(ss)));
        uint64_t dh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ds),
                               cspace->slot_gen(static_cast<uint32_t>(ds)));
        g_grant = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_GRANT), sh, dh, 0, 0,
            nullptr);
        g_copy = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_COPY), sh, dh, 0, 0,
            nullptr);
        // REVOKE the source: derivations (granted + copied handles) die,
        // the named source slot survives.
        g_revoke = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_REVOKE), sh, 0, 0, 0,
            nullptr);
        // Copy-after-revoke from the surviving source succeeds. The
        // derivation destroy freed the dest slot (gen moved on), so the
        // pre-revoke dh handle is stale — re-encode it fresh.
        dh = cap::encode_handle(
            cspace->cspace_id, static_cast<uint32_t>(ds),
            cspace->slot_gen(static_cast<uint32_t>(ds)));
        g_copy_after = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_COPY), sh, dh, 0, 0,
            nullptr);
        // Use-after-revoke of the killed derivation fails closed: g_grant
        // names a destroyed slot (gen mismatch) — revoke-by-stale-handle
        // is -1 via the frozen face.
        g_grant_dead = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_REVOKE), g_grant, 0, 0,
            0, nullptr);
        // Second revoke of the same (still-valid, derivation-free) slot
        // succeeds idempotently.
        g_revoke_again = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_REVOKE), sh, 0, 0, 0,
            nullptr);
        teardown_dest_cspace(cur, dst, ds);
        cspace->remove(static_cast<uint32_t>(ss));
        tgt->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(g_grant != static_cast<uint64_t>(-1));
    JARVIS_ASSERT(g_copy != static_cast<uint64_t>(-1));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), g_revoke);
    JARVIS_ASSERT(g_copy_after != static_cast<uint64_t>(-1));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_grant_dead);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), g_revoke_again);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #171 P3 table-full fails closed with a code, never hangs or
//        ENSUREs. Filling the destination CNode makes the next grant
//        return -1; freeing one slot recovers a valid handle.
// Input: task installs a Task cap + dest CNode; fills all 64 dest
//        slots; dispatches GRANT (expect -1), removes one slot,
//        dispatches GRANT again (expect a handle).
// Expect: -1 while full; opaque handle after one free slot; no residue
//        (every fill target released, dest CNode torn down).
// Depends: kernel::Syscall, kernel::cap
JARVIS_TEST(sys_cap_grant_dst_full_returns_minus1, "PRE: none | POST: none") {
    static uint64_t g_full = 0;
    static uint64_t g_recovered = 0;

    g_full = 0;
    g_recovered = 0;

    auto *t = run_cap_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cspace = cur->get_cspace();
        auto *tgt =
            static_cast<TestTarget *>(MemPool::alloc(sizeof(TestTarget)));
        if (!tgt)
            return;
        new (tgt) TestTarget;
        tgt->mark_pool_backed();
        kernel::test::ResourceTracker::instance().track_cap_object_add();

        int ss = cspace->install(tgt, cap::CapType::Task,
                                 cap::CapRights::READ |
                                     cap::CapRights::GRANT);
        cap::CNode *dst = nullptr;
        int ds = -1;
        install_dest_cspace(cur, &dst, &ds);
        if (ss < 0 || ds < 0) {
            tgt->release();
            return;
        }
        uint64_t sh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ss),
                               cspace->slot_gen(static_cast<uint32_t>(ss)));
        uint64_t dh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ds),
                               cspace->slot_gen(static_cast<uint32_t>(ds)));
        TestTarget *fills[64] = {};
        uint32_t filled = 0;
        for (uint32_t i = 0;
             i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT); ++i) {
            auto *fill =
                static_cast<TestTarget *>(MemPool::alloc(sizeof(TestTarget)));
            if (!fill)
                break;
            new (fill) TestTarget;
            fill->mark_pool_backed();
            kernel::test::ResourceTracker::instance().track_cap_object_add();
            int fs = dst->install(fill, cap::CapType::Task,
                                  cap::CapRights::READ);
            if (fs < 0) {
                fill->release();
                kernel::test::ResourceTracker::instance()
                    .track_cap_object_remove();
                break;
            }
            fills[filled++] = fill;
        }
        g_full = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_GRANT), sh, dh, 0, 0,
            nullptr);
        if (filled > 0) {
            dst->remove(0);
            fills[0]->release();
            fills[0] = nullptr;
            g_recovered = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::CAP_GRANT), sh, dh, 0,
                0, nullptr);
        }
        for (uint32_t i = 1; i < filled; ++i) {
            dst->remove(i);
            fills[i]->release();
        }
        teardown_dest_cspace(cur, dst, ds);
        cspace->remove(static_cast<uint32_t>(ss));
        tgt->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_full);
    JARVIS_ASSERT(g_recovered != static_cast<uint64_t>(-1));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all SYS_CAP_* syscall dispatch tests.
// Runmode: kernel
// Testidea: F2 S2-1 — a revoke exceeding the 64-edge per-call budget
//        returns typed INCOMPLETE (syscall face: fail) and resumes on
//        re-invocation; the named slot stays live throughout.
// Input: 124 derived copies (2 dests x 62); REVOKE, REVOKE.
// Expect: call 1 => fail + INCOMPLETE + survivors; call 2 => >= 0,
//        zero remaining, all derived handles stale, named usable.
// Depends: revoke_derivations budget, SYS_CAP_REVOKE face
JARVIS_TEST(cap_revoke_bounded_partial_resumable, "PRE: none | POST: none") {
    static uint64_t g_first = 0;
    static uint64_t g_first_err = 0;
    static uint64_t g_survivors = 0;
    static uint64_t g_second = 0;
    static uint64_t g_remaining = 0;

    g_first = g_first_err = g_survivors = g_second = g_remaining = 0;

    auto *t = run_cap_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cspace = cur->get_cspace();
        auto *tgt =
            static_cast<TestTarget *>(MemPool::alloc(sizeof(TestTarget)));
        if (!tgt)
            return;
        new (tgt) TestTarget;
        tgt->mark_pool_backed();
        kernel::test::ResourceTracker::instance().track_cap_object_add();

        int ss = cspace->install(tgt, cap::CapType::Task,
                                 cap::CapRights::READ | cap::CapRights::COPY);
        cap::CNode *dst1 = nullptr, *dst2 = nullptr;
        int ds1 = -1, ds2 = -1;
        install_dest_cspace(cur, &dst1, &ds1);
        install_dest_cspace(cur, &dst2, &ds2);
        if (ss < 0 || !dst1 || !dst2) {
            tgt->release();
            return;
        }
        uint64_t sh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ss),
                               cspace->slot_gen(static_cast<uint32_t>(ss)));
        for (int i = 0; i < 62; ++i) {
            if (cap::copy(cspace, sh, dst1) < 0)
                break;
            if (cap::copy(cspace, sh, dst2) < 0)
                break;
        }
        g_survivors = cap::occupied_count(dst1) + cap::occupied_count(dst2);
        // Call 1: exceeds the 64-edge budget => INCOMPLETE via syscall face.
        g_first = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_REVOKE), sh, 0, 0, 0,
            nullptr);
        // Named slot still live and usable across the incomplete call.
        KernelObject *pinned = cap::lookup(cspace, sh, cap::CapType::Task,
                                           cap::CapRights::READ);
        g_first_err = (pinned != nullptr) ? 1 : 0;
        if (pinned)
            pinned->release();
        // Call 2: resumes to completion.
        g_second = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_REVOKE), sh, 0, 0, 0,
            nullptr);
        g_remaining =
            cap::occupied_count(dst1) + cap::occupied_count(dst2);
        teardown_dest_cspace(cur, dst1, ds1);
        teardown_dest_cspace(cur, dst2, ds2);
        cspace->remove(static_cast<uint32_t>(ss));
        tgt->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(124ULL, g_survivors);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_first);
    JARVIS_ASSERT_EQ(1ULL, g_first_err);
    JARVIS_ASSERT(g_second != static_cast<uint64_t>(-1));
    JARVIS_ASSERT_EQ(0ULL, g_remaining);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F2 S2-2 — revoke with a stale named handle (slot deleted and
//        recycled) fails closed BAD_HANDLE; nothing is removed anywhere.
// Input: install, DELETE, re-install (gen moves), REVOKE old handle.
// Expect: -1; occupied counts unchanged; new object alive.
// Depends: revoke_derivations validate-and-pin (INV-DERIV-10)
JARVIS_TEST(cap_revoke_stale_named_slot_fails_closed,
            "PRE: none | POST: none") {
    static uint64_t g_revoke = 0;
    static uint64_t g_occ = 0;
    static uint64_t g_live = 0;

    g_revoke = g_occ = g_live = 0;

    auto *t = run_cap_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cspace = cur->get_cspace();
        auto *tgt =
            static_cast<TestTarget *>(MemPool::alloc(sizeof(TestTarget)));
        if (!tgt)
            return;
        new (tgt) TestTarget;
        tgt->mark_pool_backed();
        kernel::test::ResourceTracker::instance().track_cap_object_add();

        int ss = cspace->install(tgt, cap::CapType::Task,
                                 cap::CapRights::READ);
        if (ss < 0) {
            tgt->release();
            return;
        }
        uint64_t stale = cap::encode_handle(
            cspace->cspace_id, static_cast<uint32_t>(ss),
            cspace->slot_gen(static_cast<uint32_t>(ss)));
        // Delete + recycle the slot so the generation moves on.
        cspace->remove(static_cast<uint32_t>(ss));
        tgt->release();
        auto *tgt2 =
            static_cast<TestTarget *>(MemPool::alloc(sizeof(TestTarget)));
        if (!tgt2)
            return;
        new (tgt2) TestTarget;
        tgt2->mark_pool_backed();
        kernel::test::ResourceTracker::instance().track_cap_object_add();
        int ss2 = cspace->install(tgt2, cap::CapType::Task,
                                  cap::CapRights::READ);
        if (ss2 < 0) {
            tgt2->release();
            return;
        }
        g_revoke = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_REVOKE), stale, 0, 0,
            0, nullptr);
        g_occ = cap::occupied_count(cspace);
        uint64_t fresh = cap::encode_handle(
            cspace->cspace_id, static_cast<uint32_t>(ss2),
            cspace->slot_gen(static_cast<uint32_t>(ss2)));
        KernelObject *live = cap::lookup(cspace, fresh, cap::CapType::Task,
                                          cap::CapRights::READ);
        g_live = (live != nullptr && live == tgt2) ? 1 : 0;
        if (live)
            live->release();
        cspace->remove(static_cast<uint32_t>(ss2));
        tgt2->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_revoke);
    JARVIS_ASSERT_EQ(1ULL, g_occ);
    JARVIS_ASSERT_EQ(1ULL, g_live);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F2 S2-3 — retype with a saturated derivation table fails
//        DERIVATION_FULL with no residue: the 4 pages return to PMM exactly
//        once (prefix via target release, remainder via bounded loop), no
//        live child Untyped exists, the parent stays spent (second retype
//        fails ALREADY_RETYPED without moving pages).
// Input: fill table (4 dests x 64 copies = 256 edges); SYS_CAP_RETYPE.
// Expect: -1; PMM +4 exactly; exactly one Untyped (spent parent); occ same.
// Depends: retype record-failure rollback (INV-DERIV-3)
JARVIS_TEST(cap_retype_record_full_rollback, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_occ_before = 0;
    static uint64_t g_occ_after = 0;
    static uint64_t g_untypeds = 0;
    static uint64_t g_pmm_delta = 0;
    static uint64_t g_spent = 0;

    g_ret = g_occ_before = g_occ_after = g_untypeds = g_pmm_delta = g_spent =
        0;

    auto *t = run_cap_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cspace = cur->get_cspace();
        auto *tgt =
            static_cast<TestTarget *>(MemPool::alloc(sizeof(TestTarget)));
        if (!tgt)
            return;
        new (tgt) TestTarget;
        tgt->mark_pool_backed();
        kernel::test::ResourceTracker::instance().track_cap_object_add();

        int ss = cspace->install(tgt, cap::CapType::Task,
                                 cap::CapRights::READ | cap::CapRights::COPY);
        if (ss < 0) {
            tgt->release();
            return;
        }
        uint64_t sh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ss),
                               cspace->slot_gen(static_cast<uint32_t>(ss)));
        cap::CNode *dsts[4] = {};
        int dss[4] = {-1, -1, -1, -1};
        for (int k = 0; k < 4; ++k)
            install_dest_cspace(cur, &dsts[k], &dss[k]);
        for (int k = 0; k < 4; ++k) {
            if (!dsts[k])
                return;
            for (int i = 0; i < 64; ++i) {
                if (cap::copy(cspace, sh, dsts[k]) < 0)
                    break;
            }
        }
        // 4-page Untyped parent for the doomed retype.
        auto *ut = cap::UntypedMem::create(4 * arch::PAGE_SIZE, false);
        if (!ut)
            return;
        int us = cspace->install(ut, cap::CapType::Untyped,
                                 cap::CapRights::READ | cap::CapRights::WRITE);
        if (us < 0) {
            ut->release();
            return;
        }
        uint64_t uh = cap::encode_handle(
            cspace->cspace_id, static_cast<uint32_t>(us),
            cspace->slot_gen(static_cast<uint32_t>(us)));
        g_occ_before = cap::occupied_count(cspace);
        const uint64_t free_before = PMM::free_pages_ref();
        g_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_RETYPE), uh,
            static_cast<uint64_t>(cap::CapType::Frame),
            static_cast<uint64_t>(arch::PAGE_SIZE),
            static_cast<uint64_t>(cap::CapRights::READ), nullptr);
        // All 4 pages return to PMM exactly once (parent spent, owns none).
        g_pmm_delta = PMM::free_pages_ref() - free_before;
        g_occ_after = cap::occupied_count(cspace);
        // The spent parent rejects a second retype without moving pages.
        errors::CapError spent_err = errors::CAP_ERR_OK;
        int retry = cap::retype(cspace, uh, cap::CapType::Frame,
                                arch::PAGE_SIZE,
                                cap::CapRights::READ, nullptr, &spent_err);
        g_spent = (retry < 0 &&
                   spent_err == errors::CAP_ERR_ALREADY_RETYPED)
                      ? 1
                      : 0;
        // No live child Untyped may exist: exactly the parent remains.
        uint64_t n_untyped = 0;
        for (uint32_t i = 0; i < CONFIG_CSLOT_COUNT; ++i) {
            if (cspace->peek(i, cap::CapType::Untyped) != nullptr)
                ++n_untyped;
        }
        g_untypeds = n_untyped;
        for (int k = 0; k < 4; ++k)
            teardown_dest_cspace(cur, dsts[k], dss[k]);
        cspace->remove(static_cast<uint32_t>(us));
        ut->release();
        cspace->remove(static_cast<uint32_t>(ss));
        tgt->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_ret);
    JARVIS_ASSERT_EQ(4ULL, g_pmm_delta);
    JARVIS_ASSERT_EQ(1ULL, g_untypeds);
    JARVIS_ASSERT_EQ(g_occ_before, g_occ_after);
    JARVIS_ASSERT_EQ(1ULL, g_spent);
    JARVIS_TEST_PASS();
}

// Input: None
// Expect: All tests registered
// Depends: test framework
void register_cap_syscall_tests() {
    Logger::info("Registering SYS_CAP_* syscall dispatch tests");
    JARVIS_REGISTER_TEST(sys_cap_grant_dispatch);
    JARVIS_REGISTER_TEST(sys_cap_copy_dispatch);
    JARVIS_REGISTER_TEST(sys_cap_revoke_dispatch);
    JARVIS_REGISTER_TEST(sys_cap_mint_dispatch);
    JARVIS_REGISTER_TEST(sys_cap_bad_handle_returns_minus1);
    JARVIS_REGISTER_TEST(sys_cap_wrong_type_returns_minus1);
    JARVIS_REGISTER_TEST(sys_cap_rights_denied_returns_minus1);
    JARVIS_REGISTER_TEST(sys_cap_revoke_cleanup_zero_delta);
    JARVIS_REGISTER_TEST(sys_cap_returned_handle_roundtrip); // issue #266
    JARVIS_REGISTER_TEST(sys_cap_grant_copy_revoke_lifecycle); // #171 P3
    JARVIS_REGISTER_TEST(sys_cap_grant_dst_full_returns_minus1); // #171 P3
    JARVIS_REGISTER_TEST(sys_cap_creator_seeding_exact); // #171 P4
    JARVIS_REGISTER_TEST(cap_revoke_bounded_partial_resumable); // #555 F2
    JARVIS_REGISTER_TEST(cap_revoke_stale_named_slot_fails_closed); // #555 F2
    JARVIS_REGISTER_TEST(cap_retype_record_full_rollback); // #555 F2
}
