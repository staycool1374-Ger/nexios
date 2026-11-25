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
#include <kernel/syscall/syscall_errors.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/sporadic_server.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
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
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    for (size_t i = 0; i < msg.data_size; ++i) {
        msg.data[i] = data.read(i);
    }
    // ABI boundary (issue #348-D1): user passes raw u64 flags; only the
    // NONBLOCK bit is read downstream (legacy ignore-semantics).
    return IPC::send(dest_id, msg, static_cast<IpcFlags>(flags))
               ? static_cast<uint64_t>(errors::SYS_ERR_OK)
               : static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
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
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    uint8_t *raw_buf = buf.unsafe_ptr();
    Message msg{};
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    // Issue #513 INV-NO-BLOCK-DEAD: a non-RUNNING task (notably a
    // TERMINATED one resurrected by the fault-path fall-through) must
    // never enter an IPC wait — blocking it requeues a zombie.
    if (cur->state != TaskState::RUNNING)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    bool ok = false;
    bool was_blocked = false;
    if (IPC::recv_wait_arm(*cur, timeout_ticks)) {
        // Wheel-armed bounded wait (or 0 = forever, never armed).
        while (!(ok = (cur->state != TaskState::BLOCKED)
                           ? IPC::recv(msg)
                           : false)) {
            if (cur->state == TaskState::BLOCKED) {
                // INV-4b: a BLOCKED task takes no locks (F1 gate-denied
                // zombie window).  A denied reschedule returns with the
                // marking intact; go straight back to reschedule +
                // hlt-wait (paced, no hot spin) instead of taking.
                Scheduler::reschedule();
                arch::sti();
                arch::hlt();
                arch::cli();
                continue;
            }
            if (cur->get_sporadic_server()) {
                kernel::ScopedRef ss_ref{cur->get_sporadic_server()};
                cur->get_sporadic_server()->on_completion(
                    arch::Timer::ticks());
            }
            // Issue #208 audit (S3): publish the IPC wait channel BEFORE the
            // BLOCKED transition so an arrival landing between the two stores
            // still observes the channel (old code woke on state alone).
            // F1 (#545, INV-4b): the post-publish Dekker check as a
            // try-once probe INSIDE the publish guard, AFTER the channel
            // publish (Dekker order: publish then recheck — a probe
            // placed before the publish misses a push+read landing
            // between the two on SMP).  A message that arrived between
            // the loop-top check and the publish is consumed instead of
            // armed-and-slept-through — and with IF=0 the probe runs
            // while still RUNNING, so a tick cannot preempt the take
            // mid-critical-section (a preempted take while BLOCKED
            // strands the lock with no INV-C1 warning).  Contended =
            // live holder whose wake-gate read observes our publish
            // above, so no retry is needed.  A hit unpublishes the
            // channel (never BLOCKED — no waker can be owed one).
            bool probe_hit = false;
            {
                arch::IrqGuard publish_guard{};
                cur->blocked_in_recv = true;
                using QP = MessageQueue::QueueProbe;
                const QP probe = cur->msg_queue.probe_pop(msg);
                if (probe == QP::Consumed) {
                    IPC::ipc_probe_hit_mark();
                    cur->blocked_in_recv = false;
                    probe_hit = true;
                } else {
                    if (probe == QP::Contended)
                        IPC::ipc_probe_contended_mark();
                    cur->state = TaskState::BLOCKED;
                    Scheduler::dequeue_ready(*cur);
                }
            }
            // wake_sender half runs AFTER the guard (it takes the queue
            // lock itself): mirrors IPC::recv's pop+wake pairing.
            if (probe_hit) {
                ok = true;
                // M3 (F3/Q2): probe consumption recorded for REPLY_RECV.
                IPC::note_delivered_request(*cur, msg);
                if (cur->msg_queue.blocked_senders_head)
                    IPC::wake_sender(cur->msg_queue, *cur);
                break;
            }
            was_blocked = true;
            Scheduler::reschedule();
            // v0.4.0 MP-1: sti/hlt/cli is the USER-task blocked-wait
            // pattern; key on is_user_ (every task now owns a PML4).
            // Issue #513: the wait yields UNCONDITIONALLY.  A kernel-task
            // TCB reaching this loop (e.g. a user trap processed under a
            // stale current_task()) would otherwise spin with SIE=0 and
            // no wake source: no trap can fire, ticks freeze, the wheel
            // timeout can never expire — an unexitable wedge.  Trap entry
            // owns the IRQ state (SIE=0, no locks held), so sti is safe
            // for both flavors (idempotent when SIE is already 1).
            arch::sti();
            arch::hlt();
            arch::cli();
            // Resume: recheck the inbox FIRST (delivery beats timeout),
            // then the timeout flag; otherwise re-block on the live arm.
            // INV-4b: skip the take while still BLOCKED (gate-denied
            // reschedule + hlt wait) — the loop top re-checks after the
            // next wake instead.
            if (cur->state != TaskState::BLOCKED && IPC::recv(msg)) {
                ok = true;
                break;
            }
            if (__atomic_load_n(&cur->recv_timed_out, __ATOMIC_ACQUIRE)) {
                cur->blocked_in_recv = false;
                IPC::recv_wait_cancel(*cur);
                // Wait is over: restore RUNNING (the trapper resumes running;
                // a stale BLOCKED marking would mislead the next scheduler
                // decision — issue #513 follow-up).
                // sti: the hlt wake below parked with IF=0 (cli); the wait
                // is over so interrupts go back on (idempotent when already
                // enabled; trap iret restores entry state regardless).
                arch::sti();
                cur->state = TaskState::RUNNING;
                return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
            }
        }
        arch::sti(); // wait over via delivery-break: same IF restore.
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
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        const uint64_t fallback_deadline =
            arch::Timer::ticks() + timeout_ticks;
        while (!(ok = IPC::recv(msg))) {
            if (arch::Timer::ticks() >= fallback_deadline) {
                return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
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
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    return msg.type;
}

/// @brief (syscall_arg4 lives in syscall_helpers.hpp — single source.)

uint64_t Syscall::sys_send_sync(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                uint64_t arg3, uint64_t *regs) {
    uint64_t dest_id = arg0;
    uint64_t type = arg2;
    uint64_t data_size = arg3 < IPC_MAX_MSG_SIZE ? arg3 : IPC_MAX_MSG_SIZE;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto data = checked(reinterpret_cast<const uint8_t *>(arg1), data_size);
    if (!data.valid())
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    Message msg{};
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    // Issue #513 INV-NO-BLOCK-DEAD: a TERMINATED task must never block
    // in a send wait (resurrection vector, mirrors sys_receive).
    if (cur->state != TaskState::RUNNING)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    msg.sender_id = cur->id;
    msg.type = type;
    msg.data_size = data_size;
    for (size_t i = 0; i < data_size; ++i)
        msg.data[i] = data.read(i);
    Message reply{};
    if (!IPC::send_sync(dest_id, msg, reply))
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    // Issue #317 option (b): the reply payload is written to the caller's
    // REPLY BUFFER (5th arg / free register), NOT over the request buffer.
    // reply_buf == 0 => the caller requested no reply payload.  The reply is
    // bounded by the REQUEST size: reply_buf must be at least `data_size`
    // bytes (the same capacity the request check enforces) — this also fixes
    // the old unclamped `copy_size = reply.data_size` (a responder could once
    // write up to IPC_MAX_MSG_SIZE into a smaller request buffer).
    uint64_t reply_buf = syscall_arg4(regs);
    if (reply_buf != 0 && data_size != 0) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto reply_out =
            checked(reinterpret_cast<uint8_t *>(reply_buf), data_size);
        if (!reply_out.valid())
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        uint64_t copy_size = reply.data_size;
        if (copy_size > data_size)
            copy_size = data_size;
        for (size_t i = 0; i < copy_size; ++i)
            if (!reply_out.write(reply.data[i], i))
                return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL); // unmapped reply_buf: fail closed
    }
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
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    uint64_t dest_id = arg0;
    uint64_t data_size = arg2;
    if (data_size > IPC_FAST_PAYLOAD_BYTES)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    Message m{};
    m.sender_id = cur->id;
    m.type = arg1;
    m.data_size = data_size;
    (void)arg3; // payload word 0 rides in regs[4] (rsi) == arg3; gathered via regs[]
    fast_regs_to_msg(regs, data_size, m);
    return IPC::send(dest_id, m, IpcFlags::NONE)
               ? static_cast<uint64_t>(errors::SYS_ERR_OK)
               : static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
}

