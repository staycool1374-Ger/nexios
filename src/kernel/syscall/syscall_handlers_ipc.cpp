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

/// @file syscall_handlers_ipc.cpp
/// @brief Syscall handlers for IPC operations: send, receive, sync send,
/// mailbox, notify, events.

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/sporadic_server.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/arch/io.hpp>

namespace kernel {

uint64_t Syscall::sys_send(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                           uint64_t arg3, uint64_t *) {
    uint64_t dest_id = arg0;
    uint64_t flags = 0;
    Message msg{};
    auto *cur = syscall_task();
    msg.sender_id = cur ? cur->id : 0;
    msg.type = arg2;
    msg.data_size = arg3 < IPC_MAX_MSG_SIZE ? arg3 : IPC_MAX_MSG_SIZE;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto data = checked(reinterpret_cast<const uint8_t *>(arg1), msg.data_size);
    if (!data.valid())
        return static_cast<uint64_t>(-1);
    for (size_t i = 0; i < msg.data_size; ++i) {
        msg.data[i] = data.read(i);
    }
    return IPC::send(dest_id, msg, flags) ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_receive(uint64_t, uint64_t arg1, uint64_t arg2,
                              uint64_t arg3, uint64_t *) {
    uint64_t max_size = arg2;
    // VULN-W3 (closed, issue #18): arg3 is the bounded-wait timeout in
    // ticks, armed on the event-timer wheel. 0 = block forever (no arm,
    // preserves current behaviour for non-real-time callers).
    uint64_t timeout_ticks = arg3;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto buf = checked(reinterpret_cast<uint8_t *>(arg1), max_size);
    if (!buf.valid())
        return static_cast<uint64_t>(-1);
    uint8_t *raw_buf = buf.unsafe_ptr();
    Message msg{};
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    bool ok = false;
    bool was_blocked = false;
    if (IPC::recv_wait_arm(*cur, timeout_ticks)) {
        // Wheel-armed bounded wait (or 0 = forever, never armed).
        while (!(ok = IPC::recv(msg))) {
            if (cur->get_sporadic_server()) {
                kernel::ScopedRef ss_ref{cur->get_sporadic_server()};
                cur->get_sporadic_server()->on_completion(
                    arch::Timer::ticks());
            }
            // Issue #208 audit (S3): publish the IPC wait channel BEFORE the
            // BLOCKED transition so an arrival landing between the two stores
            // still observes the channel (old code woke on state alone).
            cur->blocked_in_recv = true;
            cur->state = TaskState::BLOCKED;
            was_blocked = true;
            // M-5 (audit-ipc-cap-syscalls-v0.4.2): a BLOCKED task must
            // never be physically queued (INV-2/WEDGE invariant) — the
            // block in ipc.cpp's send path declares this, and sys_receive
            // was missing the dequeue.
            Scheduler::dequeue_ready(*cur);
            Scheduler::reschedule();
            // v0.4.0 MP-1: sti/hlt/cli is the USER-task blocked-wait
            // pattern; key on is_user_ (every task now owns a PML4).
            if (cur->is_user_) {
                arch::sti();
                arch::hlt();
                arch::cli();
            }
            // Resume: recheck the inbox FIRST (delivery beats timeout),
            // then the timeout flag; otherwise re-block on the live arm.
            if (IPC::recv(msg)) {
                ok = true;
                break;
            }
            if (__atomic_load_n(&cur->recv_timed_out, __ATOMIC_ACQUIRE)) {
                cur->blocked_in_recv = false;
                IPC::recv_wait_cancel(*cur);
                return static_cast<uint64_t>(-1);
            }
        }
        cur->blocked_in_recv = false;
        IPC::recv_wait_cancel(*cur);
    } else {
        // Fallback (wheel arm failed: full wheel / non-BSP). The task
        // stays RUNNING and polls: BLOCKED+dequeue would park it with no
        // waker (the deadline check lives in this task), hanging forever.
        // Absolute coarse deadline bounds the poll; reschedule() stays
        // polite to same-priority tasks. Degenerate no-tick config fails
        // fast (a frozen tick counter can never expire).
        if (CONFIG_TICK_HZ == 0) {
            return static_cast<uint64_t>(-1);
        }
        const uint64_t fallback_deadline =
            arch::Timer::ticks() + timeout_ticks;
        while (!(ok = IPC::recv(msg))) {
            if (arch::Timer::ticks() >= fallback_deadline) {
                return static_cast<uint64_t>(-1);
            }
            Scheduler::reschedule();
            arch::pause();
        }
    }
    if (was_blocked) {
        cur->remaining_ticks = cur->period_ticks;
        if (cur->get_sporadic_server()) {
            kernel::ScopedRef ss_ref{cur->get_sporadic_server()};
            cur->get_sporadic_server()->on_activation(arch::Timer::ticks());
        }
    }
    uint64_t copy_size = msg.data_size;
    if (copy_size > max_size)
        copy_size = max_size;
    // MP-4 (SMAP): safe_copy_to_user (stac-wrapped) instead of a raw write
    // loop into user memory.
    if (copy_size > 0 &&
        !safe_copy_to_user(raw_buf, msg.data, copy_size))
        return static_cast<uint64_t>(-1);
    return msg.type;
}

uint64_t Syscall::sys_send_sync(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                uint64_t arg3, uint64_t *) {
    uint64_t dest_id = arg0;
    uint64_t type = arg2;
    uint64_t data_size = arg3 < IPC_MAX_MSG_SIZE ? arg3 : IPC_MAX_MSG_SIZE;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto data = checked(reinterpret_cast<const uint8_t *>(arg1), data_size);
    if (!data.valid())
        return static_cast<uint64_t>(-1);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto data_rw = checked(reinterpret_cast<uint8_t *>(arg1), data_size);
    Message msg{};
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    msg.sender_id = cur->id;
    msg.type = type;
    msg.data_size = data_size;
    for (size_t i = 0; i < data_size; ++i)
        msg.data[i] = data.read(i);
    Message reply{};
    if (!IPC::send_sync(dest_id, msg, reply))
        return static_cast<uint64_t>(-1);
    uint64_t copy_size = reply.data_size;
    if (copy_size > IPC_MAX_MSG_SIZE)
        copy_size = IPC_MAX_MSG_SIZE;
    for (size_t i = 0; i < copy_size; ++i)
        data_rw.write(reply.data[i], i);
    return reply.type;
}

// ---------------------------------------------------------------------------
// Issue #11 — in-register IPC fastpath (docs/specs/ipc-fastpath.md §3).
// Pointer-free by construction: payload is gathered/scattered only in the
// caller's own regs[] kernel-stack frame — no checked_ptr, no SMAP window,
// no canary walk (FAST class).  Blocking semantics delegate to the existing
// IPC::send / IPC::recv / IPC::send_sync machinery unchanged (INV-3).
//
// ARCH NOTE (auditor S3): the k_fast_word_regs[] indices are x86_64-locked
// (rsi/rdi/r8/r9/r10/r11).  The libc wrappers are x86_64-only and the numbers
// are absent from the aarch64/riscv ABI, so the handlers are unreachable
// there — but the register map must never be interpreted on a non-x86_64
// frame.  The regs[] index table is the single source of truth (ipc.hpp).
// ---------------------------------------------------------------------------

uint64_t Syscall::sys_send_fast(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                uint64_t arg3, uint64_t *regs) {
    if (!regs)
        return static_cast<uint64_t>(-1);
    uint64_t dest_id = arg0;
    uint64_t data_size = arg2;
    if (data_size > IPC_FAST_PAYLOAD_BYTES)
        return static_cast<uint64_t>(-1);
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    Message m{};
    m.sender_id = cur->id;
    m.type = arg1;
    m.data_size = data_size;
    (void)arg3; // payload word 0 rides in regs[4] (rsi) == arg3; gathered via regs[]
    fast_regs_to_msg(regs, data_size, m);
    return IPC::send(dest_id, m, 0) ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_recv_fast(uint64_t, uint64_t, uint64_t arg2,
                                uint64_t arg3, uint64_t *regs) {
    if (!regs)
        return static_cast<uint64_t>(-1);
    uint64_t max_size = arg2;
    uint64_t timeout_ticks = arg3;
    uint32_t clamp =
        max_size < IPC_FAST_PAYLOAD_BYTES
            ? static_cast<uint32_t>(max_size)
            : static_cast<uint32_t>(IPC_FAST_PAYLOAD_BYTES);
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    Message msg{};
    bool ok = false;
    bool was_blocked = false;
    if (IPC::recv_wait_arm(*cur, timeout_ticks)) {
        // Wheel-armed bounded wait (or 0 = forever, never armed).
        while (!(ok = cur->msg_queue.pop_clamped(msg, clamp))) {
            // An oversized best match is NOT consumed (stays queued for a
            // full RECEIVE, INV-4) — disambiguate empty-vs-oversized with
            // is_empty().
            if (!cur->msg_queue.is_empty()) {
                // Issue #208 audit (S2): clear the recv channel marker on
                // every loop exit.  Past the first iteration the flag is
                // still set here; returning without clearing leaks a
                // stale-true flag, and a later non-IPC BLOCKED + message
                // arrival would then spuriously wake this task (#208 recurs).
                cur->blocked_in_recv = false;
                IPC::recv_wait_cancel(*cur);
                return static_cast<uint64_t>(-1);
            }
            if (cur->get_sporadic_server()) {
                kernel::ScopedRef ss_ref{cur->get_sporadic_server()};
                cur->get_sporadic_server()->on_completion(
                    arch::Timer::ticks());
            }
            // Issue #208 audit (S3): publish the IPC wait channel BEFORE the
            // BLOCKED transition (same rationale as sys_receive above).
            cur->blocked_in_recv = true;
            cur->state = TaskState::BLOCKED;
            was_blocked = true;
            Scheduler::dequeue_ready(*cur);
            Scheduler::reschedule();
            if (cur->is_user_) {
                arch::sti();
                arch::hlt();
                arch::cli();
            } else {
                arch::hlt();
            }
            // Resume: recheck the inbox FIRST (delivery beats timeout),
            // then the oversized/timeout exits; otherwise re-block.
            if (cur->msg_queue.pop_clamped(msg, clamp)) {
                ok = true;
                break;
            }
            if (!cur->msg_queue.is_empty()) {
                cur->blocked_in_recv = false;
                IPC::recv_wait_cancel(*cur);
                return static_cast<uint64_t>(-1);
            }
            if (__atomic_load_n(&cur->recv_timed_out, __ATOMIC_ACQUIRE)) {
                cur->blocked_in_recv = false;
                IPC::recv_wait_cancel(*cur);
                return static_cast<uint64_t>(-1);
            }
        }
        cur->blocked_in_recv = false;
        IPC::recv_wait_cancel(*cur);
    } else {
        // Fallback (wheel arm failed: full wheel / non-BSP). The task
        // stays RUNNING and polls: BLOCKED+dequeue would park it with no
        // waker (the deadline check lives in this task), hanging forever.
        // Absolute coarse deadline bounds the poll; reschedule() stays
        // polite to same-priority tasks. Degenerate no-tick config fails
        // fast (a frozen tick counter can never expire).
        if (CONFIG_TICK_HZ == 0) {
            return static_cast<uint64_t>(-1);
        }
        const uint64_t fallback_deadline =
            arch::Timer::ticks() + timeout_ticks;
        while (!(ok = cur->msg_queue.pop_clamped(msg, clamp))) {
            if (!cur->msg_queue.is_empty()) {
                return static_cast<uint64_t>(-1);
            }
            if (arch::Timer::ticks() >= fallback_deadline) {
                return static_cast<uint64_t>(-1);
            }
            Scheduler::reschedule();
            arch::pause();
        }
    }
    if (was_blocked) {
        cur->remaining_ticks = cur->period_ticks;
        if (cur->get_sporadic_server()) {
            kernel::ScopedRef ss_ref{cur->get_sporadic_server()};
            cur->get_sporadic_server()->on_activation(arch::Timer::ticks());
        }
    }
    // Parity with IPC::recv: wake a blocked sender on a full queue once we
    // drain one message (a slot opened up).
    if (cur->msg_queue.blocked_senders_head)
        IPC::wake_sender(cur->msg_queue, *cur);
    fast_msg_to_regs(msg, regs);
    return msg.type;
}

uint64_t Syscall::sys_send_sync_fast(uint64_t arg0, uint64_t arg1,
                                     uint64_t arg2, uint64_t arg3,
                                     uint64_t *regs) {
    if (!regs)
        return static_cast<uint64_t>(-1);
    uint64_t dest_id = arg0;
    uint64_t data_size = arg2;
    if (data_size > IPC_FAST_PAYLOAD_BYTES)
        return static_cast<uint64_t>(-1);
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    // Gather the request BEFORE IPC::send_sync — the frame is read-only after
    // the gather, so a block cannot invalidate the payload (INV-5).
    Message m{};
    m.sender_id = cur->id;
    m.type = arg1;
    m.data_size = data_size;
    (void)arg3; // payload word 0 rides in regs[4] (rsi) == arg3; gathered via regs[]
    fast_regs_to_msg(regs, data_size, m);
    Message reply{};
    if (!IPC::send_sync(dest_id, m, reply, IPC_FAST_PAYLOAD_BYTES))
        return static_cast<uint64_t>(-1);
    fast_msg_to_regs(reply, regs);
    return reply.type;
}

uint64_t Syscall::sys_create_mailbox(uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t *) {
    return 0;
}

uint64_t Syscall::sys_destroy_mailbox(uint64_t, uint64_t, uint64_t, uint64_t,
                                      uint64_t *) {
    return 0;
}

uint64_t Syscall::sys_buf_alloc(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                                uint64_t *) {
    auto *cur = syscall_task();
    if (!cur)
        return 0;
    uint64_t va = arg0;
    return BufferPool::alloc(*cur, va);
}

uint64_t Syscall::sys_buf_free(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                               uint64_t *) {
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    if (!BufferPool::free(*cur, arg0))
        return static_cast<uint64_t>(-1);
    return 0;
}

uint64_t Syscall::sys_buf_map(uint64_t arg0, uint64_t arg1, uint64_t, uint64_t,
                              uint64_t *) {
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    uint64_t handle = arg0;
    uint64_t va = arg1;
    if (!BufferPool::map(*cur, handle, va))
        return static_cast<uint64_t>(-1);
    return 0;
}

uint64_t Syscall::sys_buf_unmap(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                                uint64_t *) {
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(-1);
    if (!BufferPool::unmap(*cur, arg0))
        return static_cast<uint64_t>(-1);
    return 0;
}

} // namespace kernel
