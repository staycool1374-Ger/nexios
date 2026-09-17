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

/// @file admission_selftest.cpp
/// @brief Boot-time admission self-test (issue #24): read-only probes of
///        the frozen gate math.  No allocation, no table mutation.

#include <kernel/task/admission_selftest.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/nexios_config.h>
#include <logger.hpp>
#include <string.hpp>

namespace kernel {

// Miss-action configuration is a build-time contract: only actions
// 0..4 exist (deadline.md §3).  The runtime check below mirrors this.
static_assert(CONFIG_DEADLINE_ACTION <= 4,
              "CONFIG_DEADLINE_ACTION out of range 0..4 (deadline.md §3)");

namespace {

// Synthetic probe candidate: stack-constructed, never added to any table
// (all helpers used here only READ it; the id can never collide with a
// live task).  No vtable use (no virtual calls on this path), no ctor.
void make_probe_candidate(TaskControlBlock &cand, uint64_t wcet,
                          uint64_t period, uint64_t deadline, bool exempt,
                          uint64_t affinity) noexcept {
    // Same memset discipline as TaskControlBlock::create (task.cpp): the
    // stack object is zeroed then fields are assigned; no virtual calls
    // are made on this path, so the zeroed vptr is never dereferenced.
    memset(&cand, 0, sizeof(cand));
    cand.magic = TaskControlBlock::TCB_MAGIC;
    cand.id = ~0ULL;
    cand.state = TaskState::READY;
    cand.priority = 11;
    cand.base_priority = 11;
    cand.cpu_affinity = affinity;
    cand.period_ticks = period;
    cand.deadline_ticks = deadline;
    cand.wcet_ticks = wcet;
    cand.remaining_ticks = period;
    cand.sched_policy = SchedPolicy::AUTO;
    cand.edf_exempt = exempt;
}

} // namespace

bool admission_boot_selftest() noexcept {
    bool ok = true;

    // (a) Admit-math pin: on the quiescent boot table (idle [+ monitor],
    // both skipped) a 60%-of-100 probe must admit with EXACT outputs
    // (util 600000, single-task bound 1000000).  Any drift in the frozen
    // numerator, the partition key, or the LUB table trips this.
    // Audit note: admission_check_cpu_locked is documented caller-holds-
    // lock; the waiver is safe here by construction — single BSP with
    // interrupts still disabled at this boot stage (no sti yet in
    // kernel.cpp), no second task context exists, and the helper is
    // allocation-free/read-only (the timer ISR takes the lock by
    // try_lock and skips on contention, so it can never block on us).
    {
        TaskControlBlock cand{};
        make_probe_candidate(cand, 60, 100, 1000, false, 1);
        uint64_t util = 0;
        uint32_t bound = 0;
        errors::SchedulerError r = Scheduler::admission_check_cpu_locked(
            cand, 0, &util, &bound);
        if (r != errors::SCHED_ERR_OK || util != 600000 ||
            bound != 1000000) {
            Logger::error("admission self-test: admit-math mismatch "
                          "(err=%u util=%u bound=%u)",
                          static_cast<uint64_t>(r), util, bound);
            ok = false;
        }
    }

    // (b) WCET-validity + exemption order: explicit WCET past the period
    // is rejected; an exempt task with a stale WCET is admitted (never
    // gate exempt tasks — denying boot daemons would wedge the boot).
    {
        TaskControlBlock bad{};
        make_probe_candidate(bad, 200, 100, 1000, false, 1);
        if (Scheduler::admission_check_cpu_locked(bad, 0, nullptr,
                                                  nullptr) !=
            errors::SCHED_ERR_WCET_INVALID) {
            Logger::error(
                "admission self-test: wcet>period not rejected");
            ok = false;
        }
        TaskControlBlock stale{};
        make_probe_candidate(stale, 50, 0, 0, true, 1);
        if (Scheduler::admission_check_cpu_locked(stale, 0, nullptr,
                                                  nullptr) !=
            errors::SCHED_ERR_OK) {
            Logger::error(
                "admission self-test: exempt task wrongly gated");
            ok = false;
        }
    }

    // (c) Budget-exhaustion gate: the global budget must be live (sized at
    // boot) and a reserve beyond it must fail WITHOUT mutating the
    // counter (failed reserve returns false before decrementing).
    // Compiled out with the gate itself under CONFIG_MEMORY_BUDGET=0.
#if CONFIG_MEMORY_BUDGET
    {
        uint64_t avail = Scheduler::remaining_memory_budget();
        if (avail == 0) {
            Logger::error(
                "admission self-test: memory budget not sized at boot");
            ok = false;
        } else if (Scheduler::reserve_memory_pages(avail + 1)) {
            Logger::error(
                "admission self-test: over-budget reserve wrongly granted");
            Scheduler::release_memory_pages(avail + 1);
            ok = false;
        } else if (Scheduler::remaining_memory_budget() != avail) {
            Logger::error(
                "admission self-test: failed reserve mutated the budget");
            ok = false;
        }
    }
#endif

    // (d) Miss-action wiring: the configured action is in range (mirrors
    // the static_assert above for the exact binary under test).  Handler
    // behavior itself is covered by the deadline_* test classes — the
    // boot path invokes nothing.
    if (CONFIG_DEADLINE_ACTION > 4) {
        Logger::error("admission self-test: deadline action %u out of "
                      "range 0..4",
                      CONFIG_DEADLINE_ACTION);
        ok = false;
    }

    if (ok) {
        Logger::info("[BOOT] admission self-test OK "
                     "(lub-math/wcet/budget/miss-action=%u)",
                     CONFIG_DEADLINE_ACTION);
    }
    return ok;
}

} // namespace kernel
