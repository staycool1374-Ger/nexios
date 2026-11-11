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
#include <kernel/arch/hal/irq_vectors.hpp>
#include <kernel/cap/irq.hpp>
#include <kernel/cap/msix.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/hal/interrupt_controller.hpp>
#include <kernel/arch/hal/idt.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/irq_thread.hpp>
#include <kernel/time/timer_wheel.hpp>
#include <kernel/arch/hal/timer.hpp>

#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/apic.hpp>
#endif
#if defined(CONFIG_ARCH_AARCH64)
#include <kernel/arch/aarch64/hal/gic.hpp>
#endif

namespace kernel {

/// @brief Hardware IRQ window (x86_64 PIC cascade: IRQ0-15 → vectors 32-47).
///        §1 freeze (ABI v1): vector 32 = TIMER = IRQ0, 33–47 = IRQ1–15
///        (`irq_line = vector - 32`); claim window starts at 33.
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
///        PLATFORM-kind only on non-x86 (issue #354): masks the controller
///        line directly.
static void restore_line_mask(IrqRegistration &r, bool force_mask) {
    if (r.kind == IrqSlotKind::MSIX)
        return;
#if defined(CONFIG_ARCH_X86_64)
    if (r.kind != IrqSlotKind::PIC)
        return;
    uint8_t irq_line = static_cast<uint8_t>(r.vector - 32U);
    if (force_mask || r.line_was_masked) {
        arch::ArchInterruptController::mask(irq_line);
    }
#else
    if (r.kind != IrqSlotKind::PLATFORM)
        return;
    if (force_mask || r.line_was_masked) {
        arch::ArchInterruptController::mask(r.vector);
    }
#endif
}

/// @brief Masks the slot's line/entry on WAIT_ACK delivery (issue #47
///        step-b2): the line stays masked until the driver's next
///        sys_irq_wait entry unmasks it after ACKing the device.  Prevents a
///        level-triggered storm between the IRQ and the device ACK.  Caller
///        holds the slot lock.  Kind-gated like restore_line_mask.
static void mask_on_delivery(IrqRegistration &r) {
    if (r.kind == IrqSlotKind::MSIX) {
        auto *msix = static_cast<cap::MsixCap *>(r.owner);
        if (msix)
            msix->set_entry_masked(true);
        return;
    }
#if defined(CONFIG_ARCH_X86_64)
    if (r.kind != IrqSlotKind::PIC)
        return;
    uint8_t irq_line = static_cast<uint8_t>(r.vector - 32U);
    arch::ArchInterruptController::mask(irq_line);
    // Issue #47 step-b2: on an APIC-enabled kernel the I/O APIC entry
    // is the live delivery path — mask it too, or a level-triggered
    // source storms between delivery and the device ACK.
    if (arch::APIC::is_enabled())
        arch::APIC::mask_irq(irq_line, true);
#else
    if (r.kind != IrqSlotKind::PLATFORM)
        return;
#if defined(CONFIG_ARCH_AARCH64)
    // GIC (level-triggered by arm, issue #47 step-b2): mask on delivery
    // so a held line cannot re-trap the CPU in a storm between delivery
    // and the driver's device ACK. The next wait entry unmasks.
    arch::ArchInterruptController::mask(r.vector);
#else
    // PLIC (edge semantics, no per-source trigger programming): do NOT
    // mask on delivery. An edge asserted during a masked window is lost
    // forever (no re-sample on unmask), and the block driver issues its
    // command BEFORE entering the wait — a fast synchronous completion
    // lands exactly in that window (convicted: first transfer after
    // (re)bind timed out deterministically, 8/8 green without the mask).
    // Storm protection stays with the storm limiter (masks + logs).
    (void)r;
#endif
#endif
}

/// @brief Unmask twin of mask_on_delivery for the WAIT_ACK wait entry
///        (issue #47 step-b2).  Caller holds the slot lock and the arch
///        unmask discipline (IrqGuard-before-lock off-x86, lock-only x86).
void IrqDelivery::unmask_for_wait(IrqRegistration &r) {
    if (!r.armed || r.delivery_mode != IrqDeliveryMode::WAIT_ACK)
        return;
    if (r.kind == IrqSlotKind::MSIX) {
        auto *msix = static_cast<cap::MsixCap *>(r.owner);
        if (msix)
            msix->set_entry_masked(false);
        return;
    }
#if defined(CONFIG_ARCH_X86_64)
    if (r.kind != IrqSlotKind::PIC)
        return;
    uint8_t irq_line = static_cast<uint8_t>(r.vector - 32U);
    arch::ArchInterruptController::unmask(irq_line);
    // Issue #47 step-b2: the I/O APIC entry is the live path — unmask
    // it alongside the PIC line now that the driver ACKed the device.
    if (arch::APIC::is_enabled())
        arch::APIC::mask_irq(irq_line, false);
#else
    if (r.kind != IrqSlotKind::PLATFORM)
        return;
    arch::ArchInterruptController::unmask(r.vector);
#endif
}

// I/O APIC redirection low-dword bits owned by the delivery table
// (issue #47 step-b2; mirrors apic.cpp to avoid depending on the HAL's
// private constants).
static constexpr uint32_t kIoapicLevelBit = 0x00008000U;
static constexpr uint32_t kIoapicMaskedBit = 0x00010000U;

/// @brief Restores the saved I/O APIC entry when this slot reprogrammed
///        it at arm (issue #47 step-b2). Caller holds the slot lock.
///        x86_64 PIC-kind only. The LEVEL bit this slot programmed is
///        always cleared (it is ours unconditionally); the mask bit is
///        restored to the pre-arm state so lines the kernel left
///        unmasked (IRQ0/1) — and their firmware trigger mode — survive.
static void restore_ioapic_mask(IrqRegistration &r) {
#if defined(CONFIG_ARCH_X86_64)
    if (r.kind == IrqSlotKind::PIC && r.ioapic_leveled) {
        const uint8_t irq_line = static_cast<uint8_t>(r.vector - 32U);
        uint32_t entry = arch::APIC::ioapic_entry(irq_line);
        entry &= ~kIoapicLevelBit;
        if (r.ioapic_unmasked_here)
            entry |= kIoapicMaskedBit;
        arch::APIC::ioapic_write_entry(irq_line, entry);
        r.ioapic_unmasked_here = false;
        r.ioapic_leveled = false;
    }
#else
    (void)r;
#endif
}

/// @brief Restores the saved ICFGR trigger field when the arm
/// reprogrammed it (issue #47 step-b2). Caller holds the slot lock.
/// aarch64 PLATFORM-kind only.
static void restore_gic_trigger(IrqRegistration &r) {
#if defined(CONFIG_ARCH_AARCH64)
    if (r.kind == IrqSlotKind::PLATFORM && r.gic_trigger_saved) {
        volatile uint32_t *w =
            arch::gicd_reg(arch::gicd_icfgr_word(r.vector));
        const uint32_t shift = arch::gicd_icfgr_shift(r.vector);
        *w = (*w & ~(0x3U << shift)) |
             ((static_cast<uint32_t>(r.gic_saved_trigger) & 0x3U) << shift);
        r.gic_trigger_saved = false;
        r.gic_saved_trigger = 0;
    }
#else
    (void)r;
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
#if defined(CONFIG_ARCH_X86_64)
    // Vector window (fail closed): PIC lines (33–47 = IRQ1–15) and MSI-X
    // vectors (48–255 minus §1 reservations), issue #10.
    if (vector < IRQ_VECTOR_MIN)
        return -1;
    if (vector > MSIX_VECTOR_MAX)
        return -1;
#else
    // Platform window (fail closed, issue #354): the cap window is the
    // whole claim window off-x86 (MSI unsupported — no MSIX kind exists).
    if (!arch::irq_cap_vector_valid(vector))
        return -1;
#endif
    // SYSCALL/TIMER rejections use per-arch values and stay unconditional:
    // riscv64 SYSCALL (= 2) lies INSIDE the PLIC window, so the check is
    // load-bearing off-x86 (gating it x86-only would open vector 2).
    if (vector == static_cast<uint8_t>(arch::InterruptVector::SYSCALL))
        return -1; // §1 sole live gate, never an IRQ source
    // Timer vector must never be claimed (scheduler tick).
    if (vector == static_cast<uint8_t>(arch::InterruptVector::TIMER))
        return -1;
#if defined(CONFIG_ARCH_X86_64)
    // Kernel-reserved vectors inside the MSI-X window (issue #10, §1
    // freeze): the xAPIC timer (APIC_TIMER_VECTOR = 0xE0 since #26; 64 is
    // ordinary/claimable) drives the scheduler tick and 0xFF is the APIC
    // spurious vector — a user slot must never claim either (it would
    // swallow the scheduler tick / mis-route a spurious delivery).
    if (vector == static_cast<uint8_t>(arch::APIC::APIC_TIMER_VECTOR) ||
        vector == 0xFF) // arch::APIC::SPURIOUS_VECTOR (private member)
        return -1;
#else
    // A vector with a live kernel handler is never claimable (single-owner).
    // Non-x86 only: x86 entries_ are always populated (hardware IDT), so
    // the probe is meaningless there — x86 keeps its explicit window +
    // reservation checks above.
    if (arch::IDT::has_handler(vector))
        return -1;
#endif
    // Reject vectors claimed by the threaded-IRQ handler (single-owner).
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
#if defined(CONFIG_ARCH_X86_64)
            r.kind = (vector <= IRQ_VECTOR_MAX) ? IrqSlotKind::PIC
                                                : IrqSlotKind::MSIX;
#else
            r.kind = IrqSlotKind::PLATFORM;
#endif
    r.occupied = true;
    r.armed = false;
    r.line_was_masked = true;
    r.delivery_mode = IrqDeliveryMode::WAIT;
    r.ioapic_unmasked_here = false;
    r.ioapic_leveled = false;
    r.gic_saved_trigger = 0;
    r.gic_trigger_saved = false;
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
#if !defined(CONFIG_ARCH_X86_64)
    // IrqGuard BEFORE the slot lock (issue #354): unmasking a platform line
    // while holding the slot lock would let this same line's ISR spin on
    // that lock on the same CPU (GIC/PLIC permit higher-priority
    // preemption). x86 keeps its exact prior ordering (lock only): x86 ISRs
    // run with interrupts disabled, so no same-CPU reentry is possible
    // there — widening its mask window would only add latency.
    arch::IrqGuard irq_guard{};
#endif
    // All IRQ-state transitions serialize on the per-slot lock (header
    // contract): re-validate occupied/armed AND ownership under the lock so a
    // concurrent revoke/dispose/drain + slot reuse cannot arm a FOREIGN cap's
    // vector.  The slot_belongs_to->arm check and the arming decision must be
    // one atomic critical section (issue #2, cross-vector hijack).
    SpinLockGuard<sync::SpinLock> guard(r->lock_);
    if (!r->occupied || r->owner != &owner || r->vector != vector)
        return false;
    // Idempotent re-arm (issue #47 step-b2, WAIT_ACK only): the same
    // owner re-arming the same vector for the same recipient in WAIT_ACK
    // mode is a no-op success (PD rebind/epoch flows re-arm without a
    // release cycle). WAIT keeps its proven fail-on-rearm contract
    // (issue #2 matrix test); anything else on an armed slot fails.
    if (r->armed) {
        return delivery_mode == IrqDeliveryMode::WAIT_ACK &&
               r->delivery_mode == IrqDeliveryMode::WAIT_ACK &&
               r->recipient == &recipient;
    }
    // Unknown delivery mode fails closed (issue #7): a mode must never be
    // stored that the ISR cannot dispatch on.
    if (delivery_mode != IrqDeliveryMode::WAIT &&
        delivery_mode != IrqDeliveryMode::NOTIFY &&
        delivery_mode != IrqDeliveryMode::WAIT_ACK)
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
        // Issue #47 step-b2: PIC delivery on an APIC-enabled kernel also
        // needs the I/O APIC entry unmasked (init masks entries 2-15;
        // the LAPIC never sees a masked entry however open the PIC is).
        // Program LEVEL-triggered delivery: the device holds the line
        // asserted until the driver ACKs it, and the PD issues the
        // command BEFORE entering the wait — an edge asserted during
        // the still-masked window would be lost, while level
        // re-samples on unmask (lossless). Only the two owned bits
        // change; the vector/dest/polarity are preserved in place, so
        // release clears exactly what arm set.
        if (arch::APIC::is_enabled()) {
            const uint32_t saved = arch::APIC::ioapic_entry(irq_line);
            r->ioapic_unmasked_here =
                (saved & kIoapicMaskedBit) != 0;
            r->ioapic_leveled = true;
            arch::APIC::ioapic_write_entry(
                irq_line,
                (saved & ~kIoapicMaskedBit) | kIoapicLevelBit);
        }
    } else {
        // MSI-X: unmask the table entry so the vector can deliver to the
        // recipient (issue #10).  The owner is the MsixCap that programmed
        // the entry; only arming unmasks it (create leaves it masked).
        auto *msix = static_cast<cap::MsixCap *>(&owner);
        msix->set_entry_masked(false);
    }
#else
    // PLATFORM (issue #354): unmask the controller line.  MSIX-kind slots
    // cannot exist off-x86 (MSI programming fails closed), so any other
    // kind here is a state violation — fail closed.
    if (r->kind != IrqSlotKind::PLATFORM)
        return false;
#if defined(CONFIG_ARCH_AARCH64)
    // Issue #47 step-b2: program level-triggered delivery for the
    // PD-owned line (save the firmware field first). The GIC init
    // programs virtio SPIs edge-high, but an edge asserted during the
    // masked window between the device IRQ and the next wait entry is
    // lost (proven: 1 completion then timeouts); level re-samples on
    // unmask because the driver ACKs the device before waiting again.
    // NOTE (audit S3): one ICFGR word covers 16 INTIDs, so this RMW
    // races a concurrent arm in the same word — accepted because PD
    // arms are single-threaded by construction (boot-time compose + one
    // rebind path per PD task; no concurrent arm path exists). A second
    // concurrent armer would need a global IRQ-table lock (new lock
    // order); revisit if arming ever leaves task context.
    // Runs before unmask() so its dsb orders both writes.
    {
        volatile uint32_t *w =
            arch::gicd_reg(arch::gicd_icfgr_word(r->vector));
        const uint32_t shift = arch::gicd_icfgr_shift(r->vector);
        r->gic_saved_trigger = static_cast<uint8_t>((*w >> shift) & 0x3U);
        r->gic_trigger_saved = true;
        *w = (*w & ~(0x3U << shift));
    }
#endif
    r->line_was_masked =
        !arch::ArchInterruptController::line_enabled(r->vector);
    arch::ArchInterruptController::unmask(r->vector);
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
    // WAIT_ACK (issue #47 step-b2) shares the WAIT accounting, then masks
    // the line/entry on level-triggered transports (x86 PIC/IOAPIC, GIC):
    // a level-held line must not re-fire between this delivery and the
    // driver's device ACK (which happens before the next sys_irq_wait
    // entry unmasks). PLIC (edge semantics) deliberately does NOT mask:
    // an edge asserted during a masked window is lost forever, and the
    // block driver issues its command BEFORE entering the wait.
    if (r->delivery_mode == IrqDeliveryMode::WAIT ||
        r->delivery_mode == IrqDeliveryMode::WAIT_ACK) {
        ++r->pending;
        if (r->delivery_mode == IrqDeliveryMode::WAIT_ACK)
            mask_on_delivery(*r);

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
    // Issue #26 class rule: this path never raises TPR itself.  PIC-kind
    // vectors (33-47) are served at/below TPR_CLASS_PIC and MSI-X vectors
    // at/below the caller's shadow; any band narrowing inside a handler
    // uses TprGuard (RAII, self-restoring).
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
#if !defined(CONFIG_ARCH_X86_64)
    // Same IrqGuard-before-lock discipline as arm() (issue #354).
    arch::IrqGuard irq_guard{};
#endif
    SpinLockGuard<sync::SpinLock> guard(r->lock_);
    // Atomic ownership revalidation (issue #2): a slot drained at task death
    // and REUSED for another cap/vector between an outer check and this call
    // must not be released on behalf of a stale reg_idx_ (that would disarm
    // and wake the new owner's delivery).  nullptr releases an ownerless
    // claimed slot (create() alloc-failure path).
    if (!r->occupied || (owner != nullptr && r->owner != owner))
        return false;

    // Disarm and restore the prior mask state: re-mask the MSI-X table entry
    // (issue #10, kind-gated) or restore the PIC line mask.  WAIT_ACK slots
    // (issue #47 step-b2) force-mask: the line was masked on delivery and
    // the dead/gone recipient will never ACK the device, so restoring the
    // pre-arm (possibly unmasked) state would leave a level-triggered line
    // storming with no consumer.
    if (r->armed) {
        r->armed = false;
        if (r->kind == IrqSlotKind::MSIX) {
            auto *msix = static_cast<cap::MsixCap *>(r->owner);
            msix->set_entry_masked(true);
        }
        restore_line_mask(*r, r->delivery_mode == IrqDeliveryMode::WAIT_ACK);
        // I/O APIC lifetime restore (issue #47 step-b2).
        restore_ioapic_mask(*r);
        // GIC trigger restore (issue #47 step-b2).
        restore_gic_trigger(*r);
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
#if !defined(CONFIG_ARCH_X86_64)
    // Same IrqGuard-before-lock discipline as arm() (issue #354).  Bounded:
    // CONFIG_CAP_MAX_IRQ iterations of lock-only work, no blocking.
    arch::IrqGuard irq_guard{};
#endif
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
            // WAIT_ACK force-mask (issue #47 step-b2): same storm
            // reasoning as release_slot_idx — the drained recipient will
            // never ACK the device.
            bool force = r.delivery_mode == IrqDeliveryMode::WAIT_ACK;
            restore_line_mask(r, force);
            // I/O APIC lifetime restore (issue #47 step-b2).
            restore_ioapic_mask(r);
            // GIC trigger restore (issue #47 step-b2).
            restore_gic_trigger(r);
        }
        r.recipient = nullptr;
        r.recipient_gen = 0;
        r.owner = nullptr;
        // A waiter that died is already TERMINATED/REAPED — no wake needed;
        // a waiter that is the drained task must not be fed to the scheduler.
        // Cancel its wheel arm so no stale expiry outlives the drain
        // (issue #47 step-b2).
        if (r.waiter)
            wait_timeout_cancel(*r.waiter);
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

/// @brief Wheel callback for a bounded IRQ wait (issue #47 step-b2).
/// Lock-free by contract (runs under scheduler_lock_ in on_tick): only
/// sets the flag; the wake is applied in scheduler_tick.  Stale fires
/// (recycled TCB, cancelled arm) are no-ops.
void IrqDelivery::wait_timeout_fire(void *context) noexcept {
    auto *task = static_cast<TaskControlBlock *>(context);
    if (!TaskControlBlock::is_valid(task))
        return;
    if (task->generation != task->irq_wait_gen)
        return;
    if (!__atomic_load_n(&task->irq_wait_armed, __ATOMIC_ACQUIRE))
        return;
    __atomic_store_n(&task->irq_wait_timed_out, true, __ATOMIC_RELEASE);
}

bool IrqDelivery::wait_timeout_arm(TaskControlBlock &task,
                                   uint64_t timeout_ticks) {
    // Consume any stale expiry from a previous wait first (mirrors
    // IPC::recv_wait_arm: the resume path only sets this flag).
    __atomic_store_n(&task.irq_wait_timed_out, false, __ATOMIC_RELEASE);
    if (timeout_ticks == 0)
        return true;
    if (CONFIG_TICK_HZ == 0)
        return false;
    // Ticks to ns without 64-bit overflow (decompose around 1e9).
    const uint64_t whole = timeout_ticks / CONFIG_TICK_HZ;
    const uint64_t part = timeout_ticks % CONFIG_TICK_HZ;
    uint64_t budget_ns = 0;
    if (whole >= 0xFFFFFFFFFFFFFFFFULL / 1000000000ULL) {
        budget_ns = 0xFFFFFFFFFFFFFFFFULL;
    } else {
        budget_ns =
            whole * 1000000000ULL + (part * 1000000000ULL) / CONFIG_TICK_HZ;
    }
    if (budget_ns == 0)
        budget_ns = 1;
    const uint64_t now_ns = arch::Timer::ns_monotonic();
    // Saturating add: absurd budgets clamp to never-expire instead of
    // wrapping into an immediate deadline.
    uint64_t expiry_ns = now_ns + budget_ns;
    if (expiry_ns < now_ns)
        expiry_ns = 0xFFFFFFFFFFFFFFFFULL;
    task.irq_wait_gen = task.generation;
    // Defensive cancel-first: never overwrite a live arm.
    if (__atomic_load_n(&task.irq_wait_armed, __ATOMIC_ACQUIRE))
        wait_timeout_cancel(task);
    if (!time::TimerWheel::arm(0, expiry_ns, wait_timeout_fire, &task,
                               &task.irq_wait_handle)) {
        __atomic_store_n(&task.irq_wait_armed, false, __ATOMIC_RELEASE);
        return false;
    }
    __atomic_store_n(&task.irq_wait_armed, true, __ATOMIC_RELEASE);
    return true;
}

void IrqDelivery::wait_timeout_cancel(TaskControlBlock &task) {
    if (__atomic_load_n(&task.irq_wait_armed, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&task.irq_wait_armed, false, __ATOMIC_RELEASE);
        time::TimerWheel::cancel(task.irq_wait_handle);
    }
}

} // namespace kernel
