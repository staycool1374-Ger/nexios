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

#include <types.hpp>
#include <kernel/ipc/ipc_errors.hpp>
#include <kernel/task/task.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>

namespace kernel {
namespace cap {
class Endpoint;
}

/// @brief Flags for IPC::send().
// NOLINTNEXTLINE(performance-enum-size)
enum class IpcFlags : uint64_t {
    NONE = 0,          ///< blocking send (default).
    NONBLOCK = 1 << 0, ///< fail immediately when the queue is full.
};

/// @brief Bitwise-or for IPC flags.
[[nodiscard]] constexpr IpcFlags operator|(IpcFlags lhs,
                                           IpcFlags rhs) noexcept {
    return static_cast<IpcFlags>(kernel::to_underlying(lhs) |
                                 kernel::to_underlying(rhs));
}
/// @brief Bitwise-and for IPC flags (test/mask).
[[nodiscard]] constexpr IpcFlags operator&(IpcFlags lhs,
                                           IpcFlags rhs) noexcept {
    return static_cast<IpcFlags>(kernel::to_underlying(lhs) &
                                 kernel::to_underlying(rhs));
}

static_assert(kernel::to_underlying(IpcFlags::NONBLOCK) == 1ULL,
              "flag bit positions are ABI");

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
    /// M2 follow-up (#545 FIRE, INV-L4): the is_full_locked probe parks on
    /// a contended foreign-owned depth instead of spinning with IF=0; the
    /// IF=0 rollback in send() only triggers for genuine own-guard scopes
    /// (fail-closed) — unreachable-by-design on the syscall trap path.
    [[nodiscard]] static bool send(uint64_t dest_id, const Message &msg, IpcFlags flags = IpcFlags::NONE) noexcept;

    /// @brief Receives a message from the calling task's own queue.
    [[nodiscard]] static bool recv(Message &msg) noexcept;

    /// @brief Sends and blocks until a reply arrives.
    ///        Reply matching (issue #296): only a queued message whose
    ///        sender_id is @p dest_id is consumed as the reply; anything
    ///        else stays queued for its real consumer (signatures and
    ///        wire layout unchanged).
    /// @param reply_max_size If non-zero, the reply is only consumed when it
    ///        fits this budget (issue #11 fastpath); an oversized matching
    ///        reply stays queued with immediate false (no block).  Zero keeps
    ///        the full-path contract (consume the matching reply).
    [[nodiscard]] static bool send_sync(uint64_t dest_id, const Message &msg, Message &reply,
                           uint32_t reply_max_size = 0) noexcept;

    /// @brief Bounded send_sync for server proxies (issues #241, #48).
    ///        Mirrors send_sync exactly (reply matching, dest-death bail,
    ///        BLOCKED + dequeue_ready + reschedule, hlt wait) but arms a
    ///        bounded receive timeout (`timeout_ticks`, 0 = forever like
    ///        send_sync); expiry returns false (`-EAGAIN` at the caller),
    ///        never a silent success and never a held block. No IRQ window
    ///        widened vs send_sync. Generic IPC mechanism (no per-server
    ///        policy); first used by timer-srv, shared by driver PDs.
    [[nodiscard]] static bool send_sync_bounded(uint64_t dest_id,
                                  const Message &msg, Message &reply,
                                  uint64_t timeout_ticks) noexcept;

    /// @brief Typed-error core of send_sync (M1/F7, issues #559/#560).
    ///        Identical rendezvous semantics; returns IPC_ERR_OK on reply
    ///        consumed, IPC_ERR_NO_REPLY on clamped-oversize or dest-gone
    ///        without a queued reply, IPC_ERR_PEER_DIED when the
    ///        destination id is absent, TERMINATED, or generation-mismatched
    ///        (§12.2 PID-reuse close).  send_sync() is this returning == OK.
    [[nodiscard]] static errors::IpcError
    send_sync_err(uint64_t dest_id, const Message &msg, Message &reply,
                  uint32_t reply_max_size = 0) noexcept;

    /// @brief Typed-error core of send_sync_bounded.  Adds
    ///        IPC_ERR_TIMEOUT on bounded expiry (wheel arm cancelled);
    ///        IPC_ERR_NO_REPLY when the arm cannot be established.
    [[nodiscard]] static errors::IpcError
    send_sync_bounded_err(uint64_t dest_id, const Message &msg,
                          Message &reply, uint64_t timeout_ticks) noexcept;

    /// @brief Wake every reply waiter of a dying task (M1/F7, INV-MK-11).
    ///        READY-marks each live task with reply_wait set whose dest tag
    ///        names @p dead (id + generation); the waiter owns its
    ///        reply_wait clear and re-checks dest liveness at loop top, so a
    ///        reply queued before the death is still consumed first (#296).
    ///        Wake-only: never arms a switch, never changes BLOCKED state.
    ///        Caller must hold no scheduler lock.  The waiter's park loop
    ///        (resched_park_until_dispatched) observes the state flip as
    ///        its wake; the send_sync[_bounded] exit arms dequeue first if
    ///        queued so the RUNNING restore cannot strand a queued task.
    ///        Queue-full senders are intentionally left to the queue
    ///        destructor drain (F6 bisection: an early sender wake collapses
    ///        the BLOCKED window the handshake depends on).
    static void wake_reply_waiters_of(TaskControlBlock &dead) noexcept;

