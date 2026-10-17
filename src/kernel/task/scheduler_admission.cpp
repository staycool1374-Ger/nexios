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

/// @file scheduler_admission.cpp
/// @brief Liu-Leyland admission control for Scheduler (issue #323).
///        Pure move out of scheduler.cpp: no logic changes.

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_admission_internal.hpp>

namespace kernel {

// ---------------------------------------------------------------------------
// Issue #20 — enforced admission control (upgrades deadline.md I-8 from
// advisory warn-only to a fail-closed gate).  All helpers are
// allocation-free integer math with no logging; the caller (add_task_err)
// holds scheduler_lock_ throughout, so the check-and-enqueue is atomic.
// Wakeups of already-admitted tasks (set_task_ready) are deliberately NOT
// re-gated: denying a wakeup would wedge IPC waiters (deadlock risk, see
// the H2 history) — admission is a create-time gate, the standard
// real-time doctrine (once admitted, guaranteed).
// ---------------------------------------------------------------------------

using sched_admission::LIU_LEYLAND_BOUNDS;
using sched_admission::LIU_LEYLAND_LIMIT;
using sched_admission::LIU_LEYLAND_MAX_TASKS;
using sched_admission::server_budget_for_admission;

bool Scheduler::admission_exempted(const TaskControlBlock &t) noexcept {
    if (is_idle_task(&t))
        return true;
    if (t.magic != TaskControlBlock::TCB_MAGIC)
        return true; // corrupt: never counted (fail-closed elsewhere)
    if (t.edf_exempt)
        return true; // SS daemons, harness (taskdefs spawn sites)
    if (t.period_ticks == 0 || t.period_ticks == TaskControlBlock::NO_PERIOD)
        return true; // aperiodic / untracked (I-4)
    if (t.deadline_ticks == 0)
        return true;
    return false;
}

errors::SchedulerError
Scheduler::check_wcet_locked(const TaskControlBlock &t) noexcept {
    if (t.wcet_ticks == 0)
        return errors::SCHED_ERR_OK; // implicit 100%: validated via LUB sum
    if (t.period_ticks == 0 || t.period_ticks == TaskControlBlock::NO_PERIOD)
        return errors::SCHED_ERR_WCET_INVALID; // explicit WCET, untracked task
    if (t.wcet_ticks > t.period_ticks)
        return errors::SCHED_ERR_WCET_INVALID;
    return errors::SCHED_ERR_OK;
}

errors::SchedulerError Scheduler::admission_check_locked(
    const TaskControlBlock &candidate, uint64_t *out_util,
    uint32_t *out_bound) noexcept {
    // Issue #23: the global gate is the single-partition special case keyed
    // on the candidate's own target — one math path, no duplication, no
    // behavior change on single-CPU.
    return admission_check_cpu_locked(candidate, queue_target(candidate),
                                      out_util, out_bound);
}

errors::SchedulerError Scheduler::admission_check_cpu_locked(
    const TaskControlBlock &candidate, uint64_t cpu, uint64_t *out_util,
    uint32_t *out_bound) noexcept {
    cpu %= CONFIG_MAX_CPUS;
    // Audit S3 (issue #20): exemption precedes WCET-validity, matching the
    // I-8 scope text exactly ("periodic non-exempt tasks only").  An
    // otherwise-exempt task (boot daemon, aperiodic) carrying a stale
    // explicit WCET must never be rejected — denying a boot daemon would
    // wedge the boot.  WCET-validity binds only tasks inside the gate.
    if (admission_exempted(candidate)) {
        if (out_util != nullptr)
            *out_util = 0;
        if (out_bound != nullptr)
            *out_bound = 0;
        return errors::SCHED_ERR_OK;
    }
    errors::SchedulerError w = check_wcet_locked(candidate);
    if (w != errors::SCHED_ERR_OK)
        return w;
    // Numerator rule frozen from the advisory path: explicit WCET, else the
    // sporadic-server max budget, else remaining_ticks.  S2 note: wcet <=
    // period <= NO_PERIOD (2^32-1), so wcet*1000000 < 2^63 — no overflow;
    // period == 0 is excluded above, so the divide is safe.
    uint64_t total_util = 0;
    uint64_t real_tasks = 0;
    for (TaskControlBlock *t = all_tasks_.first_ptr(); t != nullptr;
         t = all_tasks_.next_ptr(t)) {
        if (t->id == candidate.id)
            continue; // re-check safe: never double-count the candidate
        if (admission_exempted(*t))
            continue;
        // Issue #23: partitioned accounting — only tasks targeting this
        // CPU enter this partition's numerator (queue_target: lowest-set
        // affinity bit, empty mask reads as CPU0).
        if (queue_target(*t) != cpu)
            continue;
        ++real_tasks;
        uint64_t wcet = server_budget_for_admission(*t);
        total_util += (wcet * 1000000) / t->period_ticks;
    }
    ++real_tasks;
    uint64_t cwcet = server_budget_for_admission(candidate);
    total_util += (cwcet * 1000000) / candidate.period_ticks;
    uint32_t bound = real_tasks <= LIU_LEYLAND_MAX_TASKS
                         ? LIU_LEYLAND_BOUNDS[real_tasks]
                         : LIU_LEYLAND_LIMIT;
    if (out_util != nullptr)
        *out_util = total_util;
    if (out_bound != nullptr)
        *out_bound = bound;
    if (total_util > bound)
        return errors::SCHED_ERR_ADMISSION_DENIED;
    return errors::SCHED_ERR_OK;
}

}  // namespace kernel

namespace kernel::sched_admission {

/// @brief Frozen admission numerator (issues #20/#22): explicit WCET, else
///        the attached server's max budget C (mode-agnostic — SPORADIC,
///        DEFERRABLE and BACKGROUND all expose C via max_budget(), so
///        server budgets count identically without any gate change), else
///        remaining_ticks.  Caller guarantees t is non-exempt (period > 0).
uint64_t server_budget_for_admission(const TaskControlBlock &t) noexcept {
    if (t.wcet_ticks > 0)
        return t.wcet_ticks;
    if (t.get_sporadic_server() != nullptr)
        return t.get_sporadic_server()->max_budget();
    return t.remaining_ticks;
}

}  // namespace kernel::sched_admission
