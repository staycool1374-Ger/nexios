/*
 * NexIOS RTOS — SMP bring-up (Phase B)
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

/// @file madt.hpp
/// @brief Pure-data result of the ACPI MADT scan (issue #25, Phase B).
///        The parser (arch/x86_64/acpi.cpp) fills this bounded struct;
///        the SMP bring-up (hal/smp.cpp) consumes it.  Deliberately a
///        value type — no pointers into the ACPI tables survive the parse
///        (the tables live in firmware memory that test isolation may
///        rewind).  x86_64-only.

#pragma once

#include <types.hpp>

namespace kernel::acpi {

/// @brief Maximum LAPIC IDs recorded (== CONFIG_MAX_CPUS bound for Phase B).
#define MADT_MAX_CPUS 8

/// @brief Result of the MADT scan: enabled local-APIC IDs in entry order.
struct MadtInfo {
    /// @brief Enabled LAPIC/x2APIC IDs (entry order, BSP included).
    uint32_t lapic_ids[MADT_MAX_CPUS] = {};
    /// @brief Number of valid entries in lapic_ids.
    uint8_t ncpus = 0;
    /// @brief True when a well-formed MADT with >= 1 enabled LAPIC was found.
    bool found = false;
    /// @brief True when the MADT is present but malformed (fail-closed
    ///        bring-up: never wake APs from a corrupt table).
    bool malformed = false;
};

/// @brief Scans multiboot2 ACPI tags and the MADT for enabled local APICs.
///        Handles type-0 (LAPIC) and type-9 (x2APIC) entries; disabled
///        entries (flags bit 0 clear) are skipped.  Thread-safe by
///        construction (no shared state; firmware tables are static).
MadtInfo scan_madt();

} // namespace kernel::acpi
