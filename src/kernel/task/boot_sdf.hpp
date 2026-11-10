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
    SHM = 5,    ///< TX-ring channel endpoint (issue #51 step-c-prime:
                ///< fresh user frames + FrameCap pin, installed in the PD
                ///< CSpace; kernel producer uses the HHDM alias).
    DMA_REGION = 6, ///< Shared data window (issue #47 step-b1): fresh
                ///< user frames + FrameCap pin in the PD CSpace; the
                ///< kernel side uses the HHDM alias. Unlike SHM, no
                ///< ring header is initialised (raw sector window).
};

/// @brief One mint instruction inside a role row.
struct CapSpec {
    CapKind kind;
    uint64_t addr;   ///< MMIO exact window phys (mapping aligns the base
                     ///< down internally; the PD applies the intra-page
                     ///< offset) / FRAME phys (page-aligned) / 0 if n/a.
    uint64_t size;   ///< MMIO/FAME exact byte size (mapping rounds the
                     ///< span up to whole pages) / 0 if n/a.
    uint32_t vector; ///< IRQ vector / MSI-X entry (0 if n/a).
    uint32_t io_port;///< 1 = IO-port MmioCap, 0 = memory MmioCap.
    uint32_t pci_bdf = 0; ///< Packed PCI BDF for MSIX caps (issue #47
                     ///< step-b2): bus<<16 | device<<8 | function.
    uint32_t msix_entry = 0; ///< MSI-X table entry index for MSIX caps.
};

/// @brief Driver PD roles (one SDF row each; VFS_SRV/BLOCK_SRV are
/// issue #47, TERM_SRV/LOG_SRV are issue #51, CAP_BROKER is issue #240,
/// WDOG_MON is issue #390 and PROCFS_SRV is issue #54; all share this
/// table + compose path).
enum class PdRole : uint8_t {
    SERIAL = 0,
    KBD = 1,
    RTC = 2,
    FB = 3,
    BLOCK = 4,
    NET = 5,
    VFS_SRV = 6,
    BLOCK_SRV = 7,
    TERM_SRV = 8,
    LOG_SRV = 9,
    CAP_BROKER = 10,
    WDOG_MON = 11,
    PROCFS_SRV = 12,
};

/// @brief Maximum CapSpecs per role row (== CONFIG_DRIVER_PD_MAX_CAPS).
inline constexpr uint64_t kMaxCapsPerRole = CONFIG_DRIVER_PD_MAX_CAPS;

/// @brief Console PD scanout chunking (issue #51): a full scanout
///        (pitch*height, e.g. 3 MiB at 1024x768x32) rides as several
///        consecutive caps because one user mapping slot holds 2 MiB.
///        MMIO chunks (true device memory) size to the MMIO window,
///        FRAME chunks (RAM-backed scanouts) to the frame window.
///        Mirrors userspace/driver_pd.h CONSOLE_MMIO_CHUNK /
///        CONSOLE_FRAME_CHUNK.
inline constexpr uint64_t kConsoleMmioChunk = CONFIG_USER_MMIO_REGION_SIZE;
inline constexpr uint64_t kConsoleFrameChunk = CONFIG_USER_SHM_REGION_SIZE;
/// @brief Max scanout chunks a row can carry (issue #47 step-b2: pinned
///        at 4 — the row grew to 8 slots for IRQ caps, but the console
///        scanout layout stays at indices 2..5 to match userspace).
inline constexpr uint64_t kConsoleMaxScanoutChunks = 4;
static_assert(kConsoleMmioChunk == 0x200000ULL,
              "userspace/driver_pd.h CONSOLE_MMIO_CHUNK mirrors this value");
static_assert(kConsoleFrameChunk == 0x200000ULL,
              "userspace/driver_pd.h CONSOLE_FRAME_CHUNK mirrors this "
              "value");
static_assert(kConsoleMaxScanoutChunks == 4,
              "userspace/driver_pd.h CONSOLE_MAX_SCANOUT_CHUNKS mirrors "
              "this");

/// @brief Number of PdRole entries (issue #47 step-b1): role tables are
/// sized by this, not by a hardcoded 6 (roles ≥ 6 share the compose
/// path but previously skipped slot persistence).
inline constexpr uint64_t kPdRoleCount =
    static_cast<uint64_t>(PdRole::PROCFS_SRV) + 1;

/// @brief TX-ring element size in bytes (issue #51 step-c-prime).
/// Fixed 8: {uint8 len, char data[7]}. Multiple of 8 per the SPSC
/// protocol (shm_ring_init requires element_size % 8 == 0). Shared with
/// userspace (userspace/driver_pd.h SERIAL_TX_RING_ESZ — keep in sync).
inline constexpr uint64_t kSerialTxRingElemSize = 8;

