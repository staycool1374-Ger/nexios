#pragma once

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

/// @file vspace_map.hpp
/// @brief Bounded VSpace/PageTable link registry (F5, issue #558):
///        the single mutation site for N4 VSPACE_MAP.  Links a carved
///        PageTable page at a 9-bit-component path under a carved VSpace
///        root by writing one table-link word through the per-arch codec.
///        No derivation edges are recorded for table links (uses-not-caps:
///        the tree is reachability, not authority — F2's derivation table
///        is untouched by every path below).
///
///        INV-VS-1 single parent per table: a carved PageTable page is
///        linked under at most one parent slot in at most one VSpace.
///        Enforced by a bounded registry scan on the page's physical
///        identity before any store.  Acyclicity follows from four
///        jointly sufficient checks, all on the link path: (a) the
///        single-parent scan refuses a second parent for any page
///        (ALIASED); (b) the kind gate keeps VSpace roots out of child
///        position (only PageTable caps link, never a VSpace); (c) a
///        table is never linked under itself (self-link refusal);
///        (d) links are only installed top-down through live tables —
///        a missing component fails UNMAPPED, so no link can skip under
///        a page that is not already reachable from the root.
///        INV-VS-2 one owner per page, no allocation on the map path: the
///        success path performs one bounded child-page scrub (a dropped
///        dependent keeps stale links in its page — the resurrection
///        kill, design-review-558-r1 S3) plus exactly one 8-byte
///        table-link store in an already-existing parent page.  No PMM
///        call, no MemPool call: PMM ownership of both pages stays with
///        their CarvedObjectCaps (F1), which is what keeps the later
///        leaf walk allocate-free (INV-MK-1 / F21).
///        INV-VS-4 removal is total and order-independent: both teardown
///        entries unlink the entry, drop dependent slots whose parent
///        chain is leaving (bounded fixpoint), then zero the departing
///        object's own page before PMM::free_page runs.  The child scrub
///        on link plus the zero on teardown make the kill independent of
///        the order F2's revoke cascade destroys the caps in.
///        INV-VS-5 flush discipline: every removal ends with a
///        conservative full VMM::tlb_shootdown(0, 0, true) after the
///        registry lock is released (untagged CR3 reload on x86_64,
///        TLBI VMALLE1-class on aarch64, full sfence.vma on riscv64,
///        plus the weak SMP hook).  No narrowing to (ASID, VA): no
///        ASID/PCID is assigned to a carved space yet.  Install takes no
///        flush: the destination entry was zero, the child page is
///        scrubbed, and no carved space is ever activated at F5
///        (INV-VS-3) — M14 must move the flush before the zero and add
///        activation-time subtree re-validation once it activates a
///        carved space.
///        INV-VS-8 mapping lifetime is bound to the PageTable cap: the
///        registry stores physical addresses only (never object pointers,
///        so no record can dangle) and holds NO pin on either cap —
///        deliberate divergence from FrameUserMap, which pins because its
///        VMM unmap path dereferences the cap.  A pin here would keep the
///        page alive after capability deletion with no name to unmap it
///        (F5 adds no unmap syscall): deletion is destructive
///        (seL4-style — CAP_DELETE destroys the link and frees the page
///        on the last release).  Do NOT add pins back without
///        re-deriving the leak argument.
///        INV-VS-9 lock discipline: one TU-local leaf spinlock (s_lock_)
///        serialises the registry and the tree word.  Order: CNode slot
///        lock -> s_lock_ (same take class as cap::remove -> dispose).
///        s_lock_ never takes another lock, never calls VMM/PMM/MemPool,
///        never reschedules, is never held across the TLB flush, and is
///        never taken with scheduler_lock_ held or from an ISR path.

#include <types.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/object_sizing.hpp>
#include <kernel/cap/vspace_errors.hpp>
#include <kernel/sync/spinlock.hpp>

