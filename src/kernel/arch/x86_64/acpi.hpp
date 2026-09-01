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

/// @file acpi.hpp
/// @brief Minimal ACPI/DMAR discovery for live VT-d (issue #9, phase-2).
///        GRUB provides the RSDP through multiboot2 tags 14 (ACPI old /
///        RSDP v1) and 15 (ACPI new / RSDP v2); the parser validates the
///        checksum, walks RSDT/XSDT to the DMAR table and extracts the
///        first DRHD remapping-unit base.  Fail-closed: any malformed /
///        checksum-mismatched table yields an empty DmarInfo (a corrupt
///        table must never program an IOMMU).  x86_64-only.

#pragma once

#include <kernel/iommu/dmar.hpp>

namespace kernel::iommu::acpi {

/// @brief Scans multiboot2 ACPI tags and the DMAR table for the first
///        remapping unit.  Returns a pure-data result (no pointers into
///        the tables survive the call).  Thread-safe by construction (no
///        shared state; the tables are static boot firmware data).
dmar::DmarInfo scan_dmar();

} // namespace kernel::iommu::acpi