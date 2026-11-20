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

/// @file vspace_map.cpp
/// @brief VSpace/PageTable link registry implementation (F5, #558):
///        the single mutation site for N4 VSPACE_MAP (INV-VS-9: every
///        tree-word store and every registry mutation runs here, under
///        s_lock_, never with scheduler_lock_ held, never from an ISR).

#include <kernel/cap/vspace_map.hpp>
#include <kernel/cap/carve_binding.hpp>
#include <kernel/cap/vspace_errors.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/arch/page_table.hpp>
#include <constants.hpp>

namespace kernel::cap {

// Compile-time fail-closed (design-review-554-r1 S3:96-99, per-consumer
// check): a VSpace/PageTable object IS one arch page-table page and has
// no C++ struct, so the size equality is asserted here, at the consumer.
static_assert(arch::ArchPageTable::ENTRIES * sizeof(uint64_t) ==
                  kObjectBytesVSpace,
              "VSpace object must be exactly one page-table page");
static_assert(arch::ArchPageTable::ENTRIES * sizeof(uint64_t) ==
                  kObjectBytesPageTable,
              "PageTable object must be exactly one page-table page");
static_assert(kMaxVspaceMappings > 0, "mapping registry must hold a link");

VSpaceMap::Slot VSpaceMap::s_slots_[kMaxVspaceMappings];
sync::SpinLock VSpaceMap::s_lock_{};

uint64_t VSpaceMap::component(uint64_t path, unsigned comp_idx) noexcept {
    return (path >> (9u * comp_idx)) & 0x1FFu;
}

errors::VspaceError
VSpaceMap::walk_parent(uint64_t root_phys, uint64_t path, uint8_t path_len,
                       uint64_t &parent_out) noexcept {
    uint64_t cursor = root_phys;
    const unsigned last = static_cast<unsigned>(path_len) - 1u;
    for (unsigned level = 0; level < last; ++level) {
        const uint64_t *table = reinterpret_cast<const uint64_t *>(
            arch::HHDM_OFFSET + cursor);
        const uint64_t entry = table[component(path, level)];
        if (entry == 0)
            return errors::VSPACE_ERR_UNMAPPED;
        if (!VMM::table_entry_is_table(entry))
            return errors::VSPACE_ERR_NOT_TABLE;
        cursor = VMM::table_entry_child(entry);
    }
    parent_out = cursor;
    return errors::VSPACE_ERR_OK;
}

void VSpaceMap::drop_dependents_locked(uint64_t dying_phys) noexcept {
    uint64_t dead[kMaxVspaceMappings];
    size_t dead_count = 1;
    dead[0] = dying_phys;
    for (size_t pass = 0; pass < kMaxVspaceMappings; ++pass) {
        bool changed = false;
        for (size_t slot_idx = 0; slot_idx < kMaxVspaceMappings;
             ++slot_idx) {
            Slot &slot = s_slots_[slot_idx];
            if (!slot.occupied)
                continue;
            for (size_t dead_idx = 0; dead_idx < dead_count; ++dead_idx) {
                if (slot.parent_phys == dead[dead_idx]) {
                    slot.occupied = false;
                    if (dead_count < kMaxVspaceMappings)
                        dead[dead_count++] = slot.table_phys;
                    changed = true;
                    break;
                }
            }
        }
        if (!changed)
            break;
    }
}

void VSpaceMap::zero_page(uint64_t phys) noexcept {
    __builtin_memset(reinterpret_cast<void *>(arch::HHDM_OFFSET + phys), 0,
                     arch::PAGE_SIZE);
}

errors::VspaceError VSpaceMap::link(uint64_t vspace_phys,
                                    uint64_t table_phys, uint64_t path,
                                    uint8_t path_len) noexcept {
    // Argument validation before any shift (LEARNINGS #337 class): a
    // path_len of 255 would make 9 * path_len over-wide.  Compare first,
    // never shift an unvalidated length.
    const unsigned levels = VMM::page_table_levels();
    const unsigned max_len = (levels > 0) ? levels - 1u : 0u;
    if (path_len == 0 || path_len > max_len)
        return errors::VSPACE_ERR_BAD_LEVEL;
    if ((path >> (9u * static_cast<unsigned>(path_len))) != 0)
        return errors::VSPACE_ERR_INVALID_ARGS;
    if (component(path, 0) >= arch::PML4_USER_COUNT)
        return errors::VSPACE_ERR_KERNEL_HALF;
    if (vspace_phys == table_phys)
        return errors::VSPACE_ERR_ALIASED;
    if (vspace_phys == VMM::current_root_phys())
        return errors::VSPACE_ERR_ACTIVE_SPACE;
    // INV-VS-3-prime (issue #564, F11 Stage C): an activated root is
    // immutable — post-activation links would escape the activation-time
    // validate_subtree guarantee.  Pure lock-free probe (no lock — the
    // INV-VS-9 precedent), so there is no s_lock_ <-> binding cycle.
    if (carve_root_is_activated(vspace_phys))
        return errors::VSPACE_ERR_ACTIVATED;
    if (((vspace_phys | table_phys) & (arch::PAGE_SIZE - 1u)) != 0)
        return errors::VSPACE_ERR_INVALID_ARGS;

    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    uint64_t parent_phys = 0;
    const errors::VspaceError walk_err =
        walk_parent(vspace_phys, path, path_len, parent_phys);
    if (walk_err != errors::VSPACE_ERR_OK)
        return walk_err;
    uint64_t *parent = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                   parent_phys);
    const uint64_t dest_idx =
        component(path, static_cast<unsigned>(path_len) - 1u);
    if (parent[dest_idx] != 0)
        return errors::VSPACE_ERR_SLOT_OCCUPIED;
    for (size_t slot_idx = 0; slot_idx < kMaxVspaceMappings; ++slot_idx) {
        if (s_slots_[slot_idx].occupied &&
            s_slots_[slot_idx].table_phys == table_phys)
            return errors::VSPACE_ERR_ALIASED;
    }
    size_t free_idx = kMaxVspaceMappings;
    for (size_t slot_idx = 0; slot_idx < kMaxVspaceMappings; ++slot_idx) {
        if (!s_slots_[slot_idx].occupied) {
            free_idx = slot_idx;
            break;
        }
    }
    if (free_idx == kMaxVspaceMappings)
        return errors::VSPACE_ERR_REGISTRY_FULL;
    // Resurrection kill (design-review-558-r1 S3): a dropped dependent
    // keeps stale links in its page (teardown zeroes only the departing
    // object's own page).  Scrub the child before installing — bounded
    // single 4 KiB store, zero allocation, deterministic timing — so no
    // stale subtree can ever be re-linked into a live tree.
    zero_page(table_phys);
    // The one mutation: a single 8-byte table-link word through the
    // per-arch codec (bit-identical to the leaf walk's encoder).
    parent[dest_idx] = VMM::table_link_word(table_phys);
    Slot &slot = s_slots_[free_idx];
    slot.vspace_phys = vspace_phys;
    slot.table_phys = table_phys;
    slot.parent_phys = parent_phys;
    slot.path = path;
    slot.path_len = path_len;
    slot.occupied = true;
    return errors::VSPACE_ERR_OK;
}

