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

/// @brief regs[] indices of the 6 fastpath payload words (issue #11, paper
///        §3.2): rsi(4), rdi(5), r8(7), r9(8), r10(9), r11(10).
///        Single source of truth shared by the fast handlers and the ABI test
///        (anti-drift).  rbp(6) is deliberately excluded (frame-pointer
///        safety); rcx(2)/rdx(3) carry type/data_size in the live ABI.
constexpr uint32_t k_fast_word_regs[IPC_FAST_PAYLOAD_BYTES / 8] = {4, 5,
                                                                   7, 8,
                                                                   9, 10};

/// @brief Copy up to @p data_size bytes of @p msg.data into the caller's own
///        @p regs[] kernel-stack frame (issue #11).  Kernel mem -> kernel mem,
///        no user-pointer access, no SMAP window.  regs must be non-null.
inline void fast_msg_to_regs(const Message &msg, uint64_t *regs) {
    for (size_t w = 0; w < sizeof(k_fast_word_regs) /
                              sizeof(k_fast_word_regs[0]);
         ++w) {
        uint64_t word = 0;
        size_t off = w * 8;
        if (off < msg.data_size) {
            size_t n = msg.data_size - off;
            if (n > 8)
                n = 8;
            for (size_t b = 0; b < n; ++b)
                word |= static_cast<uint64_t>(msg.data[off + b]) << (8 * b);
        }
        regs[k_fast_word_regs[w]] = word;
    }
}

/// @brief Gather up to @p data_size bytes from the caller's own @p regs[]
///        frame into @p msg.data (issue #11).  Kernel mem -> kernel mem.
///        regs must be non-null; data_size <= IPC_FAST_PAYLOAD_BYTES.
inline void fast_regs_to_msg(uint64_t *regs, size_t data_size, Message &msg) {
    for (size_t w = 0; w < sizeof(k_fast_word_regs) /
                              sizeof(k_fast_word_regs[0]);
         ++w) {
        uint64_t word = regs[k_fast_word_regs[w]];
        size_t off = w * 8;
        if (off >= data_size)
            break;
        size_t n = data_size - off;
        if (n > 8)
            n = 8;
        for (size_t b = 0; b < n; ++b)
            msg.data[off + b] = static_cast<uint8_t>((word >> (8 * b)) & 0xFF);
    }
}

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
    /// @param reply_max_size If non-zero, the reply is only consumed when it
    ///        fits this budget (issue #11 fastpath); an oversized reply stays
    ///        queued for a later RECEIVE.  Zero keeps the full-path contract
    ///        (consume any reply).  Default 0 => byte-identical to v1.
    static bool send_sync(uint64_t dest_id, const Message &msg, Message &reply,
                          uint32_t reply_max_size = 0);

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
