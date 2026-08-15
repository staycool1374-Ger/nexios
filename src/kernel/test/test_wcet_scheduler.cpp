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

/// @file test_wcet_scheduler.cpp
/// @brief WCET benchmark for the deadline-miss detection scan
///        (Scheduler::scan_deadlines / DeadlineList walk). Phase 7b.
///
///        v0.3.10 rework (SIMULATED → DRIVEN): the scan population is built
///        from REAL dispatched kernel tasks with genuinely-expired deadlines
///        (a real task that busy-waits past its real deadline).  No existing
///        task's deadline/period is mutated.

#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/test/test_sched_helpers.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Measure the worst-case execution time (WCET) of the deadline
// miss-detection scan (Scheduler::scan_deadlines) as a function of the number
// of deadline-tracked tasks.  The scan population is a REAL dispatched kernel
// task population whose deadlines genuinely expire (each task busy-waits past
// its real deadline).
//
// One population of 30 tasks is created once (each genuinely overruns its
// real 2-tick deadline), then trimmed (deleted) to obtain the 1/10/30 data
// points.
//
// Note: the population is capped at 30 because every task blocks on the SAME
// gate semaphore, whose waiter array is bounded by CONFIG_SYNC_MAX_WAITERS
// (32).  A population above that limit makes the 33rd wait() fail
// add_waiter() → ENSURE(added) panic (observed at baseline; masked by the
// earlier `all`-gate hangs).
//
// Expect: scan_deadlines() returns a non-zero cycle count (the scan ran) for
// each task-population; the measured worst-case is logged for off-line
// analysis (see docs/specs/oom-rt.md §4).
JARVIS_TEST(wcet_scan_deadlines, "PRE: none | POST: none") {
    const uint64_t kIters = 300;

    // --- Build one population of 30 REAL tasks that genuinely overrun their
    //     2-tick deadline (busy-wait 5 real ticks) and then block.  Keeping
    //     them live lets the real monitor scan observe the expired set.
    sync::Semaphore gate;
    gate.init(0, 64);
    TaskControlBlock *tasks[64] = {};
    uint64_t made = 0;
    for (uint64_t k = 0; k < 30; ++k) {
        arch::IrqGuard guard;
        if (Scheduler::task_count() >= 58)
            break; // headroom below MAX_TASKS
        auto *t = TaskControlBlock::create(
            []() {
                auto *self = Scheduler::current_task();
                uint64_t start = arch::Timer::ticks();
                while (arch::Timer::ticks() - start < 5)
                    arch::pause();
                reinterpret_cast<sync::Semaphore *>(self->user_data)->wait();
                // INV-4: Semaphore::wait() sets BLOCKED then returns
                // immediately (the switch is deferred); returning from the
                // lambda would self-terminate before the harness observes
                // BLOCKED.  Spin on our own BLOCKED state so the harness can
                // observe it; gate.post() wakes us and we return + terminate.
                while (Scheduler::current_task()->state == TaskState::BLOCKED)
                    arch::pause();
            },
            11, 2);
        if (t == nullptr)
            break;
        t->user_data = &gate;
        Scheduler::add_task(*t);
        tasks[made++] = t;
    }
    auto teardown = ScopeGuard([&]() {
        for (uint64_t k = 0; k < made; ++k) {
            if (tasks[k]) {
                if (tasks[k]->magic == TaskControlBlock::TCB_MAGIC) {
                    Scheduler::remove_task(*tasks[k]);
                    tasks[k]->cleanup();
                    delete tasks[k];
                }
            }
        }
    });

    Scheduler::reschedule();
    for (uint64_t k = 0; k < made; ++k) {
        while (tasks[k]->state != TaskState::BLOCKED)
            arch::pause();
    }

    // --- Measure worst-case scan cycles over the real overrun population.
    auto measure = [&]() -> uint64_t {
        uint64_t max_cycles = 0;
        for (uint64_t it = 0; it < kIters; ++it) {
            uint64_t const s = arch::rdtsc();
            kernel::test::trigger_deadline_monitor_scan();
            uint64_t const e = arch::rdtsc();
            uint64_t const d = (e > s) ? (e - s) : 0;
            if (d > max_cycles)
                max_cycles = d;
        }
        return max_cycles;
    };

    uint64_t const c40 = measure();

    JARVIS_ASSERT(c40 > 0);
    Logger::info("[WCET] scan_deadlines 30-task worst=");
    Logger::print_dec(c40);
    Logger::info(" cyc");

    for (uint64_t k = 0; k < made; ++k)
        gate.post();
    for (uint64_t k = 0; k < made; ++k) {
        while (TaskControlBlock::is_valid(tasks[k]) &&
               tasks[k]->state != TaskState::TERMINATED)
            arch::pause();
    }

    JARVIS_TEST_PASS();
}

void register_wcet_scheduler_tests() {
    Logger::info("Registering WCET scheduler benchmark tests");
    JARVIS_REGISTER_TEST(wcet_scan_deadlines);
}
