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
    auto *tcb = TaskControlBlock::create(task_path_noop_entry, 20,
                                         TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(tcb != nullptr);
    MemPool::pin_block(tcb);
    bool pinned_ok = false;
    {
        arch::IrqGuard guard{};
        path_reset();
        TaskControlBlock::destroy(tcb); // pinned: teardown skipped, block kept
        pinned_ok = path_validate(k_task_cleanup_pinned_expected);
        path_reset();
        MemPool::unpin_block(tcb);
        TaskControlBlock::destroy(tcb); // now frees the block
        path_reset();
    }
    JARVIS_ASSERT(pinned_ok);
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

} // namespace

void register_task_path_tests() {
    JARVIS_REGISTER_TEST(task_create_path_segment);
    JARVIS_REGISTER_TEST(task_cleanup_full_path_segment);
    JARVIS_REGISTER_TEST(task_cleanup_pinned_terminal);
    JARVIS_REGISTER_TEST(task_cleanup_skip_detected);
    JARVIS_REGISTER_TEST(task_path_registry_math);
}
