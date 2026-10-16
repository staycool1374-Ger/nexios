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

/// @file test_sched_dispatch_path.cpp
/// @brief Dispatch-path segment tests (issue #347 Slice B): reschedule()
///        arm + next_task() select segments driven for real; skip-one-step
///        negative via the debug fault-injection mask. switch_to_task()'s
///        apply segment is file-static — exercised by the suite (observe
///        only) and covered by inspection. Every driven call is unwound
///        (arm cleared, dequeued candidate re-enqueued, tasks drained).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler_path.hpp>
#include <kernel/core/path_tracer.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;
using namespace kernel::test;

namespace {

/// @brief Create + add two kernel tasks (low-prio current, high-prio peer).
/// @return True when both tasks are live and queued.
static bool make_dispatch_pair(TaskControlBlock *&out_current,
                               TaskControlBlock *&out_next) {
    // Priorities must stay within CONFIG_PRIORITY_CEILING (127) —
    // enqueue() indexes the level array directly (no clamp). Higher
    // number = higher priority (prio-11 starves prio-10, #242).
    out_current = TaskControlBlock::create(forever_entry, 10,
                                           TaskControlBlock::NO_PERIOD);
    out_next = TaskControlBlock::create(forever_entry, 100,
                                        TaskControlBlock::NO_PERIOD);
    if (out_current == nullptr || out_next == nullptr) {
        return false;
    }
    Scheduler::add_task(*out_current);
    Scheduler::add_task(*out_next);
    return true;
}

/// @brief Re-enqueue a candidate stolen by next_task() (never the idle task).
static void restore_dispatch_candidate(TaskControlBlock *picked) {
    if (picked != nullptr && picked != Scheduler::own_idle() &&
        TaskControlBlock::is_valid(picked)) {
        Scheduler::set_task_ready(*picked);
    }
}

static void test_sched_dispatch_arm_segment() {
    TaskControlBlock *t_cur = nullptr;
    TaskControlBlock *t_next = nullptr;
    JARVIS_ASSERT(make_dispatch_pair(t_cur, t_next));
    bool arm_chained = false;
    {
        ScopedCurrentTask cur(*t_cur);
        path_reset();
        Scheduler::reschedule();
        arm_chained = path_validate(k_dispatch_arm_expected);
        __atomic_store_n(&Scheduler::SwSlots::need_resched(), false,
                         __ATOMIC_RELEASE);
    }
    terminate_and_drain(*t_cur);
    terminate_and_drain(*t_next);
    path_reset();
    JARVIS_ASSERT(arm_chained);
}

static void test_sched_dispatch_select_segment() {
    TaskControlBlock *t_cur = nullptr;
    TaskControlBlock *t_next = nullptr;
    JARVIS_ASSERT(make_dispatch_pair(t_cur, t_next));
    bool select_chained = false;
    {
        ScopedCurrentTask cur(*t_cur);
        path_reset();
        TaskControlBlock *picked = Scheduler::next_task();
        select_chained = (picked != nullptr) &&
                         path_validate(k_dispatch_select_expected);
        restore_dispatch_candidate(picked);
        if (!t_cur->in_ready_queue_ && t_cur->state == TaskState::READY) {
            Scheduler::set_task_ready(*t_cur);
        }
    }
    terminate_and_drain(*t_cur);
    terminate_and_drain(*t_next);
    path_reset();
    JARVIS_ASSERT(select_chained);
}

static void test_sched_dispatch_skip_detected() {
#if defined(CONFIG_DEBUG)
    TaskControlBlock *t_cur = nullptr;
    TaskControlBlock *t_next = nullptr;
    JARVIS_ASSERT(make_dispatch_pair(t_cur, t_next));
    bool skip_detected = false;
    {
        ScopedCurrentTask cur(*t_cur);
        g_test_skip_dispatch_step = static_cast<uint64_t>(
            to_underlying(SchedDispatchSteps::Dequeue));
        path_reset();
        TaskControlBlock *picked = Scheduler::next_task();
        skip_detected =
            (picked != nullptr) &&
            !path_validate(k_dispatch_select_expected) &&
            path_validate(static_cast<uint64_t>(
                to_underlying(SchedDispatchSteps::Select)));
        g_test_skip_dispatch_step = 0;
        restore_dispatch_candidate(picked);
        if (!t_cur->in_ready_queue_ && t_cur->state == TaskState::READY) {
            Scheduler::set_task_ready(*t_cur);
        }
    }
    terminate_and_drain(*t_cur);
    terminate_and_drain(*t_next);
    path_reset();
    JARVIS_ASSERT(skip_detected);
#else
    // Release builds compile this TU too (mk/rules.mk SRC_CXX is
    // build-type independent) but g_test_skip_dispatch_step exists only
    // under CONFIG_DEBUG - the skip cannot be injected, so pass trivially
    // and keep the 4-test count row stable in both builds
    // (cf. test_release_build.cpp preprocessor-branch pattern).
    JARVIS_TEST_PASS();
#endif
}

static void test_sched_dispatch_registry_math() {
    JARVIS_ASSERT_EQ(1ULL << 0, static_cast<uint64_t>(
                                    to_underlying(SchedDispatchSteps::Arm)));
    JARVIS_ASSERT_EQ((1ULL << 1) ^ (1ULL << 2), k_dispatch_select_expected);
    JARVIS_ASSERT_EQ((1ULL << 3) ^ (1ULL << 4) ^ (1ULL << 5) ^ (1ULL << 6) ^
                         (1ULL << 7) ^ (1ULL << 8),
                     k_dispatch_apply_expected);
}

} // namespace

void register_sched_dispatch_path_tests() {
    JARVIS_REGISTER_TEST(sched_dispatch_arm_segment);
    JARVIS_REGISTER_TEST(sched_dispatch_select_segment);
    JARVIS_REGISTER_TEST(sched_dispatch_skip_detected);
    JARVIS_REGISTER_TEST(sched_dispatch_registry_math);
}
