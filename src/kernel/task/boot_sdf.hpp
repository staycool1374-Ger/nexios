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

/// @file boot_sdf.hpp
/// @brief Static system architecture (SDF) rows for driver PDs (issue #48).
///        Declares exactly which caps each driver PD receives at boot:
///        IrqCap/MsixCap (own vector only), MmioCap (own BARs), IoMmuDmaCap
///        (DMA domain), FrameCap (fb scanout RAM, not a BAR), SHM channels.
///        compose is all-or-nothing (CNode::install chain with rollback on
///        any failure); rebind re-derives after disposal (never inherits).
///        No test files touched; no runtime `if` on the PD knobs here —
///        callers gate row installation per role.

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace kernel {

struct TaskControlBlock;

namespace sdf {

/// @brief Capability kinds an SDF row can mint.
enum class CapKind : uint8_t {
    IRQ = 0,    ///< IrqCap on @p vector (arch IRQ number).
    MSIX = 1,   ///< MsidCap path reserved (PCI MSI-X entry; opt data).
    MMIO = 2,   ///< MmioCap on [@p addr, @p size) (@p io_port selects IO).
    DMA = 3,    ///< IoMmuDmaCap domain grant (no payload).
    FRAME = 4,  ///< FrameCap pin (fb scanout RAM; folio pages).
    SHM = 5,    ///< Shared-memory channel endpoints (reserved).
};

/// @brief One mint instruction inside a role row.
struct CapSpec {
    CapKind kind;
    uint64_t addr;   ///< MMIO phys base / FRAME phys base (0 if n/a).
    uint64_t size;   ///< MMIO/FAME size (0 if n/a).
    uint32_t vector; ///< IRQ vector / MSI-X entry (0 if n/a).
    uint32_t io_port;///< 1 = IO-port MmioCap, 0 = memory MmioCap.
};

/// @brief Driver PD roles (one SDF row each).
enum class PdRole : uint8_t {
    SERIAL = 0,
    KBD = 1,
    RTC = 2,
    FB = 3,
    BLOCK = 4,
    NET = 5,
};

/// @brief Maximum CapSpecs per role row (== CONFIG_DRIVER_PD_MAX_CAPS).
inline constexpr uint64_t kMaxCapsPerRole = 6;

/// @brief Static row: exactly-declared cap set for @p role.
/// @param out Filled with the row (untouched on unknown role).
/// @return Number of CapSpecs, 0 for unknown role (fail-closed).
uint64_t sdf_row(PdRole role, CapSpec *out);

/// @brief Compose a PD's caps: mint every CapSpec of @p role and install
/// into the task's CSpace (all-or-nothing; rollback on any failure).
/// @return true when every cap minted+installed, false otherwise (nothing
/// installed on failure — partial installs are unwound first).
bool sdf_compose(TaskControlBlock &task, PdRole role);

/// @brief Re-derive a PD's caps after disposal (restart path): dispose is
/// owned by the caller (cap revoke/drain first); this re-mints the row.
/// Never inherits a domain, vector, or mapping — always fresh mints.
/// @return Same contract as sdf_compose.
bool sdf_rebind(TaskControlBlock &task, PdRole role);

/// @brief Single device owner (issue #48 S1): exactly one of {kernel, PD}
/// may own a device. Compile-time selected by the PD knobs.
/// @return true when @p role owns the device (its OFF knob compiled out
/// the kernel bring-up), false when the kernel keeps it.
bool pd_owns_device(PdRole role);

} // namespace sdf
} // namespace kernel
