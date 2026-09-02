#pragma once

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

/// @file ipc.hpp
/// @brief Priority-based IPC — send, recv, events, notifications.

#pragma once

#include <types.hpp>
#include <kernel/task/task.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>

namespace kernel {
namespace cap {
class Endpoint;
}

/// @brief Flags for IPC::send().
// NOLINTNEXTLINE(performance-enum-size)
enum IpcFlags : uint64_t {
    IPC_NONBLOCK = 1 << 0,
};

/// @brief MessageQueue is defined in <kernel/task/task.hpp> and embedded
/// in each TaskControlBlock.

/// @brief Inter-process communication manager.
class IPC {
  public:
    /// @brief Initialize the IPC subsystem (per-task message queues).
    static void init();

    /// @brief Sends a message to a destination task.
    static bool send(uint64_t dest_id, const Message &msg, uint64_t flags = 0);

    /// @brief Receives a message from the calling task's own queue.
    static bool recv(Message &msg);

    /// @brief Sends and blocks until a reply arrives.
    static bool send_sync(uint64_t dest_id, const Message &msg, Message &reply);

    /// @brief Returns the message queue for a given task ID.
    static MessageQueue &queue(uint64_t task_id);

    /// @brief Sends a message through a capability-gated endpoint (ROADMAP
    ///        0.4.1 CSpace).  The endpoint holds the receiver binding; no
    ///        task-ID ambient authority is used.  Blocks on a full endpoint
    ///        queue unless IPC_NONBLOCK.  @p ep must be pinned (a live
    ///        capability lookup result).
    static bool send_via_cap(cap::Endpoint *ep, const Message &msg,
                             uint64_t flags = 0);

    /// @brief Receives a message from a capability-gated endpoint into the
    ///        calling task's own queue.  Returns false if the endpoint queue
    ///        is empty.
    static bool recv_via_cap(cap::Endpoint *ep, Message &msg);

    /// @brief Blocks the current task on a full queue
    ///        (may boost owner priority).
    ///
    /// Maintains the scheduler's WEDGE-invariant:
    /// a BLOCKED task must never have `in_ready_queue_ == true`.
    /// This function calls `dequeue_ready()` before setting the state,
    /// so the ready-queue membership is revoked atomically with the
    /// BLOCKED transition.
    ///
    /// Precondition:  task must be a valid TCB, typically the caller
    ///                (= current task).  scheduler_lock_ must NOT be held.
    /// Postcondition: task.state == BLOCKED
    ///                task.in_ready_queue_ == false
    ///                task is appended to q.blocked_senders_{head,tail}
    ///                q.owner->priority may be boosted by priority inheritance
    ///
    /// Caller must invoke Scheduler::reschedule() after this function to
    /// request a deferred context switch (INV-4).  Do NOT set task.state
    /// to BLOCKED before calling dequeue_ready — the invariant requires
    /// a strict order: set BLOCKED, then dequeue.
    static bool block_sender(MessageQueue &q, TaskControlBlock &task);

    /// @brief Wakes the oldest blocked sender and
    /// restores owner priority.
    static void wake_sender(MessageQueue &q, TaskControlBlock &receiver);
};

} // namespace kernel