/// @brief TX-ring backing pages (issue #51 step-c-prime): page 0 holds the
/// SharedRingHeader, pages 1..N-1 hold the payload. 4 pages = 12 KiB =
/// 1536 elements ≈ 10 KiB of console text per drain — deep enough for a
/// boot flood, small enough to waste nothing when idle.
inline constexpr uint64_t kSerialTxRingPages = 4;

/// @brief Console PD text-ring pages (issue #51 console flip). Same wire
/// and element shape as the serial TX ring; 4 pages keeps the same
/// flooded-boot budget for the framebuffer console path.
inline constexpr uint64_t kConsoleRingPages = 4;
/// @brief Console PD status page count (render counters the kernel test
/// reads directly through the HHDM alias; raw window, no header).
inline constexpr uint64_t kConsoleStatusPages = 1;

/// @brief Static row: exactly-declared cap set for @p role.
/// @param out Filled with the row (untouched on unknown role).
/// @return Number of CapSpecs, 0 for unknown role (fail-closed).
uint64_t sdf_row(PdRole role, CapSpec *out);

/// @brief Compose a PD's caps: mint every CapSpec of @p role and install
/// into the task's CSpace (all-or-nothing; rollback on any failure).
/// @return true when every cap minted+installed, false otherwise (nothing
/// installed on failure — partial installs are unwound first).
bool sdf_compose(TaskControlBlock &task, PdRole role);

/// @brief Read back the SDF-composed TX-ring backing store for @p role
/// (issue #51 step-c-prime): the SHM mint's (phys, pages), written only
/// after the whole row composes; cleared on any (re)compose start.
/// @return true with *phys_out/*pages_out filled on a live ring record,
/// false when the role carries no SHM spec (knob off / compose failed).
bool sdf_shm_tx_phys(PdRole role, uint64_t &phys_out, uint64_t &pages_out);

/// @brief Read back the SDF-composed DMA_REGION window for @p role
/// (issue #47 step-b1): the DMA_REGION mint's (phys, pages), written
/// only after the whole row composes; cleared on any (re)compose start.
/// @return true with *phys_out/*pages_out filled on a live record,
/// false when the role carries no DMA_REGION spec.
bool sdf_role_frame(PdRole role, uint64_t idx, uint64_t &phys_out,
                    uint64_t &pages_out);

/// @brief Re-derive a PD's caps after disposal (restart path): dispose is
/// owned by the caller (cap revoke/drain first); this re-mints the row.
/// Never inherits a domain, vector, or mapping — always fresh mints.
/// @return Same contract as sdf_compose.
bool sdf_rebind(TaskControlBlock &task, PdRole role);

/// @brief Composed cap handle lookup (issue #51 step-c): the (slot,
///        generation) pair sdf_compose minted for @p role at boot/rebind.
///        Kernel→PD BIND conveys the PD's own handles back to it through
///        this table (the PD cannot scan its CSpace).
/// @return True with @p slot/@p gen filled; false when the role never
///         composed or @p idx is out of range (fail-closed, PD keeps its
///         BIND-only behavior and the kernel keeps the raw path).
bool sdf_role_cap(PdRole role, uint64_t idx, int &slot, uint32_t &gen);

/// @brief Single device owner (issue #48 S1): exactly one of {kernel, PD}
/// may own a device. Compile-time selected by the PD knobs.
/// @return true when @p role owns the device (its OFF knob compiled out
/// the kernel bring-up), false when the kernel keeps it.
bool pd_owns_device(PdRole role);

/// @brief Structural policy-only predicate (issues #47/#51): true only
/// for roles whose SDF row MUST stay empty (no device caps ever —
/// VFS_SRV by design, TERM_SRV/LOG_SRV/CAP_BROKER/WDOG_MON/PROCFS_SRV
/// policy PDs). Used by compose to
/// reject any non-empty row for these roles, and by #48's SDF boundary
/// test to prove the emulation side holds no device caps.
/// @return True for VFS_SRV, TERM_SRV, LOG_SRV, CAP_BROKER, WDOG_MON,
/// PROCFS_SRV; false otherwise.
bool sdf_policy_only(PdRole role);

/// @brief Capture block topology for the BLOCK_SRV row (issue #47 S2):
/// AHCI ABAR (PCI-discovered, x86) or virtio-blk transport ranges.
/// Idempotent; keeps at most kMaxCapsPerRole page-aligned MMIO specs.
/// @return true when capture ran (row may still be empty on unknown hw).
bool capture_block_topology();

/// @brief True when @p role may compose with zero caps (explicit
/// allowlist): VFS_SRV always (no ambient authority — the row IS the
/// empty set); degraded BLOCK_SRV when no controller/IOMMU was captured
/// (logged at the call site; never a silent start).
bool sdf_zero_caps_allowed(PdRole role);

} // namespace sdf
} // namespace kernel
