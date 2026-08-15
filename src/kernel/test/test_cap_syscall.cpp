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
#include <kernel/memory/kernel_object.hpp>
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
                            cap::CAP_RIGHT_GRANT | cap::CAP_RIGHT_COPY);
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
// Expect: syscall returns the new slot index; dest CNode holds the target
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
                                 cap::CAP_RIGHT_READ | cap::CAP_RIGHT_GRANT);
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
// Expect: syscall returns the new slot index; dest CNode holds the target
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
                                 cap::CAP_RIGHT_READ | cap::CAP_RIGHT_COPY);
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
// Testidea: SYS_CAP_REVOKE removes the source slot and revokes the target.
// Input: task installs a target cap; dispatches SYS_CAP_REVOKE
// Expect: syscall returns 0; CSpace has no occupied slots; target revoked
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

        int ss = cspace->install(tgt, cap::CapType::Task, cap::CAP_RIGHT_READ);
        if (ss < 0) {
            tgt->release();
            return;
        }
        uint64_t sh =
            cap::encode_handle(cspace->cspace_id, static_cast<uint32_t>(ss),
                               cspace->slot_gen(static_cast<uint32_t>(ss)));
        g_ret =
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::CAP_REVOKE),
                            sh, 0, 0, 0, nullptr);
        g_occ = cap::occupied_count(cspace);
        g_revoked = tgt->revoked() ? 1 : 0;
        tgt->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_ASSERT_EQ(0ULL, g_occ);
    JARVIS_ASSERT_EQ(1ULL, g_revoked);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_CAP_MINT copies the capability with a reduced rights mask.
// Input: task installs READ|WRITE cap; dispatches SYS_CAP_MINT with WRITE
//        only into the dest CNode
// Expect: syscall returns the slot index; dest slot carries only WRITE
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
                                 cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE |
                                     cap::CAP_RIGHT_COPY);
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
            static_cast<uint64_t>(cap::CAP_RIGHT_WRITE), 0, nullptr);
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

        int ss = cspace->install(tgt, cap::CapType::Task, cap::CAP_RIGHT_READ);
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
// Expect: -1 (CAP_RIGHT_COPY missing on destination)
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

        int ss = cspace->install(tgt, cap::CapType::Task, cap::CAP_RIGHT_READ);
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

        int ss = cspace->install(tgt, cap::CapType::Task, cap::CAP_RIGHT_READ);
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
// Testidea: Registers all SYS_CAP_* syscall dispatch tests.
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
}
