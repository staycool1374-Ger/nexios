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
/// notification on the task's Notify, issue #7, 2 = WAIT_ACK — blocking
/// wait with mask-on-delivery for PD-owned devices, issue #47 step-b2).
/// sys_irq_wait blocks until the IRQ fires (or returns immediately when one
/// is pending); arg1 bounds the wait in ticks (0 = infinite) — on expiry
/// it returns GENERIC_FAIL and the driver falls back to a bounded device
/// poll.  It refuses slots armed in NOTIFY mode (the driver must use
/// sys_notify_wait instead).
/// All dereferences through a capability slot validated by cap::lookup.  Both
/// returns are -1 on any validation failure (reachable states, never ENSURE).
/// Off-x86_64 the same machinery delivers GIC/PLIC platform lines via
/// IrqSlotKind::PLATFORM (issue #354); MSI-X paths fail closed there.

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_errors.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/irq.hpp>
#include <kernel/cap/msix.hpp>
#include <kernel/irq_delivery.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/arch/hal/irq_guard.hpp>

namespace kernel {

// Shared with syscall_handlers_cap.cpp (same TU-visible helper).
cap::CNode *current_cspace();

namespace {

/// @brief Looks up an IrqCap OR MsixCap in @p src (issue #10).  Both wrap a
///        hardware vector that the delivery table can arm for user-space
///        delivery via the same SYS_IRQ_REGISTER/SYS_IRQ_WAIT machinery.  No
///        RTTI — the concrete type is remembered in @p is_msix so the caller
///        extracts reg_idx_/vector via the correct static type.  Returns the
///        released KernelObject on success, nullptr on failure.
KernelObject *lookup_irq_like(cap::CNode *src, uint64_t cap_handle,
                              cap::CapRights rights, bool &is_msix) {
    KernelObject *obj =
        cap::lookup(src, cap_handle, cap::CapType::Irq, rights);
    if (obj) {
        is_msix = false;
        return obj;
    }
    obj = cap::lookup(src, cap_handle, cap::CapType::Msix, rights);
    if (obj) {
        is_msix = true;
        return obj;
    }
    return nullptr;
}

/// @brief Extracts the delivery-table index and vector from either cap type.
///        The slot index is the ownership anchor (issue #2); the vector is
///        the delivery-table key and MSI-X entry unmask target (issue #10).
bool irq_like_fields(KernelObject *obj, bool is_msix, int16_t &reg_idx,
                     uint8_t &vector) {
    if (is_msix) {
        auto *msix = static_cast<cap::MsixCap *>(obj);
        reg_idx = msix->reg_idx_;
        vector = msix->vector;
    } else {
        auto *irq = static_cast<cap::IrqCap *>(obj);
        reg_idx = irq->reg_idx_;
        vector = irq->vector;
    }
    return reg_idx >= 0;
}

} // namespace

uint64_t Syscall::sys_irq_register(uint64_t cap_handle, uint64_t arg1,
                                   uint64_t, uint64_t, uint64_t *) {
    cap::CNode *src = current_cspace();
    if (!src)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);

    // The caller must hold an IrqCap OR MsixCap with WRITE (the right to arm
    // delivery).
    bool is_msix = false;
    KernelObject *obj =
        lookup_irq_like(src, cap_handle, cap::CapRights::WRITE, is_msix);
    if (!obj)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);

    auto *t = Scheduler::current_task();
    int16_t reg_idx = -1;
    uint8_t vector = 0;
    if (!t || !irq_like_fields(obj, is_msix, reg_idx, vector)) {
        obj->release();
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }

    // arg1 = delivery mode (issue #7, extended issue #47 step-b2):
    // 0 = WAIT (blocking sys_irq_wait), 1 = NOTIFY (IPC notification),
    // 2 = WAIT_ACK (blocking wait with line/entry mask-on-delivery for
    // PD-owned devices).  arm() validates the mode under the slot lock
    // and rejects unknown values (fail closed).
    // Validate the RAW 64-bit arg1 BEFORE the narrowing cast to uint8_t: an
    // out-of-range value whose low byte wraps to 0, 1 or 2 (e.g. 0x101 ->
    // NOTIFY, 0x100 -> WAIT) would otherwise defeat arm()'s fail-closed
    // rejection.
    if (arg1 != 0ULL && arg1 != 1ULL && arg1 != 2ULL) {
        obj->release();
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
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
    if (!IrqDelivery::arm(reg_idx, *obj, vector, *t, mode)) {
        obj->release();
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }

    obj->release();
    return static_cast<uint64_t>(errors::SYS_ERR_OK);
}

