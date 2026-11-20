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

/// @file frame_foreign_map.hpp
/// @brief Foreign frame mapping registry (issue #564, F11 Stage C):
///        maps a FrameCap's frames into an EXPLICIT VSpace root at an
///        EXPLICIT virtual address — the non-caller counterpart of
///        FrameUserMap (which derives the root from the task and the VA
///        from the slot index inside the fixed CONFIG_USER_SHM_VA_BASE
///        window).  The authority gate lives in sys_vspace_frame_map
///        (WRITE on the VSpace, READ on the FrameCap, carved-kind and
///        unmapped-window checks); this registry is the mechanism.
///        Mirrors FrameUserMap exactly: claim-under-lock / map-outside-
///        lock, revalidate-before-release, double-release defence.
///        One VMM::unmap_frame_from_cap_err per page on teardown; holes
///        fold-and-ignore (#366 Q2b): the slot protocol, not the PTE, is
///        authoritative.

#pragma once

#include <types.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/memory/vmm_errors.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/nexios_config.h>

namespace kernel::cap {

/// @brief Foreign frame mapping registry (issue #564, F11 Stage C).
///        Bounded static table keyed (vspace_phys, va); one FrameCap pin
///        per live slot.  No dynamic allocation on RT paths.  Lock order:
///        binding leaf -> foreign-map leaf (never the reverse); the leaf
///        is never held across VMM map/unmap or reschedule.
class ForeignFrameMap {
  public:
    /// @brief Map @p fc's frames at @p va in @p vspace_phys.
    /// @return VMM_ERR_OK when every page mapped (RO pass applied when
    ///         !@p writable); VMM_ERR_INVALID_ADDR on a revoked/kernel/
    ///         empty cap, a misaligned/wrapping address or an overlapping
    ///         window; VMM_ERR_PAGE_ALLOC on registry exhaustion;
    ///         otherwise the map_frame_from_cap_err code (the _err twin
    ///         rolls back mid-range failures — the target must be
    ///         unmapped).
    [[nodiscard]] static errors::VmmError
    map(cap::FrameCap &fc, uint64_t vspace_phys, uint64_t va,
        bool executable, bool writable);

    /// @brief Unmap the mapping at (@p va, @p vspace_phys), if live.
    /// @return True when a mapping was removed.
    [[nodiscard]] static bool unmap(uint64_t va,
                                    uint64_t vspace_phys) noexcept;

    /// @brief Immediately remove every live mapping backed by @p fc
    ///        (revocation closure).  Pointer-equality only.
    static void invalidate_cap(cap::FrameCap *fc);

    /// @brief Remove every live mapping under @p vspace_phys (teardown).
    ///        Holes fold-and-ignore (#366 Q2b).
    static void drain_vspace(uint64_t vspace_phys);

    /// @brief Clears the whole registry (test isolation snapshot restore).
    static void snapshot_reset();

    /// @brief Number of live mappings (test/QE accessor).
    static size_t live_count();

    /// @brief True when (@p va, @p vspace_phys) names a live mapping
    ///        (test/QE accessor).
    [[nodiscard]] static bool is_mapped(uint64_t va,
                                       uint64_t vspace_phys) noexcept;

  private:
    /// @brief Twice the caller-window bound: the foreign window is
    ///        per-child, not per-caller (16 slots).
    static constexpr size_t kMaxMaps = CONFIG_CAP_MAX_FRAME_MAPS * 2;
    struct Slot {
        uint64_t vspace_phys = 0; ///< owning address-space root page
        uint64_t va = 0;          ///< user VA base of the mapping
        size_t pages = 0;         ///< mapped page count (fc->count)
        cap::FrameCap *fc = nullptr; ///< backing cap (equality only)
        bool occupied = false;    ///< slot in use
    };
    static Slot s_slots_[kMaxMaps];
    static sync::SpinLock s_lock_;
};

} // namespace kernel::cap
