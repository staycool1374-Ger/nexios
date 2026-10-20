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

} // namespace kernel::boot