void VSpaceMap::invalidate_table(uint64_t table_phys) noexcept {
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t slot_idx = 0; slot_idx < kMaxVspaceMappings;
             ++slot_idx) {
            Slot &slot = s_slots_[slot_idx];
            if (!slot.occupied || slot.table_phys != table_phys)
                continue;
            uint64_t parent_phys = 0;
            if (walk_parent(slot.vspace_phys, slot.path, slot.path_len,
                            parent_phys) == errors::VSPACE_ERR_OK) {
                uint64_t *parent = reinterpret_cast<uint64_t *>(
                    arch::HHDM_OFFSET + parent_phys);
                const uint64_t dest_idx = component(
                    slot.path, static_cast<unsigned>(slot.path_len) - 1u);
                // A re-used path must never be clobbered: clear only the
                // word this registry record installed.
                if (parent[dest_idx] == VMM::table_link_word(table_phys))
                    parent[dest_idx] = 0;
            }
            slot.occupied = false;
            break;
        }
        drop_dependents_locked(table_phys);
    }
    // INV-VS-5 (amended, issue #564 M14 handoff spent): the full shootdown
    // runs BEFORE the zero — a stale TLB entry translating a freed page
    // under a live task is S1 now that roots can activate.  Conservative
    // full shootdown after the registry unlock (untagged CR3 reload on
    // x86_64 / tlb_purge_all INVPCID all-incl-global, TLBI VMALLE1-class
    // on aarch64, full sfence.vma on riscv64, plus the weak SMP hook).
    // Never narrowed to (ASID, VA): no ASID/PCID is assigned to a carved
    // space at link time.  Activation re-validates the whole subtree —
    // no VA under this tree is translatable until then.
    VMM::tlb_shootdown(0, 0, true);
    {
        // The zero re-takes the registry lock so it stays mutually
        // exclusive with link()'s child scrub + store (INV-VS-9: the
        // flush itself is never held across, but the zero is).
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        // Authoritative kill (INV-VS-4 step 3): after this store no stale
        // link inside the departing page can ever reach a freed page,
        // whatever order F2's revoke cascade destroys the caps in.
        zero_page(table_phys);
    }
}

