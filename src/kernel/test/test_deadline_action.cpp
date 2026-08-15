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

/// @file test_deadline_action.cpp
/// @brief Phase 7 (P7a) — Deadline-miss handler ACTION dispatch coverage.
///
///        The handler action is selected at COMPILE TIME by
///        CONFIG_DEADLINE_ACTION (nexios_config.h). To verify each action
///        (LOG_ONLY / DEMOTE / KILL / NOTIFY_MONITOR) this file is compiled
///        once per action under the config matrix (tools/deadline_matrix.sh).
///        Exactly one test is registered per build — the one matching the
///        compiled action — so the class always contributes a single test
///        and the `all`/per-class expected counts stay stable.
///
///        action == 1 (PANIC) has no in-suite test: the handler halts the
///        kernel, so the matrix treats that build as an expected-fail.
///
///        v0.3.10 rework (SIMULATED → DRIVEN): the offending task is a REAL
///        kernel task (prio 11, period 2) that genuinely overruns its real
///        deadline (busy-waits 40 real ticks) and then blocks on a real
///        semaphore — the detection scan (the exact entry the [deadline-mon]
///        task runs) observes a state reached through real execution.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/timer.hpp>
#include <signal.hpp>

using namespace kernel;

namespace {

/// @brief Create a REAL task that genuinely overruns its 2-tick deadline
///        (busy-waits 40 real ticks) then blocks on the semaphore so it stays
///        live (BLOCKED) for the detection scan.  Returns the live task.
///        Callers must CT_ASSERT the returned pointer (CT_ASSERT is only valid
///        inside a TEST_CLASS, not here).
TaskControlBlock *spawn_overrun_blocked(sync::Semaphore &gate) {
    auto *helper = TaskControlBlock::create(
        []() {
            sync::Semaphore *g = reinterpret_cast<sync::Semaphore *>(
                Scheduler::current_task()->user_data);
            uint64_t start = arch::Timer::ticks();
            while (arch::Timer::ticks() - start < 40)
                arch::pause();
            g->wait();
            // INV-4: wait() sets BLOCKED then returns (deferred switch).  Spin
            // on BLOCKED so the harness observes it before self-termination.
            while (Scheduler::current_task()->state == TaskState::BLOCKED)
                arch::pause();
        },
        11, 2);
    if (helper == nullptr)
        return nullptr;
    helper->user_data = &gate;
    Scheduler::add_task(*helper);
    Scheduler::reschedule();
    while (helper->state != TaskState::BLOCKED)
        arch::pause();
    return helper;
}

/// @brief Wake the blocked helper (real semaphore post) and reap it.
void release_overrun_blocked(TaskControlBlock *helper, sync::Semaphore &gate) {
    gate.post();
    kernel::test::wait_for_termination_safe(helper);
    kernel::test::terminate_and_drain(*helper);
}

/// @brief Run the detection path through the real timer-woken monitor task.
void run_detection() {
    kernel::test::trigger_deadline_monitor_scan();
}

} // namespace

#if CONFIG_DEADLINE_ACTION == 0
// Runmode: kernel
// Testidea: LOG_ONLY (action=0, default build) records the miss and leaves
// the task alive with unchanged priority.
// Input: Real kernel task genuinely overruns its deadline and blocks; the
//        detection scan fires.
// Expect: deadline_miss_count>=1, state != TERMINATED, priority unchanged.
TEST_CLASS(DeadlineActionLogOnly) {
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *helper = spawn_overrun_blocked(gate);
    CT_ASSERT(helper != nullptr);
    uint64_t saved_prio = helper->priority;
    run_detection();
    CT_ASSERT(helper->deadline_miss_count >= 1);
    CT_ASSERT(helper->state != TaskState::TERMINATED);
    CT_ASSERT(helper->priority == saved_prio);
    release_overrun_blocked(helper, gate);
};
#endif

#if CONFIG_DEADLINE_ACTION == 2
// Runmode: kernel
// Testidea: DEMOTE (action=2) halves the offending task's priority (floored
// at 1) on miss.
// Input: Real kernel task genuinely overruns its deadline and blocks; the
//        detection scan fires.
// Expect: deadline_miss_count>=1, priority == 10>>1 == 5, priority >= 1.
TEST_CLASS(DeadlineActionDemote) {
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *helper = spawn_overrun_blocked(gate);
    CT_ASSERT(helper != nullptr);
    uint64_t saved_prio = helper->priority;
    run_detection();
    CT_ASSERT(helper->deadline_miss_count >= 1);
    CT_ASSERT(helper->priority == (saved_prio >> 1));
    CT_ASSERT(helper->priority >= 1);
    release_overrun_blocked(helper, gate);
};
#endif

