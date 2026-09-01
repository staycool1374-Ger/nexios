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

/// @file syscall_handlers_irq.cpp
/// @brief SYS_IRQ_REGISTER / SYS_IRQ_WAIT handlers (issues #2/#7):
/// capability-gated user-space IRQ delivery.  sys_irq_register arms the
/// current task as the recipient of the IrqCap's vector; arg1 selects the
/// delivery mode (0 = WAIT — blocking sys_irq_wait, 1 = NOTIFY — IPC
/// notification on the task's Notify, issue #7).  sys_irq_wait blocks until
/// the IRQ fires (or returns immediately when one is pending); it refuses
/// slots armed in NOTIFY mode (the driver must use sys_notify_wait instead).
/// All dereferences through a capability slot validated by cap::lookup.  Both
/// returns are -1 on any validation failure (reachable states, never ENSURE).
/// Non-x86_64 builds return -1 (no user-deliverable PIC vectors).

#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/irq.hpp>
#include <kernel/irq_delivery.hpp>
#include <kernel/sync/spinlock_guard.hpp>

namespace kernel {

// Shared with syscall_handlers_cap.cpp (same TU-visible helper).
cap::CNode *current_cspace();

uint64_t Syscall::sys_irq_register(uint64_t cap_handle, uint64_t arg1,
                                   uint64_t, uint64_t, uint64_t *) {
#if defined(CONFIG_ARCH_X86_64)
    cap::CNode *src = current_cspace();
    if (!src)
        return static_cast<uint64_t>(-1);

    // The caller must hold an IrqCap with WRITE (the right to arm delivery).
    KernelObject *obj =
        cap::lookup(src, cap_handle, cap::CapType::Irq, cap::CAP_RIGHT_WRITE);
    if (!obj)
        return static_cast<uint64_t>(-1);
    auto *irq = static_cast<cap::IrqCap *>(obj);

    auto *t = Scheduler::current_task();
    if (!t || irq->reg_idx_ < 0) {
        obj->release();
        return static_cast<uint64_t>(-1);
    }

    // arg1 = delivery mode (issue #7): 0 = WAIT (blocking sys_irq_wait),
    // 1 = NOTIFY (IPC notification on the task's Notify).  arm() validates
    // the mode under the slot lock and rejects unknown values (fail closed).
    // Validate the RAW 64-bit arg1 BEFORE the narrowing cast to uint8_t: an
    // out-of-range value whose low byte wraps to 0 or 1 (e.g. 0x101 -> NOTIFY,
    // 0x100 -> WAIT) would otherwise defeat arm()'s fail-closed rejection.
    if (arg1 != 0ULL && arg1 != 1ULL) {
        obj->release();
        return static_cast<uint64_t>(-1);
    }
    auto mode = static_cast<IrqDeliveryMode>(arg1);

    // Slot-reuse safety (issue #2): arm() re-validates UNDER THE SLOT LOCK
    // that the slot at reg_idx_ still belongs to THIS cap and still carries
    // THIS cap's vector, so a slot drained at task death (or released by a
    // concurrent revoke) and REUSED for another vector can never be re-armed
    // from a stale reg_idx_ — the ownership check and the arming decision are
    // one atomic critical section, closing the slot_belongs_to->arm TOCTOU
    // (arming a different cap's slot would hijack its vector and starve its
    // legitimate owner; fail closed, never ENSURE).
    if (!IrqDelivery::arm(static_cast<int16_t>(irq->reg_idx_), *irq, *t,
                          mode)) {
        obj->release();
        return static_cast<uint64_t>(-1);
    }

    obj->release();
    return 0;
#else
    (void)cap_handle;
    (void)arg1;
    return static_cast<uint64_t>(-1);
#endif
}

uint64_t Syscall::sys_irq_wait(uint64_t cap_handle, uint64_t, uint64_t,
                               uint64_t, uint64_t *) {
#if defined(CONFIG_ARCH_X86_64)
    cap::CNode *src = current_cspace();
    if (!src)
        return static_cast<uint64_t>(-1);

    // READ is the right to observe (wait on) the IRQ delivery.
    KernelObject *obj =
        cap::lookup(src, cap_handle, cap::CapType::Irq, cap::CAP_RIGHT_READ);
    if (!obj)
        return static_cast<uint64_t>(-1);
    auto *irq = static_cast<cap::IrqCap *>(obj);

    auto *t = Scheduler::current_task();
    if (!t || irq->reg_idx_ < 0) {
        obj->release();
        return static_cast<uint64_t>(-1);
    }
    uint8_t vector = irq->vector;
    IrqRegistration *r = IrqDelivery::find(vector);
    if (!r || r->recipient != t) {
        // Not armed for this task — the recipient must match the waiter.
        obj->release();
        return static_cast<uint64_t>(-1);
    }

    // Blocked wait (Semaphore::wait() pattern): consume a pending IRQ if one
    // is already recorded, else register as the slot's waiter ONCE under the
    // lock, then dequeue + reschedule and spin on state==BLOCKED.  The timer
    // ISR applies the deferred switch (descheduling us); the ISR wake path
    // (or revoke/dispose) calls set_task_ready, restoring us to READY.  We
    // never re-register in a loop — that would hold the slot lock in a tight
    // spin and starve the waking peer (priority-inversion livelock).
    {
        SpinLockGuard<sync::SpinLock> guard(r->lock_);
        // Slot-reuse safety: re-validate under the lock that the slot still
        // carries THIS cap's vector, owner AND recipient before touching
        // pending/waiter (the find() above is a fast path; a concurrent
        // drain+reuse, or a shared cap armed by a different task, must not let
        // us consume or register against a foreign slot).  NOTIFY-armed slots
        // are refused in BOTH lock scopes: their delivery path bypasses
        // pending/waiter entirely (issue #7), so a WAIT would never wake.
        if (!r->occupied || r->vector != vector || r->owner != irq ||
            r->recipient != t ||
            r->delivery_mode != IrqDeliveryMode::WAIT) {
            obj->release();
            return static_cast<uint64_t>(-1);
        }
        if (r->pending > 0 && r->armed) {
            --r->pending;
            obj->release();
            return static_cast<uint64_t>(vector);
        }
        if (!r->armed) {
            obj->release();
            return static_cast<uint64_t>(-1);
        }
        r->waiter = t;
        r->waiter_gen = t->generation;
        t->state = TaskState::BLOCKED;
    }

    Scheduler::dequeue_ready(*t);
    Scheduler::reschedule();

    // Spin-wait (mirrors IPC::send / Semaphore::wait).  reschedule() is
    // deferred: the task physically continues until the timer ISR applies the
    // switch.  If interrupts are off the ISR cannot fire — roll back.
    if (arch::interrupts_enabled()) {
        while (t->state == TaskState::BLOCKED) {
            arch::pause();
        }
    } else {
        // Interrupts disabled: roll back to a consistent RUNNING state.
        SpinLockGuard<sync::SpinLock> guard(r->lock_);
        if (r->waiter == t) {
            r->waiter = nullptr;
            r->waiter_gen = 0;
        }
        t->state = TaskState::RUNNING;
        Scheduler::enqueue_ready(*t);
        obj->release();
        return static_cast<uint64_t>(-1);
    }

    // Woken: consume the pending IRQ (or fail closed if disarmed/revoked).
    // Slot-reuse safety (issue #2): while we slept the slot may have been
    // released (revoke/dispose/drain) and REUSED for a DIFFERENT vector or a
    // DIFFERENT cap.  Re-validate under the lock that it still carries OUR
    // vector and OUR owner, so we never consume a foreign vector's pending IRQ
    // nor clear another task's waiter registration (that would leave the
    // foreign waiter BLOCKED forever — a lost wakeup).  Only our own waiter
    // entry is cleared.
    uint64_t result = static_cast<uint64_t>(-1);
    {
        SpinLockGuard<sync::SpinLock> guard(r->lock_);
        if (r->occupied && r->vector == vector && r->owner == irq &&
            r->recipient == t &&
            r->delivery_mode == IrqDeliveryMode::WAIT) {
            if (r->pending > 0 && r->armed) {
                --r->pending;
                result = static_cast<uint64_t>(vector);
            }
            if (r->waiter == t) {
                r->waiter = nullptr;
                r->waiter_gen = 0;
            }
        }
    }
    obj->release();
    return result;
#else
    (void)cap_handle;
    return static_cast<uint64_t>(-1);
#endif
}

} // namespace kernel
