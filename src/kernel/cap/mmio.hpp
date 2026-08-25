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

/// @file mmio.hpp
/// @brief Capability-wrapped MMIO (BAR) range (v0.4.2, issue #3).
/// Wraps a PCI BAR range: memory BARs enable capability-gated MMIO page
/// mapping; IO BARs drive sys_ioport_grant (fine-grained I/O delegation).

#pragma once

#include <types.hpp>
#include <kernel/arch/pci.hpp>
#include <kernel/memory/kernel_object.hpp>

namespace kernel::cap {

/// @brief A capability-gated MMIO/BAR range.  Does NOT own physical memory
/// (device memory is never PMM-owned) — dispose only frees the MemPool
/// block.  Shared-heap class (FrameCap pattern).
class MmioCap : public KernelObject {
  public:
    /// @brief Physical base of the BAR range.
    uint64_t phys = 0;
    /// @brief Size of the BAR range in bytes.
    uint64_t size = 0;
    /// @brief BAR type (MEMORY_32 / MEMORY_64 / IO).
    arch::PciBarType bar_type = arch::PciBarType::MEMORY_32;

    /// @brief Allocates an MmioCap from the MemPool and pool-marks it.
    ///        Validates the range (IO ranges must lie in the 64 KiB port
    ///        space; memory ranges must be page-aligned).  Returns nullptr on
    ///        failure or when CONFIG_CAP_MAX_MMIO live objects are reached.
    static MmioCap *create(uint64_t phys, uint64_t size,
                           arch::PciBarType bar_type);

    /// @brief Convenience factory wrapping an already-parsed PCI BAR.
    static MmioCap *create_from_bar(const arch::PciBar &bar);

    /// @brief Final teardown: releases the MemPool block (device memory is
    ///        never freed).
    void dispose() noexcept override;

    /// @brief Genuinely shared (referenced by capability slots).
    bool is_shared() const noexcept override {
        return true;
    }
};

} // namespace kernel::cap