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

/// @file waiter_tag.hpp
/// @brief Shared waiter-identity predicates for sync primitives (issue #256
///        H5/H8: the pointer-AND-generation conjunction was repeated at
///        ~30 sites across mutex/semaphore/queue/notify/eventgroup).
///        Pure (pointers + integers only): acquires no lock, calls no
///        scheduler; call sites keep their lock scope and short-circuit
///        outcome byte-identical.

#pragma once

#include <types.hpp>
#include <kernel/task/task.hpp>

namespace kernel {
namespace sync {

// Stored-waiter identity: pointer AND generation conjunction (defeats ABA
// when a TCB slot is recycled onto a new task).  Cheap-pointer-first order.
// Generations ride as uint64_t (widening-only: Notify stores its gen in a
// uint64_t field, the arrays hold uint32_t — both directions are exact).
inline bool same_waiter(const TaskControlBlock *stored, uint64_t stored_gen,
                        const TaskControlBlock &task) noexcept {
    return stored == &task && stored_gen == task.generation;
}

// Wakeable waiter: identity holds AND the task is not dead.  A cleaned-up
// task is REAPED, not TERMINATED — both are unwakeable (waking a freed TCB
// is ready-queue corruption / UAF).  The null check is fail-closed: dense
// waiter arrays never hold null, so it never fires on valid states.
inline bool waiter_awakeable(const TaskControlBlock *stored,
                             uint64_t stored_gen) noexcept {
    return stored != nullptr && stored->generation == stored_gen &&
           stored->state != TaskState::TERMINATED &&
           stored->state != TaskState::REAPED;
}

// Stored last-peer staleness for the PIP boost/restore paths: a REAPED or
// recycled peer must never be touched.  Unlike waiter_awakeable, a
// TERMINATED (not yet reaped) peer is still allocated and safe to boost —
// hence the narrower check.  Pure check only; callers keep their null +
// holder reset.  Callers guarantee non-null (early return above).
inline bool peer_stale(const TaskControlBlock *stored,
                       uint64_t stored_gen) noexcept {
    return stored->state == TaskState::REAPED ||
           stored->generation != stored_gen;
}

} // namespace sync
} // namespace kernel
