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

/// @file test_task_path.cpp
/// @brief Task create/cleanup segment tests (issue #347 Slice C): full
///        create() path, full 14-block cleanup() path, pinned terminal,
///        skip-one-step negative, registry math. Drives real
///        create()/destroy() under IrqGuard so the timer ISR's dispatch
///        steps cannot pollute the shared per-CPU slot. Capture happens
///        before teardown/assert; every TCB is freed.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/task_path.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/core/path_tracer.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/arch/hal/irq_guard.hpp>

using namespace kernel;

namespace {

void task_path_noop_entry() {
    for (;;) {
        arch::pause();
    }
}

static void test_task_create_path_segment() {
    bool create_ok = false;
    {
        arch::IrqGuard guard{};
        path_reset();
        auto *tcb = TaskControlBlock::create(task_path_noop_entry, 20,
                                             TaskControlBlock::NO_PERIOD);
        create_ok = (tcb != nullptr) &&
                    path_validate(k_task_create_expected);
        if (tcb != nullptr) {
            TaskControlBlock::destroy(tcb);
        }
        path_reset();
    }
    JARVIS_ASSERT(create_ok);
}

static void test_task_cleanup_full_path_segment() {
    auto *tcb = TaskControlBlock::create(task_path_noop_entry, 20,
                                         TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(tcb != nullptr);
    bool cleanup_ok = false;
    {
        arch::IrqGuard guard{};
        path_reset();
        TaskControlBlock::destroy(tcb);
        cleanup_ok = path_validate(k_task_cleanup_expected);
        path_reset();
    }
    JARVIS_ASSERT(cleanup_ok);
}

static void test_task_cleanup_pinned_terminal() {
    // Issue #373 regression: the unpin+destroy leg must complete the
    // deferred resource teardown (kstack + PML4 + IPC objects) instead of
    // freeing the block intact. Capture the tracker baseline before
    // create so the post-teardown delta must be exactly zero.
    test::ResourceCounters case_before;
    test::ResourceTracker::instance().capture(case_before);
    auto *tcb = TaskControlBlock::create(task_path_noop_entry, 20,
                                         TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(tcb != nullptr);
    MemPool::pin_block(tcb);
    bool pinned_ok = false;
    {
        arch::IrqGuard guard{};
        // Tracker symmetry: destroy()'s teardown ends in
        // track_task_remove, which pairs with the track_task_add in
        // add_task. Drive a scheduler-managed TCB (added, never
        // dispatched: remove_task runs inside this same guard).
        Scheduler::add_task(*tcb);
        path_reset();
        TaskControlBlock::destroy(tcb); // pinned: teardown skipped, block kept
        pinned_ok = path_validate(k_task_cleanup_pinned_expected);
        path_reset();
        MemPool::unpin_block(tcb);
        TaskControlBlock::destroy(tcb); // completes teardown, frees block
        path_reset();
    }
    JARVIS_ASSERT(pinned_ok);
    test::ResourceCounters case_after;
    test::ResourceTracker::instance().capture(case_after);
    JARVIS_ASSERT_EQ(case_before.pmm_pages_used, case_after.pmm_pages_used);
    JARVIS_ASSERT_EQ(case_before.tasks, case_after.tasks);
    JARVIS_ASSERT_EQ(case_before.msg_queues, case_after.msg_queues);
    JARVIS_ASSERT_EQ(case_before.notifies, case_after.notifies);
    JARVIS_ASSERT_EQ(case_before.event_groups, case_after.event_groups);
}

static void test_task_destroy_after_full_teardown_is_noop() {
    // Issue #373 branch (c): destroy on a REAPED-after-full-teardown TCB
    // is a safe no-op (magic 0xDD skip) — never a re-teardown. The second
    // destroy must not fault, must not double-free, and must leave zero
    // tracker delta.
    test::ResourceCounters case_before;
    test::ResourceTracker::instance().capture(case_before);
    auto *tcb = TaskControlBlock::create(task_path_noop_entry, 20,
                                         TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(tcb != nullptr);
    {
        arch::IrqGuard guard{};
        // Tracker symmetry (see pinned test): add pairs with the
        // track_task_remove in teardown. Never dispatched: destroy
        // removes it inside this same guard.
        Scheduler::add_task(*tcb);
        TaskControlBlock::destroy(tcb); // full teardown, block freed
        TaskControlBlock::destroy(tcb); // no-op: already freed, skip
    }
    test::ResourceCounters case_after;
    test::ResourceTracker::instance().capture(case_after);
    JARVIS_ASSERT_EQ(case_before.pmm_pages_used, case_after.pmm_pages_used);
    JARVIS_ASSERT_EQ(case_before.tasks, case_after.tasks);
    JARVIS_ASSERT_EQ(case_before.msg_queues, case_after.msg_queues);
    JARVIS_ASSERT_EQ(case_before.notifies, case_after.notifies);
    JARVIS_ASSERT_EQ(case_before.event_groups, case_after.event_groups);
}

static void test_task_cleanup_skip_detected() {
    auto *tcb = TaskControlBlock::create(task_path_noop_entry, 20,
                                         TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(tcb != nullptr);
    bool skip_ok = true; // not applicable without the debug injection
    {
        arch::IrqGuard guard{};
#if defined(CONFIG_DEBUG)
        g_test_skip_task_step = static_cast<uint64_t>(
            to_underlying(TaskCleanupSteps::MemFree));
#endif
        path_reset();
        TaskControlBlock::destroy(tcb);
#if defined(CONFIG_DEBUG)
        const uint64_t missing_mem_free =
            path_expected(TaskCleanupSteps::Validate,
                          TaskCleanupSteps::WatchdogDisarm,
                          TaskCleanupSteps::ExitStats,
                          TaskCleanupSteps::Unregister,
                          TaskCleanupSteps::ParentUnbind,
                          TaskCleanupSteps::TimerDrain,
                          TaskCleanupSteps::SyncDetach,
                          TaskCleanupSteps::Notify,
                          TaskCleanupSteps::VfsDrain,
                          TaskCleanupSteps::PageTeardown,
                          TaskCleanupSteps::IpcDestroy,
                          TaskCleanupSteps::CapRelease,
                          TaskCleanupSteps::Finalize);
        skip_ok = !path_validate(k_task_cleanup_expected) &&
                  path_validate(missing_mem_free);
        g_test_skip_task_step = 0;
#endif
        path_reset();
    }
    JARVIS_ASSERT(skip_ok);
}

static void test_task_path_registry_math() {
    // Expected values are fold-computed (never hand-derived).
    JARVIS_ASSERT_EQ(
        path_expected(TaskCreateSteps::Alloc, TaskCreateSteps::ZeroInit,
                      TaskCreateSteps::StackAlloc, TaskCreateSteps::Pml4Clone,
                      TaskCreateSteps::FrameBuild),
        k_task_create_expected);
    JARVIS_ASSERT_EQ(
        path_expected(TaskCleanupSteps::Validate,
                      TaskCleanupSteps::WatchdogDisarm),
        k_task_cleanup_pinned_expected);
    // Non-empty and pairwise-distinct.
    JARVIS_ASSERT(k_task_create_expected != 0);
    JARVIS_ASSERT(k_task_cleanup_expected != 0);
    JARVIS_ASSERT(k_task_cleanup_expected != k_task_create_expected);
}

// Canonical teardown chain (issue #324 Tick 13): terminate_err →
// release_zombie → cleanup_step pop → free records Terminate, Release,
// DrainPop + all 14 cleanup blocks in order. Drives an unregistered task
// synchronously under IrqGuard (no dispatch, no ISR pollution).
static void test_task_teardown_canonical_chain() {
    auto *tcb = TaskControlBlock::create(task_path_noop_entry, 20,
                                         TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(tcb != nullptr);
    bool chain_ok = false;
    {
        arch::IrqGuard guard{};
        path_reset();
        const errors::SchedulerError err =
            Scheduler::terminate_err(*tcb, 0);
        Scheduler::cleanup_step();
        chain_ok = (err == errors::SCHED_ERR_OK) &&
                   path_validate(k_teardown_canonical_expected);
        path_reset();
    }
    JARVIS_ASSERT(chain_ok);
}

} // namespace

void register_task_path_tests() {
    JARVIS_REGISTER_TEST(task_create_path_segment);
    JARVIS_REGISTER_TEST(task_cleanup_full_path_segment);
    JARVIS_REGISTER_TEST(task_cleanup_pinned_terminal);
    JARVIS_REGISTER_TEST(task_destroy_after_full_teardown_is_noop);
    JARVIS_REGISTER_TEST(task_cleanup_skip_detected);
    JARVIS_REGISTER_TEST(task_teardown_canonical_chain);
    JARVIS_REGISTER_TEST(task_path_registry_math);
}
