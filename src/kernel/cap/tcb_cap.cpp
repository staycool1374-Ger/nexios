/*
 * NexIOS RTOS — Capability-Based Access Control (CSpace)
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

/// @file tcb_cap.cpp
/// @brief TcbCap implementation (F4, issue #557). Pool-allocated wrapper
/// carrying (target_id, target_gen) + scope/grantee binding; folds into
/// the existing cap_objects ResourceTracker counter. No registries:
/// several wrappers may name one task, and no snapshot_reset is needed
/// (nothing global).

#include <kernel/cap/tcb_cap.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <constants.hpp>

// Placement new (defined in lib/new.cpp, no <new> header in freestanding)
inline void *operator new(unsigned long, void *placement) noexcept {
    return placement;
}

namespace kernel::cap {

namespace {

/// @brief Shared factory core: validates liveness then pool-allocates.
///        Returns nullptr when the target is not a live task.
TcbCap *tcb_cap_alloc(uint64_t target_id, uint32_t target_gen) {
    if (target_id == 0) {
        return nullptr;
    }
    auto *target = Scheduler::find_task(target_id);
    if (target == nullptr ||
        target->magic != TaskControlBlock::TCB_MAGIC) {
        return nullptr;
    }
    if (!Scheduler::validate_id(target_id)) {
        return nullptr;
    }
    if (target->generation != target_gen) {
        return nullptr;
    }
    if (target->state == TaskState::TERMINATED ||
        target->state == TaskState::REAPED) {
        return nullptr;
    }
    auto *cap = static_cast<TcbCap *>(MemPool::alloc(sizeof(TcbCap)));
    if (cap == nullptr) {
        return nullptr;
    }
    new (cap) TcbCap;
    cap->mark_pool_backed();
    cap->target_id = target_id;
    cap->target_gen = target_gen;
    kernel::test::ResourceTracker::instance().track_cap_object_add();
    return cap;
}

} // namespace

TcbCap *TcbCap::create(uint64_t target_id, uint32_t target_gen) {
    TcbCap *cap = tcb_cap_alloc(target_id, target_gen);
    if (cap == nullptr) {
        return nullptr;
    }
    cap->scope = static_cast<uint8_t>(Authority::CREATOR_ONLY);
    cap->grantee_id = 0;
    cap->grantee_gen = 0;
    return cap;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
TcbCap *TcbCap::create_supervised(uint64_t target_id,
                                  uint32_t target_gen,
                                  uint64_t grantee_id,
                                  uint32_t grantee_gen) {
// NOLINTEND(bugprone-easily-swappable-parameters)
    TcbCap *cap = tcb_cap_alloc(target_id, target_gen);
    if (cap == nullptr) {
        return nullptr;
    }
    cap->scope = static_cast<uint8_t>(Authority::SUPERVISOR);
    cap->grantee_id = grantee_id;
    cap->grantee_gen = grantee_gen;
    return cap;
}

void TcbCap::dispose() noexcept {
    kernel::test::ResourceTracker::instance().track_cap_object_remove();
    MemPool::free(this);
}

void TcbCap::revoke() noexcept {
    // Mark revoked so acquire() refuses and subsequent TCB_* lookups
    // fail closed.
    KernelObject::revoke();
}

} // namespace kernel::cap
