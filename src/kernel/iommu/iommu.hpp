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

/// @file iommu.hpp
/// @brief IOMMU DMA protection manager (v0.4.2, issues #4 + #9): capability-
/// gated, identity-IOVA second-level translation tables in memory (VT-d
/// layout, vtd.hpp) plus live VT-d enablement (issue #9).  Iteration-1
/// (#4) programs TABLES ONLY.  Phase-2 (#9) adds: ACPI DMAR discovery,
/// a real hardware probe, register-level enablement (RTADDR + kernel
/// passthrough pre-pass + GCMD.TE) and IOTLB/context-cache invalidation
/// after every SL/context mutation when a live unit is present.  The live
/// path is strictly gated on `g_live` — with no IOMMU present all calls
/// fail gracefully and kernel DMA is untouched.

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>
#include <kernel/arch/hal/pci.hpp>
#include <kernel/cap/frame.hpp>

namespace kernel::iommu {

/// @brief One outstanding DMA mapping: a contiguous frame range mapped at
///        its own physical address (identity IOVA — device DMA to an
///        unmapped address faults, it cannot bypass translation).
struct IoMmuMapping {
    uint64_t phys = 0;     ///< first frame physical address == IOVA base
    size_t pages = 0;      ///< number of 4 KiB pages
    uint32_t sl_flags = 0; ///< R/W bits programmed into each leaf entry
    bool occupied = false;
};

/// @brief One DMA protection domain: owns a private second-level page-table
///        root (one PMM page) and a bounded mapping-record table.  Bound to
///        exactly one creating task (the IoMmuDmaCap owner).
struct IoMmuDomain {
    uint64_t sl_root_phys = 0; ///< PMM page holding the SL L4 root
    uint32_t owner_task_id = 0; ///< creating task (domain authority bound)
    IoMmuMapping maps[CONFIG_IOMMU_MAX_MAPPINGS];
    bool occupied = false;
};

/// @brief Static bounded IOMMU translation-table manager.  All tables are
///        fixed .bss allocations except per-domain SL pages (PMM, zeroed on
///        allocation).  One leaf SpinLock serializes all manager state; it
///        is a leaf lock (never held while calling into cap/cspace layers,
///        dispose() or MemPool::free; never held across reschedule()).
class IoMmuManager {
  public:
    /// @brief True when a live IOMMU was detected (phase-2 probe) or the
    ///        test harness force-injected presence.  Default: false.
    static bool probe();

    /// @brief Test-injection setter for the runtime presence flag.  NOT
    ///        #ifdef-gated: control flow is identical in debug and release.
    ///        Sets the SOFTWARE authority gate only — a live unit detected
    ///        by probe_hardware() keeps the hardware path active.
    static void force_present(bool present);

    /// @brief Live hardware detection (phase-2, issue #9).  Scans the ACPI
    ///        DMAR table, maps the remapping unit's MMIO page and validates
    ///        its version register.  On success sets the live flag so
    ///        register programming / IOTLB invalidation become active.
    ///        Fail-closed: a malformed or unvalidatable table leaves the
    ///        manager on the software-only path.
    /// @return True when a live remapping unit was detected.
    static bool probe_hardware();

    /// @brief Actively enables DMA translation on the live unit (issue #9):
    ///        programs RTADDR to the manager's root table, gives every
    ///        kernel-owned device (AHCI / virtio) a T=0 passthrough context
    ///        entry so TE=1 never blocks kernel DMA, then sets GCMD.TE.
    ///        Order is fail-closed: RTADDR + passthrough pre-pass FIRST,
    ///        TE LAST; any failure leaves TE=0 and translation off.
    /// @return True when translation is live; false (translation stays off)
    ///         on any failure.
    static bool enable_translation();

    /// @brief Reads and clears the unit's fault-status register (minimal
    ///        fault handling — FECTL disables fault logging; no fault-log
    ///        walk, no task kill; documented phase-2 follow-up).
    static void read_clear_faults();

    /// @brief MMIO register base of the live unit (0 when software-only).
    static uint64_t mmio_base();

    /// @brief True when the live unit has translation enabled (GSTS.TES).
    static bool translation_live();

    /// @brief Creates an empty DMA domain bound to @p task_id.
    /// @return domain index (>= 0), or -1 when absent/exhausted (fail-closed).
    static int16_t domain_create(uint32_t task_id);

    /// @brief Destroys the domain at @p idx: tears down every outstanding
    ///        mapping, frees all SL pages (root included) and releases the
    ///        slot.  Idempotent; never touches another domain.
    static void domain_destroy(int16_t idx);

    /// @brief Maps every page of @p fc at its identity IOVA with @p sl_flags
    ///        (R|W bits) in the domain at @p idx.  Overlapping mappings are
    ///        rejected; a mid-map failure rolls back every entry written by
    ///        this call (no partial mapping is ever visible).
    static bool map_frame(int16_t idx, const cap::FrameCap &fc,
                          uint32_t sl_flags);

    /// @brief Removes the mapping covering exactly @p fc (phys + page count)
    ///        and reclaims now-empty intermediate table pages.
    static bool unmap_frame(int16_t idx, const cap::FrameCap &fc);

    /// @brief Attaches the device at @p bdf to the domain at @p idx by
    ///        programming the root entry (bus) and the context entry
    ///        (device:function) with T=translate and ASR = the domain root.
    ///        Re-attaching the same device to another domain re-points it
    ///        (one device -> one domain; last attach wins).
    static bool attach_device(int16_t idx, arch::PciBdf bdf);

    /// @brief Clears the root + context entries programmed for @p bdf.
    static void clear_attachment(int16_t idx, arch::PciBdf bdf);

    // -- introspection (tests / leak audit) --

    /// @brief SL root physical address of the domain (0 when invalid/free).
    static uint64_t sl_root(int16_t idx);
    /// @brief True when the domain slot is occupied.
    static bool domain_valid(int16_t idx);
    /// @brief Number of occupied domain slots (leak audit).
    static size_t occupied_domains();
    /// @brief Number of outstanding mappings in the domain (leak audit).
    static size_t mapping_count(int16_t idx);
    /// @brief True when the root entry for @p bus is Present.
    static bool root_entry_present(uint8_t bus);
    /// @brief Context-entry ASR for @p bdf when attached to @p idx, else 0.
    static uint64_t context_asr(int16_t idx, arch::PciBdf bdf);
    /// @brief Low 64 bits of the context entry programmed for @p bdf (the
    ///        full raw entry — Present + TT + ASR).  0 when no context table
    ///        covers @p bdf.  Test introspection for the live passthrough
    ///        entries programmed by enable_translation().
    static uint64_t context_entry(arch::PciBdf bdf);
};

} // namespace kernel::iommu