uint64_t Syscall::sys_irq_wait(uint64_t cap_handle, uint64_t timeout_ticks,
                               uint64_t, uint64_t, uint64_t *) {
    cap::CNode *src = current_cspace();
    if (!src)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);

    // READ is the right to observe (wait on) the IRQ delivery.
    bool is_msix = false;
    KernelObject *obj =
        lookup_irq_like(src, cap_handle, cap::CapRights::READ, is_msix);
    if (!obj)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);

    auto *t = Scheduler::current_task();
    int16_t reg_idx = -1;
    uint8_t vector = 0;
    if (!t || !irq_like_fields(obj, is_msix, reg_idx, vector)) {
        obj->release();
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    IrqRegistration *r = IrqDelivery::find(vector);
    if (!r || r->recipient != t) {
        // Not armed for this task — the recipient must match the waiter.
        obj->release();
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    (void)reg_idx;

    // Blocked wait (Semaphore::wait() pattern): consume a pending IRQ if one
    // is already recorded, else register as the slot's waiter ONCE under the
    // lock, then dequeue + reschedule and spin on state==BLOCKED.  The timer
    // ISR applies the deferred switch (descheduling us); the ISR wake path
    // (or revoke/dispose) calls set_task_ready, restoring us to READY.  We
    // never re-register in a loop — that would hold the slot lock in a tight
    // spin and starve the waking peer (priority-inversion livelock).
    //
    // Issue #47 step-b2: WAIT_ACK slots are accepted like WAIT (NOTIFY is
    // still refused — its delivery path bypasses pending/waiter entirely).
    // On a WAIT_ACK entry the line/entry is unmasked: the driver ACKed the
    // device before waiting again (mask-on-delivery in the ISR keeps a
    // level-triggered source from storming between delivery and ACK).
    // timeout_ticks bounds the wait (0 = infinite); on expiry the waiter
    // wakes with GENERIC_FAIL and falls back to the bounded device poll.
#if !defined(CONFIG_ARCH_X86_64)
    // Unmask discipline mirrors arm() (issue #354): IrqGuard BEFORE the
    // slot lock off-x86 so this line's ISR cannot spin on the lock on the
    // same CPU.  x86 keeps lock-only (ISRs run with interrupts disabled).
    // SCOPED to the entry critical section: the guard must drop before
    // the BLOCKED spin below — a live guard across the spin keeps
    // interrupts disabled, starving the timer ISR that applies the
    // deferred switch (hard hang at the first true blocking wait).
#endif
    {
#if !defined(CONFIG_ARCH_X86_64)
        arch::IrqGuard irq_guard{};
#endif
        SpinLockGuard<sync::SpinLock> guard(r->lock_);
        // Slot-reuse safety: re-validate under the lock that the slot still
        // carries THIS cap's vector, owner AND recipient before touching
        // pending/waiter (the find() above is a fast path; a concurrent
        // drain+reuse, or a shared cap armed by a different task, must not let
        // us consume or register against a foreign slot).  NOTIFY-armed slots
        // are refused in BOTH lock scopes: their delivery path bypasses
        // pending/waiter entirely (issue #7), so a WAIT would never wake.
        if (!r->occupied || r->vector != vector || r->owner != obj ||
            r->recipient != t ||
            r->delivery_mode == IrqDeliveryMode::NOTIFY) {
            obj->release();
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        IrqDelivery::unmask_for_wait(*r);
        if (r->pending > 0 && r->armed) {
            --r->pending;
            obj->release();
            return static_cast<uint64_t>(vector);
        }
        if (!r->armed) {
            obj->release();
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        // Bounded wait: arm the wheel BEFORE blocking (fail closed — a
        // full wheel never parks the waiter).
        if (timeout_ticks != 0 &&
            !IrqDelivery::wait_timeout_arm(*t, timeout_ticks)) {
            obj->release();
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        r->waiter = t;
        r->waiter_gen = t->generation;
        t->state = TaskState::BLOCKED;
    }

    Scheduler::dequeue_ready(*t);
    Scheduler::reschedule();

    // Spin-wait (mirrors IPC::send / Semaphore::wait).  reschedule() is
    // deferred: the task physically continues until the timer ISR applies the
    // switch.
    // Issue #47 step-b2 (+ #513 IRQ discipline): trap entry owns the
    // IRQ-off state (DAIF set / IF clear, no locks held here), and a
    // BLOCKED spin with interrupts masked sleeps through every wakeup —
    // including the timer tick that applies the deferred switch and the
    // wheel expiry.  Without this, every userspace wait that finds no
    // pending IRQ rolls back spuriously (the PD can then only consume
    // already-arrived IRQs, never block for fresh ones).  Unmask before
    // spinning: safe with the slot lock dropped and the task dequeued
    // (an IRQ landing in the window wakes via the registered waiter).
    // The interrupts-disabled branch below stays as the fail-safe for
    // genuinely-IRQ-off kernel callers.  Mirrors IPC send_sync.
    arch::sti();
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
        IrqDelivery::wait_timeout_cancel(*t);
        t->state = TaskState::RUNNING;
        Scheduler::enqueue_ready(*t);
        obj->release();
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }

    // Woken: consume the pending IRQ (or fail closed if disarmed/revoked).
    // Slot-reuse safety (issue #2): while we slept the slot may have been
    // released (revoke/dispose/drain) and REUSED for a DIFFERENT vector or a
    // DIFFERENT cap.  Re-validate under the lock that it still carries OUR
    // vector and OUR owner, so we never consume a foreign vector's pending IRQ
    // nor clear another task's waiter registration (that would leave the
    // foreign waiter BLOCKED forever — a lost wakeup).  Only our own waiter
    // entry is cleared.
    // Bounded expiry (issue #47 step-b2): the tick applied the timeout wake
    // (irq_wait_timed_out set, no pending recorded).  Cancel the arm, clear
    // the flag, and return GENERIC_FAIL — the driver falls back to the
    // bounded device poll (a later IRQ still records pending for the next
    // wait entry, so no delivery is lost).
    // Fail-closed default: stays GENERIC_FAIL unless a pending IRQ for OUR
    // vector is consumed below (issue #2 slot-reuse re-validation).
    uint64_t result =
        static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    {
        SpinLockGuard<sync::SpinLock> guard(r->lock_);
        bool mine = r->occupied && r->vector == vector && r->owner == obj &&
                    r->recipient == t &&
                    r->delivery_mode != IrqDeliveryMode::NOTIFY;
        if (mine && __atomic_load_n(&t->irq_wait_timed_out,
                                    __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&t->irq_wait_timed_out, false,
                             __ATOMIC_RELEASE);
            IrqDelivery::wait_timeout_cancel(*t);
            if (r->waiter == t) {
                r->waiter = nullptr;
                r->waiter_gen = 0;
            }
            // A timeout that arrived alongside a genuine pending IRQ still
            // consumes it: the device DID fire, so report the vector.
            if (r->pending > 0 && r->armed) {
                --r->pending;
                result = static_cast<uint64_t>(vector);
            }
        } else if (mine) {
            if (r->pending > 0 && r->armed) {
                --r->pending;
                result = static_cast<uint64_t>(vector);
            }
            if (r->waiter == t) {
                r->waiter = nullptr;
                r->waiter_gen = 0;
            }
            IrqDelivery::wait_timeout_cancel(*t);
        }
    }
    obj->release();
    return result;
}

} // namespace kernel
