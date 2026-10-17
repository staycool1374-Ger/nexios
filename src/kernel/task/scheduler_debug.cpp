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

/// @file scheduler_debug.cpp
/// @brief Debugger support + id-table debug helpers for Scheduler
///        (issue #323).  Pure move out of scheduler.cpp: no logic changes.

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_debug_internal.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/spinlock_guard.hpp>

namespace kernel {

namespace sched_debug {
constinit uint64_t debug_tick_pc_[CONFIG_MAX_CPUS] = {};
}  // namespace sched_debug

using sched_debug::debug_tick_pc_;

bool Scheduler::debug_id_table_references(void *block) noexcept {
    auto *b = static_cast<TaskControlBlock *>(block);
    for (uint64_t i = 0; i < ID_TABLE_SIZE; ++i) {
        if (id_table_[i] == b)
            return true;
    }
    return false;
}

void Scheduler::note_debug_tick_pc(uint64_t interrupted_pc) noexcept {
    uint64_t cpu = sched_cpu();
    if (cpu < CONFIG_MAX_CPUS)
        __atomic_store_n(&debug_tick_pc_[cpu], interrupted_pc,
                         __ATOMIC_RELAXED);
}

// Issue #226: debugger fault-stop park. ISR-safe: takes scheduler_lock_
// directly (same position as switch_away_from_terminating, which runs
// after with the lock released — never nested). The tick park path never
// takes g_bind_lock, so router order (scheduler -> stop-queue) matches the
// documented scheduler_lock_ -> registry order.
bool Scheduler::debug_park_stop(TaskControlBlock &tgt, uint64_t kind,
                               uint64_t va) noexcept {
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    if (__atomic_load_n(&tgt.debugger_id, __ATOMIC_ACQUIRE) == 0)
        return false;
    if (tgt.magic != TaskControlBlock::TCB_MAGIC)
        return false;
    __atomic_store_n(&tgt.debug_stop_kind, kind, __ATOMIC_RELEASE);
    __atomic_store_n(&tgt.debug_stop_va, va, __ATOMIC_RELEASE);
    // A step-over re-arm belongs to exactly one pending step: any fresh
    // non-STEP stop supersedes it (otherwise a later unrelated STEP
    // completion would re-insert an unasked breakpoint).
    if (kind != 2) // StopKind::STEP numerics (debug_stop.hpp)
        __atomic_store_n(&tgt.debug_rearm_va, 0, __ATOMIC_RELEASE);
    tgt.debug_parked = true;
    tgt.state = TaskState::BLOCKED;
    dequeue_ready(tgt);
    tgt.has_utrap_frame = true; // the trap wrote the U-frame slot
    return true;
}

// Issue #225: debugger detach resume. Task-context only (takes IrqGuard +
// scheduler_lock_); the tick park path never takes g_bind_lock, so the
// detach order (bind -> scheduler) cannot invert.
void Scheduler::debugger_resume(TaskControlBlock &tgt) noexcept {
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    __atomic_store_n(&tgt.debug_stop_requested, false, __ATOMIC_RELEASE);
    if (!__atomic_load_n(&tgt.debug_parked, __ATOMIC_ACQUIRE))
        return;
    __atomic_store_n(&tgt.debug_parked, false, __ATOMIC_RELEASE);
    // Resume only what the park hook still owns: BLOCKED with a debugger
    // attached. Any other BLOCKED channel keeps its waiter.
    if (tgt.state != TaskState::BLOCKED ||
        __atomic_load_n(&tgt.debugger_id, __ATOMIC_ACQUIRE) == 0)
        return;
    // The park hook dequeued before blocking and enqueue_ready self-unlinks
    // the EDF case: READY, never RUNNING (the runq owns READY tasks only).
    tgt.state = TaskState::READY;
    enqueue_ready(tgt);
}

}  // namespace kernel
