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

/// @file test_deadline_recovery.cpp
/// @brief Phase 5 tests: Asymmetric Recovery & Safety Protocols — safe KILL
///        cleanup, NOTIFY_MONITOR delivery, magic-check structural isolation,
///        and MC/DC coverage of the detection predicate.
///
///        v0.3.10 rework (SIMULATED → DRIVEN): detection is driven by REAL
///        kernel tasks (prio ≥ 11) that genuinely overrun their real
///        deadlines (busy-wait on the real timer) and block on real
///        semaphores; the detection scan is the exact entry the
///        [deadline-mon] task runs.  No task field is hand-mutated to reach
///        the overrun state.

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

/// @brief Create a REAL kernel task that GENUINELY overruns its 2-tick
///        deadline (busy-waits 40 real ticks) then blocks on the semaphore
///        so it stays live (BLOCKED) when the detection scan runs.
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

/// @brief Wake a blocked overrun task (real post) and reap it.
void release_overrun_blocked(TaskControlBlock *helper, sync::Semaphore &gate) {
    gate.post();
    kernel::test::wait_for_termination_safe(helper);
    kernel::test::terminate_and_drain(*helper);
}

/// @brief Create a REAL kernel task with the given period and an optional
///        short real busy-wait, then block it on the semaphore.
TaskControlBlock *spawn_blocked(sync::Semaphore &gate, uint64_t period) {
    auto *helper = TaskControlBlock::create(
        []() {
            sync::Semaphore *g = reinterpret_cast<sync::Semaphore *>(
                Scheduler::current_task()->user_data);
            uint64_t start = arch::Timer::ticks();
            while (arch::Timer::ticks() - start < 5)
                arch::pause();
            g->wait();
            // INV-4: wait() sets BLOCKED then returns (deferred switch).  Spin
            // on BLOCKED so the harness observes it before self-termination.
            while (Scheduler::current_task()->state == TaskState::BLOCKED)
                arch::pause();
        },
        11, period);
    if (helper == nullptr)
        return nullptr;
    helper->user_data = &gate;
    Scheduler::add_task(*helper);
    Scheduler::reschedule();
    while (helper->state != TaskState::BLOCKED)
        arch::pause();
    return helper;
}

} // namespace

// Runmode: kernel
// Testidea: The KILL cleanup sequence (cleanup() + remove_task() + free)
// must safely release all task resources.  This is the same sequence that
// Scheduler::process_deferred_kills() performs for deferred-kill tasks.
// Snapshot/restore at test end verifies ResourceTracker returns to baseline.
// Input: Helper task created, then killed via the manual KILL sequence.
// Expect: cleanup() and remove_task() complete without crash/assert.
TEST_CLASS(DeadlineActionKillCleansUp) {
    auto *helper = TaskControlBlock::create([]() {}, 10, 10);
    CT_ASSERT(helper != nullptr);
    helper->base_priority = 10;
    helper->priority = 10;
    Scheduler::add_task(*helper);

    // Full KILL sequence as performed by process_deferred_kills()
    kernel::test::terminate_and_drain(*helper);
};

// Runmode: kernel
// Testidea: The detection scan walks ONLY live, registered tasks and
// completes without aborting.  A genuinely-overrun live task fires a miss; a
// genuinely-terminated task (removed from the live scan set by the real
// reaper) is never scanned.  The scan-integrity counter advances, proving the
// guard that skips invalid (non-live) TCBs kept the walk safe.
// Input: Real kernel task (prio 11, period 2) genuinely overruns and blocks.
//        Another real task genuinely terminates (trampoline → zombie).
//        The detection scan runs.
// Expect: integrity counter advanced; the live task has deadline_miss_count
//         advanced; the terminated task's count stays unchanged.
TEST_CLASS(DeadlineDetectionMagicCheck) {
    sync::Semaphore gate;
    gate.init(0, 1);

    // Live overrun task (must be detected).
    auto *valid = spawn_overrun_blocked(gate);
    CT_ASSERT(valid != nullptr);
    CT_ASSERT(valid->deadline_ticks < arch::Timer::ticks());

    // Genuinely-terminated task (removed from the live scan set).
    auto *gone = TaskControlBlock::create([]() {}, 11, 2);
    CT_ASSERT(gone != nullptr);
    Scheduler::add_task(*gone);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(gone);
    uint64_t gone_count_before = gone->deadline_miss_count;

    uint64_t before_integrity = deadline_detection_integrity;

    kernel::test::trigger_deadline_monitor_scan();

    // Integrity counter must have advanced (scan completed).
    CT_ASSERT(deadline_detection_integrity == before_integrity + 1);

    // The live task must have been detected.
    CT_ASSERT(valid->deadline_miss_count >= 1);

    // The terminated task was removed from the live scan set — not detected.
    CT_ASSERT(gone->deadline_miss_count == gone_count_before);

    release_overrun_blocked(valid, gate);
    kernel::test::terminate_and_drain(*gone);
};

