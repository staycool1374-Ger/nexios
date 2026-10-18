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

/// @file scheduler_dispatch.cpp
/// @brief Dispatch decision core for Scheduler (issue #323): preemption
///        predicate + next-task selection.  Pure move out of scheduler.cpp.

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_queue_internal.hpp>
#include <kernel/task/scheduler_path.hpp>

namespace kernel {

using sched_queue::edf_preempt_candidate;
using sched_queue::edf_remove;

bool Scheduler::needs_switch() noexcept {
    if (all_tasks_.size() <= 1)
        return false;
    auto *current = current_task();
    if (!current || current->magic != TaskControlBlock::TCB_MAGIC)
        return false;
    if (current == own_idle())
        return false;

    // A blocked/terminated current task must always yield to let a runnable
    // task take over — otherwise a high-priority task that blocks
    // (e.g. inside IPC::send_sync) would keep "winning" the priority compare
    // against lower-priority ready peers and the scheduler would never switch
    // to them, deadlocking the handshake (kernel hang in the test suite).
    if (current->state != TaskState::READY &&
        current->state != TaskState::RUNNING)
        return true;

    // Issue #19: earlier-deadline EDF preemption.  Cross-class comparison
    // stays priority-based below, so exempt/system tasks keep exact
    // current behavior — EDF only ADDS a preemption source, never removes
    // the bitmap one.
    if (edf_preempt_candidate() != nullptr)
        return true;

    uint64_t cur_eff = effective_priority(current);
    // O(1): check if any higher-priority task exists in the ready queue
    uint64_t highest_ready = rq_own().highest_ready_priority();
    return highest_ready > cur_eff;
}

TaskControlBlock *Scheduler::next_task() noexcept {
    if (all_tasks_.size() <= 1) [[unlikely]]
        return own_idle();

    // Issue #19: the EDF candidate agreed with needs_switch() — dispatch
    // it first so the arm and the decision can never disagree.
    if (auto *edf_next = edf_preempt_candidate()) [[unlikely]] {
        edf_remove(*edf_next, sched_cpu());
        return edf_next;
    }

    {
        // Issue #25 C1: dequeue from the OWN queue only (no stealing);
        // candidates not affine here are skipped (defense-in-depth).
        while (auto *candidate = rq_own().peek_highest()) {
            if (candidate == current_task() ||
                (candidate->state != TaskState::READY &&
                 candidate->state != TaskState::RUNNING) ||
                queue_target(*candidate) != sched_cpu()) [[unlikely]] {
            rq_own().dequeue_highest();
            continue;
        }
            kernel::dispatch_step(kernel::SchedDispatchSteps::Select);
            rq_own().dequeue_highest();
            kernel::dispatch_step(kernel::SchedDispatchSteps::Dequeue);
            return candidate;
        }
    }

    return own_idle();
}

TaskControlBlock *Scheduler::next_task_below(uint64_t top) noexcept {
    // Issue #242 starvation-breaker pick.  Mirrors next_task() eligibility
    // exactly (READY/RUNNING, affine to this CPU, never the current task);
    // ineligible heads are dequeued-dropped like next_task().  Each iteration
    // unlinks at least one node or clears at least one stale bitmap bit
    // (dequeue_level clears the bit when its queue is empty), both finite,
    // so the loop always terminates; the caller additionally only invokes
    // this once per tick.
    while (rq_own().has_ready()) {
        uint64_t lvl =
            rq_own().bitmap().highest_starvable_below(top);
        if (lvl == PriorityMap::STARVABLE_NONE) {
            return nullptr;
        }
        TaskControlBlock *candidate = rq_own().dequeue_level(lvl);
        if (!candidate) {
            continue;
        }
        if (candidate == current_task() ||
            (candidate->state != TaskState::READY &&
             candidate->state != TaskState::RUNNING) ||
            queue_target(*candidate) != sched_cpu()) {
            continue;
        }
        return candidate;
    }
    // Unreachable in the firing path (bitmap stable under the held
    // scheduler_lock_ between the counter block and this call), but the
    // contract promises nullptr — the caller falls back to the strict pick,
    // which returns idle on an empty queue anyway (identical outcome).
    return nullptr;
}

}  // namespace kernel
