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

/// @file thread_cap.cpp
/// @brief ThreadCap implementation (issue #53, option Y). Pool-allocated
/// wrapper carrying (target_id, target_gen); folds into the existing
/// cap_objects ResourceTracker counter. No registries: several wrappers
/// may name one thread, and no snapshot_reset is needed (nothing global).

#include <kernel/cap/thread_cap.hpp>
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

ThreadCap *ThreadCap::create(uint64_t target_id, uint32_t target_gen) {
    if (target_id == 0)
        return nullptr;
    auto *target = Scheduler::find_task(target_id);
    if (!target || target->magic != TaskControlBlock::TCB_MAGIC)
        return nullptr;
    if (!target->is_thread())
        return nullptr;
    if (target->generation != target_gen)
        return nullptr;
    auto *cap = static_cast<ThreadCap *>(MemPool::alloc(sizeof(ThreadCap)));
    if (!cap)
        return nullptr;
    new (cap) ThreadCap;
    cap->mark_pool_backed();
    cap->target_id = target_id;
    cap->target_gen = target_gen;
    kernel::test::ResourceTracker::instance().track_cap_object_add();
    return cap;
}

void ThreadCap::dispose() noexcept {
    kernel::test::ResourceTracker::instance().track_cap_object_remove();
    MemPool::free(this);
}

void ThreadCap::revoke() noexcept {
    // Mark revoked so acquire() refuses and lookup fails.
    KernelObject::revoke();
}

}  // namespace kernel::cap
