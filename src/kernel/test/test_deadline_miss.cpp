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

/// @file test_deadline_miss.cpp
/// @brief Deadline miss detection tests — Phase 1: BLOCKED/WAITING coverage,
///        TERMINATED exclusion, and periodic re-arm.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): every test now dispatches a REAL
/// kernel task (prio ≥ 11) whose lambda GENUINELY overruns its real deadline
/// (busy-waits past `period_ticks` using the real timer) and then either
/// blocks on a real semaphore (staying live for the scan) or terminates.
/// The detection scan `Scheduler::scan_deadlines()` is the exact entry the
/// [deadline-mon] task executes — it observes a state reached through real
/// execution, never through direct field mutation.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/timer.hpp>

using namespace kernel;

/// @brief  Body shared by the "overrun then block" helpers: busy-wait past
///         the real deadline (2-tick period ⇒ 40 real ticks is ~20 periods
///         of genuine overrun), then block on the semaphore handed via
///         `user_data` so the task stays live (BLOCKED) when the scan runs.
static void overrun_then_block_body() {
    sync::Semaphore *gate = reinterpret_cast<sync::Semaphore *>(
        Scheduler::current_task()->user_data);
    uint64_t start = arch::Timer::ticks();
    while (arch::Timer::ticks() - start < 40)
        arch::pause();
    gate->wait();
    // INV-4: Semaphore::wait() sets BLOCKED then returns immediately (deferred
    // switch).  Spin on BLOCKED so the harness observes it before we would
    // self-terminate; the harness's gate.post() wakes us and we return.
    while (Scheduler::current_task()->state == TaskState::BLOCKED)
        arch::pause();
}

/// @brief  Create the overrun-then-block task and dispatch it for real.
/// @return Pointer to the live BLOCKED task, or nullptr on failure.
static TaskControlBlock *spawn_overrun_blocked(sync::Semaphore &gate) {
    auto *t = TaskControlBlock::create(overrun_then_block_body, 11, 2);
    if (t == nullptr)
        return nullptr;
    t->user_data = &gate;
    Scheduler::add_task(*t);
    // Defer the switch; the timer ISR dispatches the prio-11 task on the
    // next tick.  It busy-waits 40 real ticks (genuine overrun) then blocks
    // on the semaphore — at which point the harness resumes.
    Scheduler::reschedule();
    while (t->state != TaskState::BLOCKED)
        arch::pause();
    return t;
}

/// @brief  Teardown: wake the blocked task (real semaphore post), wait for
///         genuine termination, then release the TCB (mirrors
///         test_ipc_blocking.cpp).
static void release_overrun_blocked(TaskControlBlock *t, sync::Semaphore &gate) {
    gate.post();
    kernel::test::wait_for_termination_safe(t);
    kernel::test::terminate_and_drain(*t);
}

// Runmode: kernel
// Testidea: A task in BLOCKED state must still trigger deadline miss
// detection when its deadline passes.
// Input: Real kernel task (prio 11, period 2) is dispatched, genuinely
//        overruns its deadline (busy-waits 40 real ticks), then blocks on a
//        real semaphore.  The harness waits for the genuine block, then runs
//        the detection scan (the exact entry the [deadline-mon] task uses).
// Expect: deadline_missed==true, deadline_miss_count>=1.
TEST_CLASS(DeadlineMissWhileBlocked) {
    sync::Semaphore gate;
    gate.init(0, 1);

    auto *helper = spawn_overrun_blocked(gate);
    CT_ASSERT(helper != nullptr);
    CT_ASSERT(helper->state == TaskState::BLOCKED);

    // Genuine overrun: the real deadline (now+2 at create) is long past.
    CT_ASSERT(helper->deadline_ticks < arch::Timer::ticks());

    kernel::test::trigger_deadline_monitor_scan();

    // deadline_missed may be cleared by re-arm (P1b) — count is the stable
    // check.
    CT_ASSERT(helper->deadline_miss_count >= 1);

    release_overrun_blocked(helper, gate);
};