    /// @brief Record a delivered request's caller identity for REPLY_RECV
    ///        (M3, F3/Q2).  Stores sender id + generation (last-caller
    ///        pair).  Called on every consumed inbound message.  No locks
    ///        (caller owns its own TCB fields).
    static void note_delivered_request(TaskControlBlock &receiver,
                                       const Message &msg) noexcept;

    /// @brief Reply to the recorded caller and wait for the next request
    ///        (M3, N5 = syscall 97).  Requires a live last-caller pair
    ///        (else REPLY_NO_TOKEN) whose generation still matches and
    ///        whose ticket is still live (else REPLY_STALE, never blocks).
    ///        On success the reply is stamped with the one-shot token and
    ///        the waiter blocks for the next request exactly like a bounded
    ///        sys_receive wait (direct-switch entry included).
    /// @return IPC_ERR_OK (reply queued, next request consumed into @p msg),
    ///         REPLY_NO_TOKEN, REPLY_STALE, REPLY_BAD_ARGS, or the bounded
    ///         wait's dispositions.
    static errors::IpcError
    reply_recv_err(Message &reply, uint32_t reply_type, uint32_t reply_len,
                   Message &msg, uint64_t next_cap) noexcept;

    /// @brief Returns the message queue for a given task ID.
    static MessageQueue &queue(uint64_t task_id);

    /// @brief Sends a message through a capability-gated endpoint (ROADMAP
    ///        0.4.1 CSpace).  The endpoint holds the receiver binding; no
    ///        task-ID ambient authority is used.  Blocks on a full endpoint
    ///        queue unless IPC_NONBLOCK.  @p ep must be pinned (a live
    ///        capability lookup result).
    [[nodiscard]] static bool send_via_cap(cap::Endpoint *ep, const Message &msg,
                             IpcFlags flags = IpcFlags::NONE) noexcept;

    /// @brief Receives a message from a capability-gated endpoint into the
    ///        calling task's own queue.  Returns false if the endpoint queue
    ///        is empty.
    [[nodiscard]] static bool recv_via_cap(cap::Endpoint *ep, Message &msg) noexcept;

    /// @brief Wheel callback arming a bounded receive's expiry (issue #18).
    /// Flag-only: validates the TCB and generation, sets recv_timed_out.
    /// Takes no locks, no scheduler services, never reschedules or
    /// allocates (audit #17 callback contract). Runs in tick context.
    /// @param context Owning TaskControlBlock (nullable, fail-closed).
    static void recv_timeout_fire(void *context) noexcept;

    /// @brief Arm the calling task's receive timeout on the wheel.
    /// Sets recv_timed_out=false, snapshots the TCB generation, arms the
    /// wheel at now_ns+timeout converted from ticks. Fail-closed false on
    /// non-BSP CPU, full wheel, or zero CONFIG_TICK_HZ. No-op (returns
    /// true, no arm) when timeout_ticks is 0 — the 0=forever contract
    /// never arms. A false return means the caller must use a fallback
    /// that keeps the task RUNNING (never BLOCKED+dequeue): a blocked
    /// task has no waker and would park forever.
    /// @param task Waiting task (non-null, current task).
    /// @param timeout_ticks Budget in coarse ticks (0 = forever).
    /// @return true when armed or when no arm is needed (timeout 0).
    [[nodiscard]] static bool recv_wait_arm(TaskControlBlock &task,
                              uint64_t timeout_ticks) noexcept;

    /// @brief Cancel the calling task's receive timeout (idempotent).
    /// Disarms the TCB slot; stale handles fail closed inside.
    /// @param task Waiting task.
    static void recv_wait_cancel(TaskControlBlock &task) noexcept;

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

    /// @brief M2 follow-up evidence (#545 SILENT): pushes that found the
    ///        destination BLOCKED with none of the three wait flags set
    ///        (reply_wait / blocked_on_queue / blocked_in_recv) — the
    ///        lost-wakeup window probe.  CONFIG_DEBUG only; always 0 in a
    ///        healthy run.
    static uint64_t ipc_wake_gate_missed_count() noexcept;
    /// @brief M2 follow-up evidence (#545 SILENT): send_sync wait
    ///        publications that found a matching reply already queued — the
    ///        publish-vs-arrival window probe.  CONFIG_DEBUG only.
    static uint64_t ipc_block_with_pending_inbox_count() noexcept;
    /// @brief F1 follow-up evidence (#545, INV-4b): in-guard try-once
    ///        probe hits (consumed without blocking) and contended
    ///        outcomes.  CONFIG_DEBUG only; hits must be > 0 in a healthy
    ///        run (proves the Dekker path is live).
    static uint64_t ipc_probe_hit_count() noexcept;
    static uint64_t ipc_probe_contended_count() noexcept;
#if defined(CONFIG_DEBUG)
    /// @brief Record a try-once probe outcome (CONFIG_DEBUG only).
    static void ipc_probe_hit_mark() noexcept;
    static void ipc_probe_contended_mark() noexcept;
#else
    static void ipc_probe_hit_mark() noexcept {}
    static void ipc_probe_contended_mark() noexcept {}
#endif
};

} // namespace kernel