uint64_t Syscall::sys_recv_fast(uint64_t, uint64_t, uint64_t arg2,
                                uint64_t arg3, uint64_t *regs) {
    if (!regs)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    uint64_t max_size = arg2;
    uint64_t timeout_ticks = arg3;
    uint32_t clamp =
        max_size < IPC_FAST_PAYLOAD_BYTES
            ? static_cast<uint32_t>(max_size)
            : static_cast<uint32_t>(IPC_FAST_PAYLOAD_BYTES);
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    // Issue #513 INV-NO-BLOCK-DEAD: a TERMINATED task must never block
    // in a fast receive (resurrection vector, mirrors sys_receive).
    if (cur->state != TaskState::RUNNING)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    Message msg{};
    bool ok = false;
    bool was_blocked = false;
    if (IPC::recv_wait_arm(*cur, timeout_ticks)) {
        // Wheel-armed bounded wait (or 0 = forever, never armed).
        while (!(ok = (cur->state != TaskState::BLOCKED)
                           ? cur->msg_queue.pop_clamped(msg, clamp)
                           : false)) {
            if (cur->state == TaskState::BLOCKED) {
                // INV-4b: a BLOCKED task takes no locks (F1 gate-denied
                // zombie window).  Go straight back to reschedule +
                // hlt-wait instead of taking.
                Scheduler::reschedule();
                arch::sti();
                arch::hlt();
                arch::cli();
                continue;
            }
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
                // Wait is over: restore RUNNING (see sys_receive above).
                // sti: wait-loop exit restores IF (hlt/cli wake pattern —
                // see sys_receive; idempotent, trap iret restores anyway).
                arch::sti();
                cur->state = TaskState::RUNNING;
                return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
            }
            if (cur->get_sporadic_server()) {
                kernel::ScopedRef ss_ref{cur->get_sporadic_server()};
                cur->get_sporadic_server()->on_completion(
                    arch::Timer::ticks());
            }
            // Issue #208 audit (S3): publish the IPC wait channel BEFORE the
            // BLOCKED transition (same rationale as sys_receive above).
            // F1 (#545, INV-4b): the post-publish Dekker check as a
            // try-once probe INSIDE the publish guard, AFTER the channel
            // publish (Dekker order: publish then recheck — a probe
            // placed before the publish misses a push+read landing
            // between the two on SMP).  Contended = live holder whose
            // wake-gate read observes our publish above, so no retry is
            // needed.  A hit/oversized outcome unpublishes the channel
            // (never BLOCKED — no waker can be owed one).
            bool probe_hit = false;
            bool probe_oversized = false;
            {
                arch::IrqGuard publish_guard{};
                cur->blocked_in_recv = true;
                using QP = MessageQueue::QueueProbe;
                const QP probe =
                    cur->msg_queue.probe_pop_clamped(msg, clamp);
                if (probe == QP::Consumed) {
                    IPC::ipc_probe_hit_mark();
                    cur->blocked_in_recv = false;
                    probe_hit = true;
                } else if (probe == QP::PresentOversized) {
                    cur->blocked_in_recv = false;
                    probe_oversized = true;
                } else {
                    if (probe == QP::Contended)
                        IPC::ipc_probe_contended_mark();
                    cur->state = TaskState::BLOCKED;
                    Scheduler::dequeue_ready(*cur);
                }
            }
            if (probe_hit) {
                ok = true;
                // M3 (F3/Q2): record the delivered request for REPLY_RECV.
                IPC::note_delivered_request(*cur, msg);
                cur->blocked_in_recv = false;
                IPC::recv_wait_cancel(*cur);
                arch::sti();
                cur->state = TaskState::RUNNING;
                break;
            }
            if (probe_oversized) {
                cur->blocked_in_recv = false;
                IPC::recv_wait_cancel(*cur);
                arch::sti();
                cur->state = TaskState::RUNNING;
                return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
            }
            was_blocked = true;
            Scheduler::reschedule();
            // Issue #513: yield unconditionally (see sys_receive above).
            // The old bare-hlt else-branch slept forever when entered
            // with SIE=0 (trap entry state); sti first so the timer tick
            // that drives the wheel timeout can always fire.
            arch::sti();
            arch::hlt();
            arch::cli();
            // Resume: recheck the inbox FIRST (delivery beats timeout),
            // then the oversized/timeout exits; otherwise re-block.
            // INV-4b: skip the take while still BLOCKED (gate-denied
            // reschedule + hlt wait) — the loop top re-checks after the
            // next wake instead.
            if (cur->state != TaskState::BLOCKED &&
                cur->msg_queue.pop_clamped(msg, clamp)) {
                ok = true;
                // M3 (F3/Q2): record the delivered request for REPLY_RECV.
                IPC::note_delivered_request(*cur, msg);
                break;
            }
            if (!cur->msg_queue.is_empty()) {
                cur->blocked_in_recv = false;
                IPC::recv_wait_cancel(*cur);
                // Wait is over: restore RUNNING (see sys_receive above).
                arch::sti(); // wait-loop exit restores IF (see sys_receive).
                cur->state = TaskState::RUNNING;
                return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
            }
            if (__atomic_load_n(&cur->recv_timed_out, __ATOMIC_ACQUIRE)) {
                cur->blocked_in_recv = false;
                IPC::recv_wait_cancel(*cur);
                // Wait is over: restore RUNNING (see sys_receive above).
                arch::sti(); // wait-loop exit restores IF (see sys_receive).
                cur->state = TaskState::RUNNING;
                return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
            }
        }
        if (ok) {
            // M3 (F3/Q2): loop-top consumption recorded for REPLY_RECV
            // (probe and post-hlt breaks record at their own sites).
            IPC::note_delivered_request(*cur, msg);
        }
        arch::sti(); // wait over via delivery-break: same IF restore.
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
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        const uint64_t fallback_deadline =
            arch::Timer::ticks() + timeout_ticks;
        while (!(ok = cur->msg_queue.pop_clamped(msg, clamp))) {
            if (!cur->msg_queue.is_empty()) {
                return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
            }
            if (arch::Timer::ticks() >= fallback_deadline) {
                return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
            }
            Scheduler::reschedule();
            arch::pause();
        }
    }
    // M3 (F3/Q2): fallback-loop consumption recorded for REPLY_RECV.
    IPC::note_delivered_request(*cur, msg);
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
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    uint64_t dest_id = arg0;
    uint64_t data_size = arg2;
    if (data_size > IPC_FAST_PAYLOAD_BYTES)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
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
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    fast_msg_to_regs(reply, regs);
    return reply.type;
}