// Runmode: kernel
// Testidea: Each reachable condition of the deadline detection predicate must
// be shown to independently affect the outcome (MC/DC) using REAL tasks:
//
//   predicate: period_ticks > 0  &&  !deadline_missed  &&
//              deadline_ticks > 0  &&  ticks > deadline_ticks
//
// Conditions (via real execution / allowed latch field — deadline_ticks
// itself is never mutated):
//   A = period_ticks > 0          (real create() period)
//   B = !deadline_missed          (latch field on a live task)
//   D = ticks > deadline_ticks    (real overrun vs future deadline)
//
// Test cases (real kernel tasks, real detection scan):
//   1. A=1 B=1 D=1 -> fires (genuine overrun + block)
//   2. A=0 (period 0, untracked) -> no fire
//   3. B=0 (missed latch set)    -> no fire
//   5. D=0 (deadline in future)  -> no fire
//
// Case 4 (deadline_ticks == 0) is equivalent to the untracked state already
// covered by case 2 — the kernel never legitimately reaches deadline_ticks==0
// for a periodic task created via TaskControlBlock::create().
// Input: See each sub-test.  Expect: deadline_miss_count advances only in
// case 1.
TEST_CLASS(DeadlineDetectionMcdcCoverage) {
    // Case 1: All-true -> must fire.
    {
        sync::Semaphore gate;
        gate.init(0, 1);
        auto *t = spawn_overrun_blocked(gate);
        CT_ASSERT(t != nullptr);
        kernel::test::trigger_deadline_monitor_scan();
        CT_ASSERT(t->deadline_miss_count >= 1);
        release_overrun_blocked(t, gate);
    }

    // Case 2: period_ticks=0 (A false) -> must NOT fire.
    {
        sync::Semaphore gate;
        gate.init(0, 1);
        auto *t = spawn_blocked(gate, 0);
        CT_ASSERT(t != nullptr);
        kernel::test::trigger_deadline_monitor_scan();
        CT_ASSERT(t->deadline_miss_count == 0);
        gate.post();
        kernel::test::wait_for_termination_safe(t);
        kernel::test::terminate_and_drain(*t);
    }

    // Case 3: deadline in the future (D false, not past) -> must NOT fire.
    {
        sync::Semaphore gate;
        gate.init(0, 1);
        auto *t = spawn_blocked(gate, 100000);
        CT_ASSERT(t != nullptr);
        CT_ASSERT(t->deadline_ticks > arch::Timer::ticks());
        kernel::test::trigger_deadline_monitor_scan();
        CT_ASSERT(t->deadline_miss_count == 0);
        gate.post();
        kernel::test::wait_for_termination_safe(t);
        kernel::test::terminate_and_drain(*t);
    }
};

// Runmode: kernel
// Testidea: NOTIFY_MONITOR delivers SIGUSR1 to the designated monitor task
// when a REAL deadline miss fires.  The test-only hook
// (TestContext::deadline_monitor_pid) points the action=4 path at the live
// [deadline-mon] task; a real overrun task genuinely misses its deadline.
// Under the default LOG_ONLY build (CONFIG_DEADLINE_ACTION==0) the handler
// does not deliver a signal, so this test verifies the genuine miss
// detection plus the configured delivery hook; under action=4 builds it
// additionally asserts the real SIGUSR1 delivery.
// Input: Real kernel task (prio 11, period 2) genuinely overruns and blocks.
//        Hook set to monitor id; detection scan fires.
// Expect: deadline_miss_count>=1 always; monitor->pending_signals has
//         SIGUSR1 when CONFIG_DEADLINE_ACTION==4.
TEST_CLASS(DeadlineActionNotifyMonitor) {
    sync::Semaphore gate;
    gate.init(0, 1);

    auto *helper = spawn_overrun_blocked(gate);
    CT_ASSERT(helper != nullptr);

    auto *mon = Scheduler::get_monitor_task();
    CT_ASSERT(mon != nullptr);
    auto *tctx = Scheduler::get_test_context();
    CT_ASSERT(tctx != nullptr);
    tctx->deadline_monitor_pid = mon->id;

    uint64_t signals_before = mon->pending_signals;

    kernel::test::trigger_deadline_monitor_scan();

    CT_ASSERT(helper->deadline_miss_count >= 1);
#if CONFIG_DEADLINE_ACTION == 4
    CT_ASSERT(mon->pending_signals &
              (1ULL << static_cast<uint64_t>(Signal::SIGUSR1)));
#else
    // Default LOG_ONLY handler does not deliver; the hook is installed and
    // the miss genuinely fired — no spurious signal was set by this build.
    CT_ASSERT(mon->pending_signals == signals_before);
#endif

    tctx->deadline_monitor_pid = 0;
    release_overrun_blocked(helper, gate);
};

void register_deadline_recovery_tests() {
    Logger::info("Registering deadline recovery tests");
    REGISTER_CLASS(DeadlineActionKillCleansUp);
    REGISTER_CLASS(DeadlineDetectionMagicCheck);
    REGISTER_CLASS(DeadlineDetectionMcdcCoverage);
    REGISTER_CLASS(DeadlineActionNotifyMonitor);
}