// Runmode: kernel
// Testidea: A TERMINATED task must NOT trigger deadline miss detection
// even if its deadline is in the past.
// Input: Real kernel task (prio 11, period 2) is dispatched, genuinely
//        overruns its deadline, then terminates via the trampoline (REAPED /
//        zombie — removed from the live scan set).  The detection scan runs.
// Expect: deadline_missed stays false, deadline_miss_count stays 0.
TEST_CLASS(DeadlineMissWhileTerminatedSkipped) {
    auto *helper = TaskControlBlock::create([]() {
        uint64_t start = arch::Timer::ticks();
        while (arch::Timer::ticks() - start < 40)
            arch::pause();
    }, 11, 2);
    CT_ASSERT(helper != nullptr);
    Scheduler::add_task(*helper);

    auto *original = Scheduler::current_task();
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(helper);

    // Task genuinely terminated; its deadline (now+2) has long passed.
    uint64_t count_before = helper->deadline_miss_count;
    kernel::test::trigger_deadline_monitor_scan();

    // Must NOT fire for TERMINATED tasks.
    CT_ASSERT(helper->deadline_miss_count == count_before);

    Scheduler::set_current(*original);
    kernel::test::terminate_and_drain(*helper);
};

// Runmode: kernel
// Testidea: For periodic tasks, after a deadline miss the detection
// block re-arms deadline_ticks += period_ticks and clears deadline_missed.
// Input: Real kernel task (prio 11, period 2) genuinely overruns and blocks.
//        The detection scan fires once; the re-arm advances the deadline.
// Expect: deadline_miss_count>=1; deadline_ticks advanced by period_ticks;
// deadline_missed cleared (re-armed for the next period).
TEST_CLASS(DeadlineRearmOnPeriodRollover) {
    sync::Semaphore gate;
    gate.init(0, 1);

    auto *helper = spawn_overrun_blocked(gate);
    CT_ASSERT(helper != nullptr);

    uint64_t deadline_before = helper->deadline_ticks;
    uint64_t period = helper->period_ticks;
    CT_ASSERT(period > 0);

    kernel::test::trigger_deadline_monitor_scan();

    // Deadline miss fired.
    CT_ASSERT(helper->deadline_miss_count >= 1);
    // Re-arm: deadline advanced by period_ticks.
    CT_ASSERT(helper->deadline_ticks == deadline_before + period);
    // Latch cleared for the next period.
    CT_ASSERT(helper->deadline_missed == false);

    release_overrun_blocked(helper, gate);
};

#if CONFIG_DEADLINE_MONITOR_TASK
// Runmode: kernel
// Testidea: With CONFIG_DEADLINE_MONITOR_TASK=1, verify the [deadline-mon]
// task is spawned at priority 127 and in BLOCKED state.
// Input: (none — task created during Scheduler::init())
// Expect: A task with priority 127 and state BLOCKED exists.
TEST_CLASS(DeadlineMonitorTaskSpawned) {
    bool found = false;
    for (uint64_t i = 0; i < Scheduler::task_count(); ++i) {
        auto *t = Scheduler::task_at(i);
        if (t && t->magic == TaskControlBlock::TCB_MAGIC &&
            t->priority == 127 && t->state == TaskState::BLOCKED) {
            found = true;
            break;
        }
    }
    CT_ASSERT(found);
};

// Runmode: kernel
// Testidea: The deadline monitor's scan_deadlines() detects a task with
// deadline in the past.
// Input: A real kernel task (prio 11, period 2) genuinely overruns and
//        blocks on a real semaphore; its real deadline is past.  The
//        monitor's scan (Scheduler::scan_deadlines) runs.
// Expect: deadline_missed==true, deadline_miss_count>=1.
TEST_CLASS(DeadlineMonitorDetectsMiss) {
    sync::Semaphore gate;
    gate.init(0, 1);

    auto *helper = spawn_overrun_blocked(gate);
    CT_ASSERT(helper != nullptr);

    kernel::test::trigger_deadline_monitor_scan();

    CT_ASSERT(helper->deadline_miss_count >= 1);

    release_overrun_blocked(helper, gate);
};
#endif // CONFIG_DEADLINE_MONITOR_TASK

void register_deadline_miss_tests() {
    Logger::info("Registering deadline miss detection tests");
    REGISTER_CLASS(DeadlineMissWhileBlocked);
    REGISTER_CLASS(DeadlineMissWhileTerminatedSkipped);
    REGISTER_CLASS(DeadlineRearmOnPeriodRollover);
#if CONFIG_DEADLINE_MONITOR_TASK
    REGISTER_CLASS(DeadlineMonitorTaskSpawned);
    REGISTER_CLASS(DeadlineMonitorDetectsMiss);
#endif
}
