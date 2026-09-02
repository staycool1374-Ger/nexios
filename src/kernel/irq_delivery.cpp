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

/// @file irq_delivery.cpp
/// @brief User-space IRQ delivery table (issues #2/#7).  A static bounded
/// table of armed IRQ vectors mapping to their recipient task.  All state
/// transitions serialize on the per-slot SpinLock: the ISR and the
/// syscall/teardown paths share one lock, so a pending IRQ is never lost
/// between the pending-check and waiter registration, and a blocked
/// sys_irq_wait is woken exactly once (never left BLOCKED forever).  Issue #7
/// adds the NOTIFY delivery mode: the ISR transforms the interrupt into an IPC
/// notification on the recipient's Notify object (capability-backed, no Ring 0
/// driver execution).  The mode is read/written only under the slot lock.

#include <kernel/irq_delivery.hpp>
#include <kernel/cap/irq.hpp>
#include <kernel/cap/msix.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/hal/interrupt_controller.hpp>
#include <kernel/arch/hal/idt.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/irq_thread.hpp>

#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/apic.hpp>
#endif

namespace kernel {

/// @brief Hardware IRQ window (x86_64 PIC cascade: IRQ0-15 → vectors 32-47).
///        The timer vector (32) is reserved for the scheduler and must never
///        be handed to a user task — a user task claiming it would steal every
///        scheduler tick through the delivery hook.
constexpr uint8_t IRQ_VECTOR_MIN = 33;
constexpr uint8_t IRQ_VECTOR_MAX = 47;

/// @brief MSI-X vector window upper bound (x86_64: 0xFF is the highest vector).
///        MSI-X vectors live in 48–255, excluding the syscall vector 0x80.
constexpr uint8_t MSIX_VECTOR_MAX = 255;

/// @brief Value signalled to a NOTIFY-mode recipient's Notify when its slot
///        is released (revoke/dispose/drain): the waker-owns-wakeup contract
///        (CODING_STYLE §12.3).  0 is sync::NOTIFY_INVALID — outside the
///        vector range (33–255) and distinguishable from a real delivery.
constexpr uint64_t kIrqNotifyRevoked = 0;

/// @brief Static delivery table (no dynamic allocation on RT paths).
IrqRegistration g_irq_regs[CONFIG_CAP_MAX_IRQ];

IrqRegistration *IrqDelivery::slot(int16_t idx) {
    if (idx < 0 || idx >= static_cast<int16_t>(CONFIG_CAP_MAX_IRQ))
        return nullptr;
    return &g_irq_regs[idx];
}

/// @brief Restores the PIC line to its pre-arm mask state (or masks it when
///        @p force_mask — used on revoke/disarm where the line must never be
///        left delivering).  Mirrors the caller holding the slot lock.
///        PIC-kind only (issue #10): a MSI-X vector must never run the
///        `vector - 32` line arithmetic — it would mask a wrong PIC line.
static void restore_line_mask(IrqRegistration &r, bool force_mask) {
    if (r.kind != IrqSlotKind::PIC)
        return;
#if defined(CONFIG_ARCH_X86_64)
    uint8_t irq_line = static_cast<uint8_t>(r.vector - 32U);
    if (force_mask || r.line_was_masked) {
        arch::ArchInterruptController::mask(irq_line);
    }
#else
    (void)r;
    (void)force_mask;
#endif
}

IrqRegistration *IrqDelivery::find(uint8_t vector) {
    for (size_t i = 0; i < static_cast<size_t>(CONFIG_CAP_MAX_IRQ); ++i) {
        IrqRegistration &r = g_irq_regs[i];
        if (r.occupied && r.vector == vector)
            return &r;
    }
    return nullptr;
}

int16_t IrqDelivery::claim_slot(uint8_t vector) {
    // Single-owner per vector must be atomic vs a concurrent create (issue
    // #2).  This is a task-context leaf with no blocking or locks, so on the
    // uniprocessor an IrqGuard is sufficient to make the find->claim window
    // un-interleavable (same discipline as drain_zombie_list).
    arch::IrqGuard irq_guard{};
    // Vector window (fail closed): PIC lines (33–47) and MSI-X vectors
    // (48–255), issue #10.
    if (vector < IRQ_VECTOR_MIN)
        return -1;
    if (vector == static_cast<uint8_t>(arch::InterruptVector::SYSCALL))
        return -1; // 0x80 — software syscall vector, never an IRQ source
    if (vector > MSIX_VECTOR_MAX)
        return -1;
    // Timer vector must never be claimed.
    if (vector == static_cast<uint8_t>(arch::InterruptVector::TIMER))
        return -1;
#if defined(CONFIG_ARCH_X86_64)
    // Kernel-reserved vectors inside the MSI-X window (issue #10): the xAPIC
    // timer (64) drives the scheduler tick and 0xFF is the APIC spurious
    // vector — a user slot must never claim either (it would swallow the
    // scheduler tick / mis-route a spurious delivery).
    if (vector == static_cast<uint8_t>(arch::APIC::APIC_TIMER_VECTOR) ||
        vector == 0xFF) // arch::APIC::SPURIOUS_VECTOR (private member)
        return -1;
#endif
    // Reject vectors claimed by the threaded-IRQ path (single-owner).
    if (IrqThread::for_vector(vector) != nullptr)
        return -1;

    // Single-owner per vector: a live cap already claiming this vector
    // rejects the new claim (create() fails closed).
    if (find(vector) != nullptr)
        return -1;

    for (size_t i = 0; i < static_cast<size_t>(CONFIG_CAP_MAX_IRQ); ++i) {
        IrqRegistration &r = g_irq_regs[i];
        if (!r.occupied) {
            r.vector = vector;
            r.kind = (vector <= IRQ_VECTOR_MAX) ? IrqSlotKind::PIC
                                                : IrqSlotKind::MSIX;
            r.occupied = true;
            r.armed = false;
            r.line_was_masked = true;
            r.delivery_mode = IrqDeliveryMode::WAIT;
            r.owner = nullptr;
            r.recipient = nullptr;
            r.recipient_gen = 0;
            r.pending = 0;
            r.waiter = nullptr;
            r.waiter_gen = 0;
            return static_cast<int16_t>(i);
        }
    }
    return -1; // table full — reachable exhaustion, fail closed
}

void IrqDelivery::set_slot_owner(int16_t idx, KernelObject *owner) {
    IrqRegistration *r = slot(idx);
    if (!r)
        return;
    SpinLockGuard<sync::SpinLock> guard(r->lock_);
    if (!r->occupied)
        return;
    r->owner = owner;
}

bool IrqDelivery::arm(int16_t reg_idx, KernelObject &owner, uint8_t vector,
                      TaskControlBlock &recipient,
                      IrqDeliveryMode delivery_mode) {
    IrqRegistration *r = slot(reg_idx);
    if (!r)
        return false;
    // All IRQ-state transitions serialize on the per-slot lock (header
    // contract): re-validate occupied/armed AND ownership under the lock so a
    // concurrent revoke/dispose/drain + slot reuse cannot arm a FOREIGN cap's
    // vector.  The slot_belongs_to->arm check and the arming decision must be
    // one atomic critical section (issue #2, cross-vector hijack).
    SpinLockGuard<sync::SpinLock> guard(r->lock_);
    if (!r->occupied || r->armed || r->owner != &owner || r->vector != vector)
        return false;
    // Unknown delivery mode fails closed (issue #7): a mode must never be
    // stored that the ISR cannot dispatch on.
    if (delivery_mode != IrqDeliveryMode::WAIT &&
        delivery_mode != IrqDeliveryMode::NOTIFY)
        return false;
    r->delivery_mode = delivery_mode;
    r->recipient = &recipient;
    r->recipient_gen = recipient.generation;
    r->armed = true;
#if defined(CONFIG_ARCH_X86_64)
    if (r->kind == IrqSlotKind::PIC) {
        // Capture the line's current PIC mask so release restores the prior
        // state exactly (the boot PIC init unmasks all lines — an
        // unconditional re-mask on release would diverge from the pre-arm
        // state).
        uint8_t irq_line = static_cast<uint8_t>(r->vector - 32U);
        arch::IrqState cur = arch::ArchInterruptController::snapshot();
        uint8_t bit = static_cast<uint8_t>(1u << (irq_line & 7u));
        uint16_t m = (irq_line < 8) ? cur.pic1_mask : cur.pic2_mask;
        r->line_was_masked = (m & bit) != 0;
        arch::ArchInterruptController::unmask(irq_line);
    } else {
        // MSI-X: unmask the table entry so the vector can deliver to the
        // recipient (issue #10).  The owner is the MsixCap that programmed
        // the entry; only arming unmasks it (create leaves it masked).
        auto *msix = static_cast<cap::MsixCap *>(&owner);
        msix->set_entry_masked(false);
    }
#endif
    return true;
}

bool IrqDelivery::isr_entry(uint8_t vector) {
    IrqRegistration *r = find(vector);
    if (!r)
        return false;

    SpinLockGuard<sync::SpinLock> guard(r->lock_);

    // A claimed-but-unarmed vector must FALL THROUGH to the generic handler
    // (the kernel.cpp hook contract: "unarmed vectors fall through").  Never
    // EOI/consume an IRQ for a vector whose cap was created but never armed —
    // doing so would silently swallow the vector's IRQs and starve any kernel
    // handler on that line.  Re-validate the vector under the lock too: a slot
    // drained (at task death) and REUSED for a DIFFERENT vector between the
    // lockless find() above and this lock must not be consumed on behalf of
    // the old vector.  EOI then happens exactly once in the tail path.
    if (!r->armed || r->vector != vector)
        return false;

    // Record the pending IRQ first — never lost even with no waiter.
    if (r->delivery_mode == IrqDeliveryMode::WAIT) {
        ++r->pending;

        // Wake a blocked sys_irq_wait waiter (Notify discipline: reject dead /
        // recycled TCBs).
        if (r->waiter && r->waiter->state != TaskState::TERMINATED &&
            r->waiter->state != TaskState::REAPED &&
            r->waiter->generation == r->waiter_gen) {
            Scheduler::set_task_ready(*r->waiter);
            r->waiter = nullptr;
            r->waiter_gen = 0;
        }
    } else {
        // NOTIFY mode (issue #7): transform the interrupt into an IPC
        // notification on the recipient's Notify — no pending/waiter
        // bookkeeping (coalescing: the last vector value wins).  Lock order:
        // slot lock -> Notify lock (leaf; acyclic — notify() never touches the
        // slot table and is already invoked from ISR context today).  Reject
        // dead / recycled TCBs (Notify discipline); notify() additionally
        // guards its own waiter internally.
        if (r->recipient && r->recipient->state != TaskState::TERMINATED &&
            r->recipient->state != TaskState::REAPED &&
            r->recipient->generation == r->recipient_gen) {
            r->recipient->notify.notify(static_cast<uint64_t>(r->vector));
        }
    }

    // EOI exactly once for the consumed vector (the caller returns early, so
    // the handle_interrupt_c tail EOI never double-fires).
#if defined(CONFIG_ARCH_X86_64)
    if (arch::APIC::is_enabled()) {
        arch::APIC::eoi();
    }
    if (vector >= 32 && vector < 48) {
        arch::ArchInterruptController::eoi(vector);
    }
#endif
    return true;
}

bool IrqDelivery::release_slot_idx(int16_t reg_idx,
                                   const KernelObject *owner) {
    IrqRegistration *r = slot(reg_idx);
    if (!r)
        return false;
    SpinLockGuard<sync::SpinLock> guard(r->lock_);
    // Atomic ownership revalidation (issue #2): a slot drained at task death
    // and REUSED for another cap/vector between an outer check and this call
    // must not be released on behalf of a stale reg_idx_ (that would disarm
    // and wake the new owner's delivery).  nullptr releases an ownerless
    // claimed slot (create() alloc-failure path).
    if (!r->occupied || (owner != nullptr && r->owner != owner))
        return false;

    // Disarm and restore the prior mask state: re-mask the MSI-X table entry
    // (issue #10, kind-gated) or restore the PIC line mask.
    if (r->armed) {
        r->armed = false;
        if (r->kind == IrqSlotKind::MSIX) {
            auto *msix = static_cast<cap::MsixCap *>(r->owner);
            msix->set_entry_masked(true);
        }
        restore_line_mask(*r, false);
    }

    // Wakers own the wakeup (CODING_STYLE §12.3): never leave the driver
    // BLOCKED forever.  WAIT mode wakes a blocked sys_irq_wait with -1;
    // NOTIFY mode signals the recipient's Notify with the revoked sentinel
    // (0) so a blocked sys_notify_wait returns.  Capture the recipient/gen
    // BEFORE clearing the slot fields.
    if (r->delivery_mode == IrqDeliveryMode::NOTIFY) {
        TaskControlBlock *notify_target = r->recipient;
        uint64_t notify_gen = r->recipient_gen;
        if (notify_target && notify_target->state != TaskState::TERMINATED &&
            notify_target->state != TaskState::REAPED &&
            notify_target->generation == notify_gen) {
            notify_target->notify.notify(kIrqNotifyRevoked);
        }
    } else if (r->waiter && r->waiter->state != TaskState::TERMINATED &&
               r->waiter->state != TaskState::REAPED &&
               r->waiter->generation == r->waiter_gen) {
        Scheduler::set_task_ready(*r->waiter);
    }
    r->recipient = nullptr;
    r->recipient_gen = 0;
    r->owner = nullptr;
    r->waiter = nullptr;
    r->waiter_gen = 0;
    r->pending = 0;
    r->delivery_mode = IrqDeliveryMode::WAIT;
    r->occupied = false;
    return true;
}

void IrqDelivery::drain_task(TaskControlBlock &tcb) {
    for (size_t i = 0; i < static_cast<size_t>(CONFIG_CAP_MAX_IRQ); ++i) {
        IrqRegistration &r = g_irq_regs[i];
        SpinLockGuard<sync::SpinLock> guard(r.lock_);
        if (!r.occupied)
            continue;
        bool mine = (r.recipient == &tcb) || (r.waiter == &tcb);
        if (!mine)
            continue;

        // NOTIFY-mode ordering trap (issue #7): TaskControlBlock::cleanup()
        // destroys the task's Notify (task.cpp) BEFORE drain_task runs, so the
        // dying task's Notify object is already destroyed.  NEVER call
        // notify() on it — that is a use-after-free on a poisoned object.
        // The wakers-own-wakeup contract does not apply here: the drained task
        // is TERMINATED/REAPED and must never be fed to the scheduler anyway.
        if (r.delivery_mode == IrqDeliveryMode::NOTIFY && r.recipient != &tcb) {
            // Defensive branch (unreachable via the current syscall path where
            // the recipient is always the arming task): another task's Notify
            // must still be signalled so it is never left BLOCKED forever.
            if (r.recipient &&
                r.recipient->state != TaskState::TERMINATED &&
                r.recipient->state != TaskState::REAPED &&
                r.recipient->generation == r.recipient_gen) {
                r.recipient->notify.notify(kIrqNotifyRevoked);
            }
        }

        if (r.armed) {
            r.armed = false;
            if (r.kind == IrqSlotKind::MSIX) {
                auto *msix = static_cast<cap::MsixCap *>(r.owner);
                msix->set_entry_masked(true);
            }
            restore_line_mask(r, false);
        }
        r.recipient = nullptr;
        r.recipient_gen = 0;
        r.owner = nullptr;
        // A waiter that died is already TERMINATED/REAPED — no wake needed;
        // a waiter that is the drained task must not be fed to the scheduler.
        r.waiter = nullptr;
        r.waiter_gen = 0;
        r.pending = 0;
        r.delivery_mode = IrqDeliveryMode::WAIT;
        r.occupied = false;
    }
}

size_t IrqDelivery::occupied_count() {
    size_t n = 0;
    for (size_t i = 0; i < static_cast<size_t>(CONFIG_CAP_MAX_IRQ); ++i) {
        if (g_irq_regs[i].occupied)
            ++n;
    }
    return n;
}

} // namespace kernel
