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

/// @file test_wcet_overrun.cpp
/// @brief WCET overrun detection tests — Phase 3: distinguish execution-
///        time overrun from deadline miss.
///
///        v0.3.10 rework (SIMULATED → DRIVEN): the task GENUINELY runs past
///        its WCET on real timer ticks.  wcet_ticks is a static per-task
///        configuration (set at create time, like period_ticks — it is NOT a
///        runtime state field).  The real on_tick ISR fires the WCET handler
///        and latches `wcet_overrun_fired`; the task busy-waits until it
///        observes its own latch set by real execution.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/timer.hpp>

using namespace kernel;

namespace {

/// @brief Create a REAL kernel task (prio 11) with the given static WCET.
///        Dispatches it for real and waits for genuine termination.
/// @param wcet_ticks Static WCET configuration (ticks).
/// @param period     Task period (ticks).
/// @param latch      Out-param: the task sets *latch = 1 if the real ISR
///                   fired the WCET overrun latch on it before it exited.
/// @return TCB pointer (caller must release), or nullptr.
TaskControlBlock *run_wcet_task(uint64_t wcet_ticks, uint64_t period,
                                uint64_t *latch) {
    auto *t = TaskControlBlock::create(
        []() {
            uint64_t *l = reinterpret_cast<uint64_t *>(
                Scheduler::current_task()->user_data);
            auto *self = Scheduler::current_task();
            // Run until the REAL on_tick ISR latches wcet_overrun_fired
            // (executed_ticks has exceeded wcet_ticks) — or the busy-wait
            // budget is exhausted.
            uint64_t start = arch::Timer::ticks();
            while (arch::Timer::ticks() - start < 200) {
                if (self->wcet_overrun_fired) {
                    *l = 1;
                    return;
                }
                arch::pause();
            }
            *l = 0;
        },
        11, period);
    if (t == nullptr)
        return nullptr;
    t->user_data = latch;
    t->wcet_ticks = wcet_ticks;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    return t;
}

void release_task(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    kernel::test::terminate_if_live(t);
}

} // namespace

// Runmode: kernel
// Testidea: A task with wcet_ticks > 0 that GENUINELY exceeds its WCET on
// real timer ticks must fire the WCET overrun latch (set by the real on_tick
// ISR) — and must NOT fire a deadline miss.
// Input: Real kernel task (prio 11, period 10, wcet 2) busy-waits until the
//        real ISR latches wcet_overrun_fired.  No deadline scan is run.
// Expect: wcet_overrun_fired == true (observed in the running task);
//         deadline_miss_count == 0 (deadline detection was never triggered).
TEST_CLASS(WcetOverrunDetectionFires) {
    static uint64_t g_latch = 0;
    auto *helper = run_wcet_task(2, 10, &g_latch);
    CT_ASSERT(helper != nullptr);
    CT_ASSERT(g_latch == 1);
    // The task exited by its own observation of the real-ISR latch.
    CT_ASSERT(helper->wcet_overrun_fired == true);
    // Deadline detection was never invoked, so no miss was recorded.
    CT_ASSERT(helper->deadline_miss_count == 0);
    release_task(helper);
    Scheduler::drain_zombie_list();
};

// Runmode: kernel
// Testidea: A task that meets WCET (runs within its static budget) but whose
// real deadline passes while it is genuinely blocked must fire the DEADLINE
// handler but NOT the WCET overrun handler.
// Input: Real kernel task (prio 11, period 2, wcet 1000000) blocks on a real
//        semaphore after a short real run; the detection scan fires.
// Expect: deadline_miss_count>=1, wcet_overrun_fired==false.
TEST_CLASS(DeadlineMissWithinWcet) {
    static uint64_t g_latch = 0;
    sync::Semaphore gate;
    gate.init(0, 1);

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
        11, 2);
    CT_ASSERT(helper != nullptr);
    helper->user_data = &gate;
    helper->wcet_ticks = 1000000; // static WCET far above any real run
    Scheduler::add_task(*helper);
    Scheduler::reschedule();
    while (helper->state != TaskState::BLOCKED)
        arch::pause();

    // Deadline genuinely passed while the task was running/blocked.
    kernel::test::trigger_deadline_monitor_scan();

    // Deadline must fire; WCET must NOT fire (budget not exceeded).
    CT_ASSERT(helper->deadline_miss_count >= 1);
    CT_ASSERT(helper->wcet_overrun_fired == false);
    CT_ASSERT(g_latch == 0);

    gate.post();
    kernel::test::wait_for_termination_safe(helper);
    release_task(helper);
    Scheduler::drain_zombie_list();
};

void register_wcet_overrun_tests() {
    Logger::info("Registering WCET overrun detection tests");
    REGISTER_CLASS(WcetOverrunDetectionFires);
    REGISTER_CLASS(DeadlineMissWithinWcet);
}
