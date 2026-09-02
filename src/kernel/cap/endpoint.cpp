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

/// @file endpoint.cpp
/// @brief Capability-backed IPC endpoint object implementation.

#include <kernel/cap/endpoint.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/test/resource_tracker.hpp>

// Placement new (defined in lib/new.cpp, no <new> header in freestanding)
inline void *operator new(unsigned long, void *placement) noexcept {
    return placement;
}

namespace kernel::cap {

Endpoint *Endpoint::create(uint32_t badge) {
    auto *endpoint = static_cast<Endpoint *>(MemPool::alloc(sizeof(Endpoint)));
    if (!endpoint)
        return nullptr;
    new (endpoint) Endpoint;
    endpoint->mark_pool_backed();
    endpoint->badge = badge;
    endpoint->q.init();
    endpoint->q.owner = nullptr;
    kernel::test::ResourceTracker::instance().track_cap_object_add();
    return endpoint;
}

void Endpoint::dispose() noexcept {
    // H-3 (audit-ipc-cap-syscalls-v0.4.2): a sender blocked in
    // IPC::send_via_cap on a full endpoint queue holds a raw `ep` pointer and
    // touches `ep->q` on wakeup.  The embedded MessageQueue destructor never
    // runs (MemPool::free bypasses it), so dispose() MUST drain the blocked
    // senders itself — mirroring MessageQueue::~MessageQueue — before the
    // block is freed, else the sender writes into freed memory.
    {
        SpinLockGuard<sync::SpinLock> guard(q.lock_);
        auto *task = q.blocked_senders_head;
        while (task) {
            auto *next = task->blocked_next;
            task->blocked_next = nullptr;
            task->blocked_on_queue = nullptr;
            // H-3: a cleaned-up task is REAPED, not TERMINATED.  Never feed a
            // freed TCB to set_task_ready (ready-queue corruption / UAF).
            if (task->state != TaskState::TERMINATED &&
                task->state != TaskState::REAPED)
                Scheduler::set_task_ready(*task);
            task = next;
        }
        q.blocked_senders_head = nullptr;
        q.blocked_senders_tail = nullptr;

        // M-4: undo the priority-inheritance boost applied to the owner by
        // block_sender() for every drained sender — with no senders left the
        // owner must return to its base priority (mirrors wake_sender's
        // restore-to-max-remaining, here restoring to base since none remain).
        if (q.owner) {
            TaskControlBlock &owner = *q.owner;
            arch::IrqGuard irq_guard{};
            uint64_t old_prio = Scheduler::effective_priority(&owner);
            owner.priority = owner.base_priority;
            uint64_t new_prio = Scheduler::effective_priority(&owner);
            if (old_prio != new_prio && owner.in_ready_queue_)
                Scheduler::move_priority(owner, old_prio, new_prio);
        }

        disposed_ = true; // publish BEFORE waking senders (they re-check it)
    }

    bound_receiver = nullptr;
    kernel::test::ResourceTracker::instance().track_cap_object_remove();
    MemPool::free(this);
}

} // namespace kernel::cap
