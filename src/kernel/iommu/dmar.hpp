/*
 * NexIOS RTOS — IOMMU DMA protection
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

/// @file dmar.hpp
/// @brief Pure-data result of the ACPI DMAR table scan (issue #9, phase-2
/// live VT-d).  The parser (arch/x86_64/acpi.cpp) fills this bounded struct;
/// IoMmuManager consumes it.  Deliberately a value type — no pointers into
/// the ACPI tables survive the parse (the tables live in low memory that
/// test isolation may rewind).

#pragma once

#include <types.hpp>

namespace kernel::iommu::dmar {

/// @brief One DMAR remapping unit (DRHD) — the VT-d MMIO base to program.
struct RemappingUnit {
    /// @brief Register base physical address (page-aligned, e.g. 0xFED90000).
    uint64_t base_phys = 0;
    /// @brief PCI segment number of the unit.
    uint16_t segment = 0;
    /// @brief True when a DRHD structure was parsed and base is valid.
    bool present = false;
    /// @brief True when the unit is INCLUDE_PCI_ALL (flags bit 0).
    bool include_pci_all = false;
};

/// @brief Result of the full DMAR scan: the first usable remapping unit.
struct DmarInfo {
    RemappingUnit unit;
    /// @brief True when a well-formed DMAR table with a DRHD was found.
    bool found = false;
    /// @brief True when the DMAR table is present but malformed (fail-closed
    ///        probe: never program a unit from a corrupt table).
    bool malformed = false;
};

} // namespace kernel::iommu::dmar