void VSpaceMap::invalidate_vspace(uint64_t vspace_phys) noexcept {
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t slot_idx = 0; slot_idx < kMaxVspaceMappings;
             ++slot_idx) {
            Slot &slot = s_slots_[slot_idx];
            if (slot.occupied && slot.vspace_phys == vspace_phys)
                slot.occupied = false;
        }
    }
    // INV-VS-5 (amended): flush BEFORE the zero (same S1 rationale as
    // invalidate_table).
    VMM::tlb_shootdown(0, 0, true);
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        // One bounded store kills every link inside the root, whatever
        // order the caps leave in (INV-VS-4).  Orphaned PageTable caps
        // stay deletable afterwards (their own teardown re-zeroes a page
        // they still own — no double free, no dangling record).
        zero_page(vspace_phys);
    }
}

void VSpaceMap::invalidate_vspace_links_only(uint64_t vspace_phys) noexcept {
    // Issue #564, F11 Stage C: the carved-teardown releaser path.  The
    // pages are cap-owned (root/table/frame caps stay pinned to the
    // teardown drain), so zeroing here would scrub live caps — unlink the
    // registry records and drop dependents only, then the TLB flush.
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t slot_idx = 0; slot_idx < kMaxVspaceMappings;
             ++slot_idx) {
            Slot &slot = s_slots_[slot_idx];
            if (slot.occupied && slot.vspace_phys == vspace_phys)
                slot.occupied = false;
        }
        drop_dependents_locked(vspace_phys);
    }
    VMM::tlb_shootdown(0, 0, true);
}

void VSpaceMap::snapshot_reset() noexcept {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    for (size_t slot_idx = 0; slot_idx < kMaxVspaceMappings; ++slot_idx)
        s_slots_[slot_idx].occupied = false;
}

size_t VSpaceMap::live_count() noexcept {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    size_t live = 0;
    for (size_t slot_idx = 0; slot_idx < kMaxVspaceMappings; ++slot_idx) {
        if (s_slots_[slot_idx].occupied)
            ++live;
    }
    return live;
}

bool VSpaceMap::is_linked(uint64_t table_phys) noexcept {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    for (size_t slot_idx = 0; slot_idx < kMaxVspaceMappings; ++slot_idx) {
        if (s_slots_[slot_idx].occupied &&
            s_slots_[slot_idx].table_phys == table_phys)
            return true;
    }
    return false;
}

} // namespace kernel::cap
