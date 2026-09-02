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

/// @file irq.hpp
/// @brief Capability-wrapped hardware IRQ vector (v0.4.2, issue #2).
/// Wraps a single x86_64 hardware IRQ vector (32–47 PIC window): owning the
/// cap is the authority to arm user-space delivery of that vector.  Single
/// owner per vector — create() fails closed when another live IrqCap already
/// claims the same vector.  The delivery table (kernel/irq_delivery.hpp) maps
/// an armed vector to its recipient task.

#pragma once

#include <types.hpp>
#include <kernel/memory/kernel_object.hpp>

namespace kernel::cap {

/// @brief A capability-gated hardware IRQ vector.  Does NOT own the IRQ line
/// (the PIC/APIC remains kernel-owned) — dispose only unregisters the vector
/// from the delivery table and frees the MemPool block.  Shared-heap class
/// (MmioCap pattern).
class IrqCap : public KernelObject {
  public:
    /// @brief The wrapped hardware IRQ vector (x86_64 PIC window 32–47).
    uint8_t vector = 0;
    /// @brief Index of this cap in the IRQ delivery table (-1 = not armed).
    int16_t reg_idx_ = -1;

    /// @brief Allocates an IrqCap from the MemPool and pool-marks it.
    ///        Validates the vector lies in the hardware IRQ window and that no
    ///        other live IrqCap claims the same vector (single-owner).  Also
    ///        rejects vectors already claimed by a threaded-IRQ handler.
    ///        Returns nullptr on failure or when CONFIG_CAP_MAX_IRQ live
    ///        objects are reached.
    static IrqCap *create(uint8_t vector);

    /// @brief Final teardown: unregisters the vector from the delivery table
    ///        (waking any blocked waiter with -1) and releases the MemPool
    ///        block.  Idempotent on an unarmed cap.
    void dispose() noexcept override;

    /// @brief Capability revocation: invalidates the cap and unregisters its
    ///        vector (disarm + wake any waiter).  A revoked cap refuses
    ///        acquire() and subsequent sys_irq_wait returns -1.
    void revoke() noexcept override;

    /// @brief Genuinely shared (referenced by capability slots).
    bool is_shared() const noexcept override {
        return true;
    }
};

} // namespace kernel::cap
