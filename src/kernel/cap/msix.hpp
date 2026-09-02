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

/// @file msix.hpp
/// @brief Capability-wrapped per-vector MSI-X entry (v0.4.2, issue #10).
/// Wraps one MSI-X vector/entry on a PCI device.  Owning the cap is the
/// authority to arm user-space delivery of that MSI-X vector via the shared
/// IrqDelivery table + SYS_IRQ_REGISTER/SYS_IRQ_WAIT (generalized to accept
/// both IrqCap and MsixCap).  The MSI-X table entry is programmed (masked) at
/// create time and unmasks only while armed.  Single owner per vector —
/// create() fails closed when another live cap (IrqCap or MsixCap) already
/// claims the same vector.  VT-d interrupt remapping is explicitly out of
/// scope (no IR — MSI-X messages go straight to the xAPIC, correct for BSP).

#pragma once

#include <types.hpp>
#include <kernel/arch/pci.hpp>
#include <kernel/memory/kernel_object.hpp>

namespace kernel::cap {

/// @brief A capability-gated MSI-X vector on a PCI device.  Does NOT own the
/// MSI-X table memory (device memory is never PMM-owned) — dispose only
/// re-masks the entry, releases the delivery-table slot, frees the vector and
/// the MemPool block.  Shared-heap class (IrqCap pattern).
class MsixCap : public KernelObject {
  public:
    /// @brief Device BDF carrying the MSI-X capability.
    arch::PciBdf bdf{};
    /// @brief MSI-X table entry index (0-based, < entry_count).
    uint16_t entry_index = 0;
    /// @brief Allocated MSI-X vector (48–255, != 0x80).
    uint8_t vector = 0;
    /// @brief Index of this cap in the IRQ delivery table (-1 = not claimed).
    int16_t reg_idx_ = -1;
    /// @brief Parsed MSI-X table info (BAR/entry_count/table KVA) at create.
    arch::PciMsixTableInfo tbl_{};

    /// @brief Allocates an MsixCap from the MemPool and pool-marks it.
    ///        Validates the BDF bounds, MSI-X capability presence, entry
    ///        bounds and that no other live cap claims the same vector.
    ///        Programs the table entry MASKED (fail-closed) and enables the
    ///        MSI-X function.  Returns nullptr on failure or when
    ///        CONFIG_CAP_MAX_MSIX live objects are reached.
    static MsixCap *create(const arch::PciBdf &bdf, uint16_t entry_index);

    /// @brief Final teardown: re-masks the entry, unregisters the vector from
    ///        the delivery table (waking any blocked waiter with -1), frees
    ///        the vector and the MemPool block.  Idempotent on an unclaimed
    ///        cap.
    void dispose() noexcept override;

    /// @brief Capability revocation: invalidates the cap, re-masks the entry
    ///        and unregisters its vector (disarm + wake any waiter).  A
    ///        revoked cap refuses acquire() and subsequent sys_irq_wait
    ///        returns -1.
    void revoke() noexcept override;

    /// @brief Sets/clears the mask bit of this cap's MSI-X table entry.
    ///        Called by IrqDelivery under the slot lock on arm/release/drain
    ///        (issue #10).  @p masked = true blocks delivery.
    void set_entry_masked(bool masked);

    /// @brief True when this cap's MSI-X table entry is currently masked.
    bool entry_masked() const;

    /// @brief Clears the static per-(BDF, entry) claim registry (test
    ///        isolation snapshot restore).  The snapshot rewinds MsixCap
    ///        blocks and delivery slots but not this static; without the
    ///        reset a recycled (bdf, entry) would stay un-claimable.
    static void snapshot_reset();

    /// @brief Genuinely shared (referenced by capability slots).
    bool is_shared() const noexcept override {
        return true;
    }
};

} // namespace kernel::cap