// ---------------------------------------------------------------------------
// M3 — REPLY_RECV (syscall 97, F3/Q2, issue #556).
// ---------------------------------------------------------------------------

uint64_t Syscall::sys_reply_recv(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                 uint64_t arg3, uint64_t *regs) {
    // arg0 = reply buffer (reply_len bytes), arg1 = reply type,
    // arg2 = reply len, arg3 = next-request buffer (IPC_MAX_MSG_SIZE),
    // arg4 (regs) = next_cap, reserved, must be 0 (fail-closed).
    const uint64_t next_cap = syscall_arg4(regs);
    if (next_cap != 0)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    // Issue #513 INV-NO-BLOCK-DEAD: a TERMINATED task must never block.
    if (cur->state != TaskState::RUNNING)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    const uint64_t reply_len = arg2;
    if (reply_len > IPC_MAX_MSG_SIZE)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    Message reply_out{};
    if (reply_len != 0) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto data = checked(reinterpret_cast<const uint8_t *>(arg0),
                            reply_len);
        if (!data.valid())
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        reply_out.type = arg1;
        reply_out.data_size = reply_len;
        for (uint64_t i = 0; i < reply_len; ++i)
            reply_out.data[i] = data.read(static_cast<size_t>(i));
    } else {
        reply_out.type = arg1;
        reply_out.data_size = 0;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto next_buf =
        checked(reinterpret_cast<uint8_t *>(arg3), IPC_MAX_MSG_SIZE);
    if (!next_buf.valid())
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    Message next_req{};
    const errors::IpcError err = IPC::reply_recv_err(
        reply_out, static_cast<uint32_t>(arg1),
        static_cast<uint32_t>(reply_len), next_req, next_cap);
    if (err != errors::IPC_ERR_OK)
        return static_cast<uint64_t>(errors::from_ipc_error(err));
    uint64_t copy_size = next_req.data_size;
    if (copy_size > IPC_MAX_MSG_SIZE)
        copy_size = IPC_MAX_MSG_SIZE;
    for (uint64_t i = 0; i < copy_size; ++i) {
        if (!next_buf.write(next_req.data[i], static_cast<size_t>(i)))
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    return next_req.type;
}

uint64_t Syscall::sys_create_mailbox(uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t *) {
    return static_cast<uint64_t>(errors::SYS_ERR_OK);
}

uint64_t Syscall::sys_destroy_mailbox(uint64_t, uint64_t, uint64_t, uint64_t,
                                      uint64_t *) {
    return static_cast<uint64_t>(errors::SYS_ERR_OK);
}

uint64_t Syscall::sys_buf_alloc(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                                uint64_t *) {
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(errors::SYS_ERR_OK);
    uint64_t va = arg0;
    // M4 (#563): typed path underneath (exhaustion latches pressure
    // inside alloc_err); the syscall ABI stays handle-or-0.
    uint64_t handle = 0;
    return BufferPool::alloc_err(*cur, va, handle) == errors::BUF_ERR_OK
               ? handle
               : static_cast<uint64_t>(errors::SYS_ERR_OK);
}

uint64_t Syscall::sys_buf_free(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                               uint64_t *) {
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    if (!BufferPool::free(*cur, arg0))
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    return static_cast<uint64_t>(errors::SYS_ERR_OK);
}

uint64_t Syscall::sys_buf_map(uint64_t arg0, uint64_t arg1, uint64_t, uint64_t,
                              uint64_t *) {
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    uint64_t handle = arg0;
    uint64_t va = arg1;
    if (!BufferPool::map(*cur, handle, va))
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    return static_cast<uint64_t>(errors::SYS_ERR_OK);
}

uint64_t Syscall::sys_buf_unmap(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                                uint64_t *) {
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    if (!BufferPool::unmap(*cur, arg0))
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    return static_cast<uint64_t>(errors::SYS_ERR_OK);
}

} // namespace kernel
