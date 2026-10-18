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

/// @file test_stress_24h.cpp
/// @brief Soak driver (issue #303, Phase 6 module 2).
///
///        A literal 24 h CI test cannot exist (no 24 h CI host), so this
///        file implements the real soak DRIVER with horizon tiers and
///        runs the CI tier (SHORT, seconds): sustained fork/exit churn,
///        continuous IPC traffic, PMM alloc/free churn, and invariant
///        evidence (free-memory delta, tick liveness, inbox hygiene,
///        ResourceTracker zero-delta via isolation) after every round.
///        Horizons MEDIUM (minutes) / LONG (hours) / FULL (1/6/12/24 h)
///        are selected by STRESS_24H_HORIZON at compile time and produce
///        the per-horizon evidence lines the tracker ticks on — but only
///        a human-run host can execute them; CI runs SHORT.
///        LEFT for long-horizon runs: 1/6/12/24 h invariant evidence and
///        the deadline-overrun rate (< 0.01 % needs hours of EDF load).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

// Compile-time horizon: 0 = SHORT (CI, seconds), 1 = MEDIUM (minutes),
// 2 = LONG (hours, manual), 3 = FULL (1/6/12/24 h evidence, manual).
#ifndef STRESS_24H_HORIZON
#define STRESS_24H_HORIZON 0
#endif

struct Horizon {
    uint64_t fork_cycles;
    uint64_t ipc_burst;
    uint64_t pmm_pages;
    uint64_t rounds;
    const char *name;
};

Horizon current_horizon() {
#if STRESS_24H_HORIZON == 0
    return {20, 3000, 16, 3, "SHORT"};
#elif STRESS_24H_HORIZON == 1
    return {200, 30000, 64, 10, "MEDIUM"};
#elif STRESS_24H_HORIZON == 2
    return {2000, 200000, 256, 30, "LONG"};
#else
    return {10000, 1000000, 1024, 120, "FULL"};
#endif
}

uint64_t g_churn_exits = 0;

void churn_entry() {
    ++g_churn_exits;
}

bool fork_exit_cycle(uint64_t n) {
    uint64_t before = g_churn_exits;
    for (uint64_t i = 0; i < n; ++i) {
        auto *t = TaskControlBlock::create(churn_entry, 11, 0);
        if (t == nullptr)
            return false;
        {
            arch::IrqGuard ig{};
            Scheduler::add_task(*t);
        }
        Scheduler::reschedule();
        kernel::test::wait_for_termination_safe(t);
        if (t->state != TaskState::TERMINATED)
            return false;
        kernel::test::terminate_and_drain(*t);
    }
    return g_churn_exits == before + n;
}

bool ipc_burst(uint64_t n) {
    auto *cur = Scheduler::current_task();
    if (cur == nullptr)
        return false;
    Message drain;
    while (IPC::recv(drain))
        ;
    for (uint64_t i = 0; i < n; ++i) {
        Message msg{};
        msg.sender_id = cur->id;
        msg.type = i & 0xFF;
        msg.data_size = 0;
        if (!IPC::send(cur->id, msg, IpcFlags::NONBLOCK))
            return false;
        Message out;
        if (!IPC::recv(out))
            return false;
        if (out.type != (i & 0xFF))
            return false;
    }
    return true;
}

bool pmm_churn(uint64_t pages, uint64_t &peak_delta) {
    uint64_t base = PMM::free_memory();
    peak_delta = 0;
    for (uint64_t i = 0; i < pages; ++i) {
        uint64_t phys = PMM::alloc_page();
        if (phys == 0)
            return false;
        uint64_t held =
            (base > PMM::free_memory()) ? base - PMM::free_memory() : 0;
        if (held > peak_delta)
            peak_delta = held;
        PMM::free_page(phys);
    }
    return PMM::free_memory() == base;
}

} // namespace

// Runmode: kernel
// Testidea: SHORT-horizon soak: every round churns tasks, IPC and PMM,
// then checks the invariants (free-memory delta 0, inbox empty, all
// churn tasks exited, ticks alive).
// Input: Horizon tier (default SHORT: 20 forks + 3000 IPC + 16 pages,
// 3 rounds).
// Expect: Every round green; per-round evidence logged; zero
// ResourceTracker delta via isolation.
// Depends: TaskControlBlock create/drain, IPC self path, PMM alloc/free
JARVIS_TEST(stress_24h_short_soak_driver, "PRE: none | POST: none") {
    Horizon h = current_horizon();
    uint64_t t_start = arch::Timer::ticks();
    for (uint64_t r = 0; r < h.rounds; ++r) {
        JARVIS_ASSERT_FMT(fork_exit_cycle(h.fork_cycles),
                          "round %lu: fork/exit churn failed", r);
        JARVIS_ASSERT_FMT(ipc_burst(h.ipc_burst),
                          "round %lu: IPC burst failed", r);
        uint64_t peak = 0;
        JARVIS_ASSERT_FMT(pmm_churn(h.pmm_pages, peak),
                          "round %lu: PMM churn leaked", r);
        Message leftover;
        JARVIS_ASSERT_FMT(!IPC::recv(leftover),
                          "round %lu: inbox not clean", r);
        Logger::info("SOAK %s round %lu/%lu: forks=%lu ipc=%lu "
                     "pmm_pages=%lu peak_hold=%lu B ticks=%lu",
                     h.name, r + 1, h.rounds, h.fork_cycles, h.ipc_burst,
                     h.pmm_pages, peak, arch::Timer::ticks());
    }
    uint64_t dt = arch::Timer::ticks() - t_start;
    Logger::info("SOAK %s complete: %lu rounds in %lu ticks", h.name,
                 h.rounds, dt);
    JARVIS_ASSERT_FMT(dt > 0, "timer did not advance during soak");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Scheduler liveness across churn: the tick count strictly
// grows while tasks are created, dispatched and reaped (no starvation
// or wedged runqueue hides inside the churn).
// Input: Tick sample, 10 fork/exit cycles, tick sample.
// Expect: Later sample strictly greater; all 10 exited.
// Depends: Timer::ticks, driven task dispatch
JARVIS_TEST(stress_24h_scheduler_liveness, "PRE: none | POST: none") {
    uint64_t t0 = arch::Timer::ticks();
    JARVIS_ASSERT_FMT(fork_exit_cycle(10), "liveness churn failed");
    uint64_t t1 = arch::Timer::ticks();
    JARVIS_ASSERT_FMT(t1 > t0, "ticks frozen across 10 dispatches");
    Logger::info("SOAK liveness: 10 cycles over %lu ticks", t1 - t0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all soak-driver tests.
// Input: None
// Expect: Soak tests registered
// Depends: test framework
void register_stress_24h_tests() {
    Logger::info("Registering 24h-stress soak driver");
    JARVIS_REGISTER_TEST(stress_24h_short_soak_driver);
    JARVIS_REGISTER_TEST(stress_24h_scheduler_liveness);
}
