#pragma once

/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
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

/// @file dtb.hpp
/// @brief Validated device-tree access (issue #350) — the DTB is firmware
/// input, so every accessor fail-closes to null/false on absent or
/// malformed data. No allocation; all walks bounded.

#include <types.hpp>

namespace kernel::boot {

/// Maximum compatible-node matches returned per search.
constexpr int DTB_MAX_MATCHES = 32;

/// @brief Validated DTB pointer from boot info, or nullptr.
/// Checks region containment (usable or reserved RAM), HHDM aliasing,
/// layout + structure validation. Fail-closed on any defect.
const void *dtb_from_boot_info();

/// @brief Collect up to @p max_offsets node offsets whose "compatible"
/// property contains @p compat as one NUL-separated string.
/// @return match count (0 = none), or negative -FDT_ERR_* on bad input.
/// The blob must already be validated (use dtb_from_boot_info()).
int dtb_find_compatible_nodes(const void *fdt, const char *compat,
                              int *out_offsets, int max_offsets);

/// @brief Decode a node's first "reg" tuple using the root
/// #address-cells/#size-cells (1 or 2 each, default 2/2).
/// @return true with @p base/@p size on success, false otherwise.
bool dtb_node_reg(const void *fdt, int node, uint64_t &base, uint64_t &size);

/// @brief Decode a u32-cell property (e.g. "interrupts") big-endian.
/// @return true with @p cell_count cells on success, false otherwise.
bool dtb_node_cells(const void *fdt, int node, const char *prop_name,
                    uint32_t *out_cells, int max_cells, int &cell_count);

/// @brief CPU enable-method parsed from a cpu node's "enable-method".
/// UNKNOWN covers absent (rv64 HSM-implicit) — never a boot decision.
enum class CpuEnableMethod : uint8_t {
    UNKNOWN = 0,    ///< No (or unrecognized) enable-method property.
    PSCI = 1,       ///< "psci" — booted via PSCI_CPU_ON (slice-2).
    SPIN_TABLE = 2, ///< "spin-table" — boot via cpu-release-addr (slice-2).
};

/// @brief One discovered CPU node (/cpus/cpu@*).
struct DtbCpuEntry {
    uint64_t id = 0; ///< reg value: MPIDR Aff0 (aa64) or hartid (rv64).
    CpuEnableMethod method = CpuEnableMethod::UNKNOWN;
    uint64_t release_addr = 0; ///< cpu-release-addr (spin-table only).
};

/// @brief Walk /cpus/cpu@* nodes (issue #351 slice-1).
/// Decodes each cpu reg with the /cpus #address-cells (probed, never
/// assumed), parses enable-method + cpu-release-addr. Fail-closed:
/// returns 0 on absent DTB, missing /cpus, missing/non-{1,2}
/// #address-cells, any malformed cpu node, or more CPUs than fit
/// (callers size by CONFIG_MAX_CPUS; overflow cannot boot safely).
/// @return cpu count stored (0 = none — caller takes the C1 halt path).
int dtb_walk_cpus(const void *fdt, DtbCpuEntry *out_entries, int max_entries);

/// @brief Read the PSCI conduit ("smc"/"hvc") from the first
/// "arm,psci-*" compatible node (issue #351 slice-1).
/// Fail-closed false when no psci node, no method, or malformed.
/// @return true with the NUL-terminated method on success.
bool dtb_psci_method(const void *fdt, char *out_method, int max_len);

} // namespace kernel::boot
