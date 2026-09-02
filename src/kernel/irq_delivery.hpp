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

/// @file irq_delivery.hpp
/// @brief User-space IRQ delivery table (v0.4.2, issues #2/#7).  A static,
/// bounded set of slots (CONFIG_CAP_MAX_IRQ) that maps an armed IRQ vector to
/// the task registered to receive it.  The ISR entry is invoked from
/// handle_interrupt_c BEFORE the threaded-IRQ path; it EOI-acks, records a
/// pending IRQ and wakes a blocked sys_irq_wait waiter under the slot lock
/// (no lost wakeup, no double delivery).  Issue #7 adds a NOTIFY delivery
/// mode that transforms the incoming interrupt into a capability-backed IPC
/// notification (the recipient task's Notify object) instead of the blocking
/// sys_irq_wait wait — eliminating Ring 0 driver execution for
/// notification-driven user-space drivers.

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/sync/spinlock.hpp>

namespace kernel {

struct TaskControlBlock;

namespace cap {
class IrqCap;
class MsixCap;
}

/// @brief Delivery mode of an armed IRQ slot (issue #7).  Bound atomically at
/// arm() time under the slot lock; immutable for the arm lifetime.
enum class IrqDeliveryMode : uint8_t {
    WAIT = 0,  ///< blocking sys_irq_wait delivery (issue #2, default)
    NOTIFY = 1 ///< capability-backed IPC notification to the recipient's
               ///< Notify (issue #7) — no pending/waiter bookkeeping
};

/// @brief Source kind of a claimed delivery slot (issue #10).  The PIC
/// (x86_64 IRQ lines 0–15 → vectors 33–47) has line-mask snapshot/restore
/// state; MSI-X vectors (48–255) mask/unmask their table entry instead.  All
/// mask logic is gated on this kind — a MSI-X vector must never run the
/// PIC `vector - 32` line arithmetic (would mask the wrong IRQ line).
enum class IrqSlotKind : uint8_t {
    PIC = 0,  ///< legacy PIC line (vectors 33–47)
    MSIX = 1  ///< MSI-X table entry (vectors 48–255, != 0x80)
};

/// @brief One armed IRQ delivery registration.
struct IrqRegistration {
    uint8_t vector = 0;        ///< hardware vector (PIC 33–47 or MSI-X 48–255)
    IrqSlotKind kind = IrqSlotKind::PIC; ///< PIC vs MSI-X (kind-gated masks)
    bool occupied = false;     ///< slot in use
    bool armed = false;        ///< vector unmasked + delivering to recipient
    bool line_was_masked = true; ///< PIC mask state of this line before arm
    IrqDeliveryMode delivery_mode = IrqDeliveryMode::WAIT; ///< arm-time mode
    KernelObject *owner = nullptr; ///< the cap that claimed this slot
    TaskControlBlock *recipient = nullptr; ///< task armed to receive
    uint64_t recipient_gen = 0;            ///< TCB generation at arm time
    uint32_t pending = 0;      ///< undelivered IRQ count (never lost)
    TaskControlBlock *waiter = nullptr;    ///< task blocked in sys_irq_wait
    uint64_t waiter_gen = 0;               ///< TCB generation at wait time
    sync::SpinLock lock_;      ///< serializes ISR vs syscall/teardown paths
};

/// @brief User-space IRQ delivery engine (issue #2).
///
/// All IRQ-state transitions happen under the slot's SpinLock.  The ISR and
/// the syscall path serialize on that one lock, so a pending IRQ can never be
/// lost between the pending-check and the waiter registration.  The wake path
/// rejects TERMINATED/REAPED tasks and mismatched generations (Notify
/// discipline) — a recycled TCB must never be fed to the scheduler.
class IrqDelivery {
  public:
    /// @brief Finds the occupied slot for @p vector, or nullptr.
    static IrqRegistration *find(uint8_t vector);

    /// @brief Allocates a free slot for @p vector (no vector duplication).
    /// @return slot index, or -1 when the table is full or the vector is
    ///         already occupied.
    static int16_t claim_slot(uint8_t vector);

    /// @brief Binds the owning cap to the slot at @p idx (called by
    ///        IrqCap/MsixCap::create after the cap object is constructed).
    ///        Guards slot-reuse safety: a slot can only be released/armed by
    ///        the cap that owns it.
    static void set_slot_owner(int16_t idx, KernelObject *owner);

    /// @brief Arms delivery of @p owner's vector to @p recipient (unmasks the
    ///        PIC line for PIC slots, unmasks the MSI-X table entry for MSIX
    ///        slots).  Fails if the vector is already armed, claimed by a
    ///        threaded-IRQ handler, or the slot no longer belongs to @p owner
    ///        (slot-reuse safety: the ownership + arming decision is ONE atomic
    ///        critical section — issue #2).
    /// @param reg_idx Slot index from claim_slot().
    /// @param owner   The cap that must own the slot (revalidated under lock).
    /// @param vector  The vector the owning cap claims (revalidated under lock
    ///                against the slot — issue #2 cross-vector hijack; used for
    ///                MSI-X entry unmask, issue #10).
    /// @param recipient The task armed to receive delivery.
    /// @param delivery_mode Delivery mode (issue #7); unknown modes are
    ///        rejected (fail closed).  Bound at arm time, immutable until
    ///        release.
    /// @return true on success.
    static bool arm(int16_t reg_idx, KernelObject &owner, uint8_t vector,
                    TaskControlBlock &recipient,
                    IrqDeliveryMode delivery_mode = IrqDeliveryMode::WAIT);

    /// @brief ISR entry — called from handle_interrupt_c.  Scans for an armed
    ///        slot, EOI-acks the controller, and dispatches per delivery_mode:
    ///        WAIT records a pending IRQ and wakes a blocked waiter; NOTIFY
    ///        signals the recipient's Notify with the vector value (no
    ///        pending/waiter bookkeeping — coalescing: last vector wins).
    /// @return true if the vector was consumed by an IrqCap registration (the
    ///         caller must NOT continue to the generic handler path).
    static bool isr_entry(uint8_t vector);

    /// @brief Disarms the slot at @p reg_idx (re-masks the line / MSI-X entry)
    ///        and wakes any blocked waiter with -1 (WAIT mode) or signals the
    ///        recipient's Notify with the revoked sentinel (NOTIFY mode, issue
    ///        #7).  Called from IrqCap/MsixCap::dispose/revoke via the cap's
    ///        stored reg_idx_.  Revalidates ownership under the lock (issue
    ///        #2): a slot drained-and-reused between an outer check and this
    ///        call is NOT released.  @p owner == nullptr releases an ownerless
    ///        claimed slot (create()'s alloc-failure path).
    /// @return true when the slot was actually released.
    static bool release_slot_idx(int16_t reg_idx, const KernelObject *owner);

    /// @brief Drains every slot whose recipient or waiter is @p tcb.  Called
    ///        from TaskControlBlock::cleanup() so a dying task can never leave
    ///        a dangling recipient/waiter in the delivery table.
    static void drain_task(TaskControlBlock &tcb);

    /// @brief Number of occupied slots (debug/leak audit helper).
    static size_t occupied_count();

  private:
    static IrqRegistration *slot(int16_t idx);
};

} // namespace kernel