#if CONFIG_DEADLINE_ACTION == 3
// Runmode: kernel
// Testidea: KILL (action=3) marks the task TERMINATED and defers cleanup.
// The deferred kill is flushed by process_deferred_kills() (the same call
// on_tick() makes after returning) and must be leak-free (framework
// snapshot/restore checks ResourceTracker).
// Input: Real kernel task genuinely overruns its deadline and blocks; the
//        detection scan fires and KILLs it.
// Expect: deadline_miss_count>=1, state == TERMINATED. Helper is freed by
//         the flush — do NOT dereference afterwards.
TEST_CLASS(DeadlineActionKill) {
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *helper = spawn_overrun_blocked(gate);
    CT_ASSERT(helper != nullptr);
    run_detection();
    CT_ASSERT(helper->deadline_miss_count >= 1);
    CT_ASSERT(helper->state == TaskState::TERMINATED);
    // The next real timer tick flushes the deferred kill list.  Do not touch
    // helper after the monitor has marked it for deferred destruction.
    const uint64_t helper_id = helper->id;
    gate.post();
    while (Scheduler::find_task(helper_id) != nullptr)
        arch::hlt();
};
#endif

#if CONFIG_DEADLINE_ACTION == 4
// Runmode: kernel
// Testidea: NOTIFY_MONITOR (action=4) delivers SIGUSR1 to the monitor task.
// The test-only hook (TestContext::deadline_monitor_pid) is pointed at the
// live [deadline-mon] task so the compile-time CONFIG_DEADLINE_MONITOR_PID (0
// by default) is bypassed.
// Input: Real kernel task genuinely overruns its deadline and blocks; hook
//        set to monitor id; detection scan fires.
// Expect: deadline_miss_count>=1, monitor pending_signals has SIGUSR1.
TEST_CLASS(DeadlineActionNotifyProbe) {
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *helper = spawn_overrun_blocked(gate);
    CT_ASSERT(helper != nullptr);
    auto *mon = Scheduler::get_monitor_task();
    CT_ASSERT(mon != nullptr);
    auto *tctx = Scheduler::get_test_context();
    CT_ASSERT(tctx != nullptr);
    tctx->deadline_monitor_pid = mon->id;
    run_detection();
    CT_ASSERT(helper->deadline_miss_count >= 1);
    CT_ASSERT((mon->pending_signals &
               (1ULL << static_cast<uint64_t>(Signal::SIGUSR1))) != 0);
    tctx->deadline_monitor_pid = 0;
    release_overrun_blocked(helper, gate);
};
#endif

#if CONFIG_DEADLINE_ACTION == 1
// Runmode: kernel
// Testidea: PANIC (action=1) must halt the kernel when a deadline miss fires.
// This test only compiles under CONFIG_DEADLINE_ACTION==1. It triggers a miss
// and expects the handler to call panic() BEFORE the test completes — the
// config matrix treats the presence of the "action=PANIC" message as the
// success signal (the kernel intentionally does not return here).
// Input: Real kernel task genuinely overruns its deadline and blocks; the
//        detection scan fires.
// Expect: kernel panics with [DMD] ... action=PANIC (verified by the matrix,
//         not by an in-test assertion).
TEST_CLASS(DeadlineActionPanics) {
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *helper = spawn_overrun_blocked(gate);
    CT_ASSERT(helper != nullptr);
    run_detection();
    // Reached only if PANIC did NOT fire — that is a failure of the action.
    CT_ASSERT(false);
};
#endif

void register_deadline_action_tests() {
#if CONFIG_DEADLINE_ACTION == 0
    REGISTER_CLASS(DeadlineActionLogOnly);
#elif CONFIG_DEADLINE_ACTION == 1
    REGISTER_CLASS(DeadlineActionPanics);
#elif CONFIG_DEADLINE_ACTION == 2
    REGISTER_CLASS(DeadlineActionDemote);
#elif CONFIG_DEADLINE_ACTION == 3
    REGISTER_CLASS(DeadlineActionKill);
#elif CONFIG_DEADLINE_ACTION == 4
    REGISTER_CLASS(DeadlineActionNotifyProbe);
#endif
}