namespace kernel::cap {

/// @brief Maximum live VSpace table links kernel-wide.  Placeholder
///        sizing owned by F9 (#562, Q7 bound re-sizing): 32 slots cover
///        10+ distinct 4-level subtrees.  Lives here (cap/) per Q7 — not
///        a nexios_config.h key; when F9 re-sizes page-table bounds this
///        constant moves with it, it is not duplicated.  Tracked by F9 so
///        the placeholder cannot fossilize.
inline constexpr size_t kMaxVspaceMappings = 32;

/// @brief Bounded registry + tree-link policy for VSpace/PageTable
///        objects (F5 N4, issue #558).  All methods are noexcept, bounded
///        (no unbounded loop, no blocking wait, no reschedule), and safe
///        from task context only (never ISR).
class VSpaceMap {
  public:
    /// @brief Link @p table_phys at @p path under @p vspace_phys.
    ///        Component k of the path rides bits [9k .. 9k+8], root
    ///        first; exactly @p path_len components are consumed.
    /// @return VSPACE_ERR_OK on success, else the single typed reason.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    [[nodiscard]] static errors::VspaceError link(uint64_t vspace_phys,
                                                 uint64_t table_phys,
                                                 uint64_t path,
                                                 uint8_t path_len) noexcept;
    // NOLINTEND(bugprone-easily-swappable-parameters)
    /// @brief Destroy the link held by @p table_phys (if any), drop
    ///        dependent slots, zero the page, flush the TLB.
    ///        Idempotent: a second call only re-zeroes a page the caller
    ///        still owns.  Runs before PMM::free_page in dispose().
    static void invalidate_table(uint64_t table_phys) noexcept;
    /// @brief Destroy every link under @p vspace_phys, zero the root
    ///        page (kills the whole subtree in one bounded store), flush
    ///        the TLB.  Idempotent.  Runs before PMM::free_page.
    static void invalidate_vspace(uint64_t vspace_phys) noexcept;
    /// @brief Test isolation: drop every registry slot.  Touches no
    ///        page (fixture pages belong to the snapshot, rewound by it)
    ///        and unwinds no pin (none exists, INV-VS-8).
    static void snapshot_reset() noexcept;
    /// @brief Number of live registry slots (test accessor).
    static size_t live_count() noexcept;
    /// @brief True when @p table_phys holds a live link (test accessor).
    static bool is_linked(uint64_t table_phys) noexcept;

  private:
    /// @brief One live link: physical identities only (INV-VS-8), so no
    ///        record can become a dangling dereference.
    struct Slot {
        uint64_t vspace_phys = 0; ///< owning address-space root page
        uint64_t table_phys = 0;  ///< linked child page (single parent)
        uint64_t parent_phys = 0; ///< page holding the installed word
        uint64_t path = 0;        ///< packed 9-bit components, root first
        uint8_t path_len = 0;     ///< consumed component count
        bool occupied = false;
    };
    static Slot s_slots_[kMaxVspaceMappings];
    static sync::SpinLock s_lock_;

    /// @brief Component k of @p path (callers guarantee k < 4 and a
    ///        pre-validated length, so the index is in range).
    static uint64_t component(uint64_t path, unsigned comp_idx) noexcept;
    /// @brief Walk components 0..path_len-2 from @p root_phys; on
    ///        success @p parent_out names the page holding the target
    ///        slot.  Pure walk, no store.
    [[nodiscard]] static errors::VspaceError
    walk_parent(uint64_t root_phys, uint64_t path, uint8_t path_len,
                uint64_t &parent_out) noexcept;
    /// @brief Drop every slot whose parent chain is leaving: fixpoint
    ///        over the registry, at most kMaxVspaceMappings passes.
    ///        Caller holds s_lock_.
    static void drop_dependents_locked(uint64_t dying_phys) noexcept;
    /// @brief Zero one page via the HHDM (bounded single 4 KiB store).
    static void zero_page(uint64_t phys) noexcept;
};

} // namespace kernel::cap
