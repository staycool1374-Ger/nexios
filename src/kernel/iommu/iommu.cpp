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

/// @file iommu.cpp
/// @brief IoMmuManager implementation (issue #4).  All state is static and
/// bounded (.bss tables + per-domain PMM SL pages).  Single-owner page
/// discipline: every SL page belongs to exactly one domain and is freed
/// exactly once — either by cascade-free during unmap/rollback or by
/// domain_destroy walking only still-occupied mapping records.  PMM returns
/// unzeroed memory, so EVERY table page is memset(0) before use (a stale
/// R/W entry would grant DMA to recycled frames).

#include <kernel/iommu/iommu.hpp>
#include <kernel/iommu/vtd.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <constants.hpp>

namespace kernel::iommu {

using sync::SpinLock;

namespace {

SpinLock g_lock_{};
bool g_present = false;

IoMmuDomain g_domains[CONFIG_IOMMU_MAX_DOMAINS];

/// @brief 16-byte root/context entry (VT-d legacy layout).
struct Entry16 {
    uint64_t lo = 0;
    uint64_t hi = 0;
};

/// @brief Root table: 256 bus entries x 16 bytes = exactly one 4 KiB page.
Entry16 g_root_table[256];
/// @brief Per-bus context tables (256 device:function entries each).
Entry16 g_ctx_tables[CONFIG_IOMMU_MAX_BUSES][256];
/// @brief Bus number served by each context-table slot (-1 = free).
int16_t g_ctx_bus_ids[CONFIG_IOMMU_MAX_BUSES] = {
    -1, -1, -1, -1, -1, -1, -1, -1,
};
static_assert(CONFIG_IOMMU_MAX_BUSES <= 8,
              "initializer list covers the default bound");

/// @brief Kernel virtual address of a static table -> physical address
/// (the kernel image is mapped at HHDM_OFFSET + phys).
uint64_t kva_to_phys(const void *kva) {
    return reinterpret_cast<uint64_t>(kva) - arch::HHDM_OFFSET;
}

/// @brief Physical table address -> kernel virtual address (HHDM).
uint64_t *table_va(uint64_t table_phys) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + table_phys);
}

/// @brief Allocates and zeroes one SL table page.
/// @return physical address, or 0 on exhaustion (fail-closed).
uint64_t alloc_zeroed_table_page() {
    uint64_t page = PMM::alloc_page();
    if (page == 0)
        return 0;
    __builtin_memset(table_va(page), 0, arch::PAGE_SIZE);
    return page;
}

/// @brief Context-entry index for a BDF: device * 8 + function (256 entries).
size_t ctx_index(arch::PciBdf bdf) {
    return static_cast<size_t>(bdf.device) * 8 + bdf.function;
}

/// @brief True when @p bdf is within the encodable PCI range (device 0-31,
///        function 0-7) so ctx_index stays inside the 256-entry context
///        table.  PciBdf fields are raw uint8_t: an unvalidated BDF indexes
///        up to 255*8+255 = 2295 — out of bounds.  Fail closed.
bool bdf_valid(arch::PciBdf bdf) {
    return bdf.device < 32 && bdf.function < 8;
}

/// @brief Walks (and optionally allocates) the 4-level SL path for @p iova.
/// @param[out] path Physical address of every table page on the walk
///                  (path[0] = L4 root ... path[3] = L1 leaf table).
/// @param[out] idx  512-index used at each level (idx[3] = leaf index).
/// @return Pointer to the leaf (L1) entry, or nullptr when a table is
///         missing and !alloc, or a page allocation failed (fail-closed —
///         a failed walk leaves NO state: every page it linked is
///         unlinked and freed again before returning (no orphaned pages).
uint64_t *sl_walk(uint64_t root_phys, uint64_t iova, bool alloc,
                  uint64_t path[vtd::kSlLevels], size_t idx[vtd::kSlLevels]) {
    uint64_t table_phys = root_phys;
    // Pages linked by THIS walk: freshly allocated under the manager lock,
    // referenced only by entries this walk wrote — unwindable on failure.
    uint64_t *linked_entry[vtd::kSlLevels - 1];
    uint64_t linked_page[vtd::kSlLevels - 1];
    size_t nlinked = 0;
    for (size_t level = vtd::kSlLevels; level >= 1; --level) {
        size_t slot = vtd::kSlLevels - level;
        path[slot] = table_phys;
        size_t shift = 12 + 9 * (level - 1);
        idx[slot] = (iova >> shift) & 0x1FF;
        uint64_t *table = table_va(table_phys);
        uint64_t *entry = &table[idx[slot]];
        if (level == 1)
            return entry;
        uint64_t next = *entry & vtd::kSlEntryAddrMask;
        if (next == 0) {
            if (!alloc)
                return nullptr;
            uint64_t page = alloc_zeroed_table_page();
            if (page == 0) {
                // Unwind the links this walk made: unlink (parent entry = 0)
                // then free — freshly allocated single-owner pages, each
                // freed exactly once.  A failed walk leaves NO state behind.
                while (nlinked > 0) {
                    --nlinked;
                    *linked_entry[nlinked] = 0;
                    PMM::free_page(linked_page[nlinked]);
                }
                return nullptr;
            }
            *entry = page; // address bits 51:12, no permission flags
            linked_entry[nlinked] = entry;
            linked_page[nlinked] = page;
            ++nlinked;
            table_phys = page;
        } else {
            table_phys = next;
        }
    }
    return nullptr; // unreachable: loop covers all levels
}

/// @brief Frees every table page on @p path (except the root, path[0]) that
///        became empty after the leaf entry was cleared.  A table shared
///        with another mapping still has non-zero entries and is kept.
void cascade_free_empty(uint64_t path[vtd::kSlLevels],
                        size_t idx[vtd::kSlLevels]) {
    for (size_t lvl = vtd::kSlLevels - 1; lvl >= 1; --lvl) {
        uint64_t *table = table_va(path[lvl]);
        bool empty = true;
        for (size_t i = 0; i < vtd::kEntriesPerTable; ++i) {
            if (table[i] != 0) {
                empty = false;
                break;
            }
        }
        if (!empty)
            return; // parent chain stays occupied — nothing above to free
        PMM::free_page(path[lvl]);
        uint64_t *parent = table_va(path[lvl - 1]);
        parent[idx[lvl - 1]] = 0;
    }
}

/// @brief Clears one page's leaf entry and reclaims empty tables.
void clear_leaf(uint64_t root_phys, uint64_t iova) {
    uint64_t path[vtd::kSlLevels];
    size_t idx[vtd::kSlLevels];
    uint64_t *leaf = sl_walk(root_phys, iova, false, path, idx);
    if (leaf == nullptr)
        return; // table already gone — mapping already torn down
    *leaf = 0;
    cascade_free_empty(path, idx);
}

} // namespace

bool IoMmuManager::probe() {
    return g_present;
}

void IoMmuManager::force_present(bool present) {
    g_present = present;
}

int16_t IoMmuManager::domain_create(uint32_t task_id) {
    if (!g_present)
        return -1;
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    for (int16_t i = 0; i < static_cast<int16_t>(CONFIG_IOMMU_MAX_DOMAINS);
         ++i) {
        if (g_domains[i].occupied)
            continue;
        uint64_t root = alloc_zeroed_table_page();
        if (root == 0)
            return -1; // PMM exhausted — fail closed
        g_domains[i].sl_root_phys = root;
        g_domains[i].owner_task_id = task_id;
        g_domains[i].occupied = true;
        return i;
    }
    return -1; // domain table exhausted — fail closed
}

void IoMmuManager::domain_destroy(int16_t idx) {
    if (idx < 0 || idx >= static_cast<int16_t>(CONFIG_IOMMU_MAX_DOMAINS))
        return;
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    IoMmuDomain &d = g_domains[idx];
    if (!d.occupied)
        return;
    // Tear down every outstanding mapping.  clear_leaf reclaims now-empty
    // tables; the single-owner discipline guarantees each page is freed
    // exactly once (destroy walks only still-occupied records, and a record
    // is cleared before its entries are walked again).
    for (size_t m = 0; m < static_cast<size_t>(CONFIG_IOMMU_MAX_MAPPINGS);
         ++m) {
        IoMmuMapping &map = d.maps[m];
        if (!map.occupied)
            continue;
        for (size_t p = 0; p < map.pages; ++p)
            clear_leaf(d.sl_root_phys, map.phys + p * arch::PAGE_SIZE);
        map.occupied = false;
        map.phys = 0;
        map.pages = 0;
        map.sl_flags = 0;
    }
    if (d.sl_root_phys != 0) {
        PMM::free_page(d.sl_root_phys);
        d.sl_root_phys = 0;
    }
    d.owner_task_id = 0;
    d.occupied = false;
}

bool IoMmuManager::map_frame(int16_t idx, const cap::FrameCap &fc,
                             uint32_t sl_flags) {
    if (!g_present)
        return false;
    if (idx < 0 || idx >= static_cast<int16_t>(CONFIG_IOMMU_MAX_DOMAINS))
        return false;
    if (fc.count == 0 || sl_flags == 0)
        return false;
    if ((fc.phys & (arch::PAGE_SIZE - 1)) != 0)
        return false;
    if (fc.revoked())
        return false; // revoked authority must never (re)gain DMA access
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    IoMmuDomain &d = g_domains[idx];
    if (!d.occupied)
        return false;

    // Reject overlap with an existing mapping (one owner per IOVA range).
    uint64_t b0 = fc.phys;
    uint64_t b1 = b0 + fc.count * arch::PAGE_SIZE;
    for (size_t m = 0; m < static_cast<size_t>(CONFIG_IOMMU_MAX_MAPPINGS);
         ++m) {
        if (!d.maps[m].occupied)
            continue;
        uint64_t a0 = d.maps[m].phys;
        uint64_t a1 = a0 + d.maps[m].pages * arch::PAGE_SIZE;
        if (a0 < b1 && b0 < a1)
            return false;
    }

    // A free record is required up-front — no partial commit.
    int rec = -1;
    for (size_t m = 0; m < static_cast<size_t>(CONFIG_IOMMU_MAX_MAPPINGS);
         ++m) {
        if (!d.maps[m].occupied) {
            rec = static_cast<int>(m);
            break;
        }
    }
    if (rec < 0)
        return false; // mapping table exhausted — fail closed

    // Program every leaf entry.  On mid-walk failure roll back THIS call's
    // entries (cascade-free reclaims exactly the tables this call created —
    // shared tables survive because they still hold other mappings).
    for (size_t p = 0; p < fc.count; ++p) {
        uint64_t iova = fc.phys + p * arch::PAGE_SIZE;
        uint64_t path[vtd::kSlLevels];
        size_t walk_idx[vtd::kSlLevels];
        uint64_t *leaf = sl_walk(d.sl_root_phys, iova, true, path, walk_idx);
        if (leaf == nullptr) {
            for (size_t q = 0; q < p; ++q)
                clear_leaf(d.sl_root_phys, fc.phys + q * arch::PAGE_SIZE);
            return false;
        }
        *leaf = (iova & vtd::kSlEntryAddrMask) | sl_flags;
    }

    d.maps[rec].phys = fc.phys;
    d.maps[rec].pages = fc.count;
    d.maps[rec].sl_flags = sl_flags;
    d.maps[rec].occupied = true;
    return true;
}

bool IoMmuManager::unmap_frame(int16_t idx, const cap::FrameCap &fc) {
    if (!g_present)
        return false;
    if (idx < 0 || idx >= static_cast<int16_t>(CONFIG_IOMMU_MAX_DOMAINS))
        return false;
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    IoMmuDomain &d = g_domains[idx];
    if (!d.occupied)
        return false;
    int rec = -1;
    for (size_t m = 0; m < static_cast<size_t>(CONFIG_IOMMU_MAX_MAPPINGS);
         ++m) {
        if (d.maps[m].occupied && d.maps[m].phys == fc.phys &&
            d.maps[m].pages == fc.count) {
            rec = static_cast<int>(m);
            break;
        }
    }
    if (rec < 0)
        return false; // no such mapping — fail closed
    for (size_t p = 0; p < d.maps[rec].pages; ++p)
        clear_leaf(d.sl_root_phys, d.maps[rec].phys + p * arch::PAGE_SIZE);
    d.maps[rec].occupied = false;
    d.maps[rec].phys = 0;
    d.maps[rec].pages = 0;
    d.maps[rec].sl_flags = 0;
    return true;
}

bool IoMmuManager::attach_device(int16_t idx, arch::PciBdf bdf) {
    if (!g_present)
        return false;
    if (!bdf_valid(bdf))
        return false; // unencodable BDF — never index out of bounds
    if (idx < 0 || idx >= static_cast<int16_t>(CONFIG_IOMMU_MAX_DOMAINS))
        return false;
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    IoMmuDomain &d = g_domains[idx];
    if (!d.occupied)
        return false;
    // Find or claim a context-table slot for the bus.
    int slot = -1;
    for (size_t i = 0; i < static_cast<size_t>(CONFIG_IOMMU_MAX_BUSES); ++i) {
        if (g_ctx_bus_ids[i] == static_cast<int16_t>(bdf.bus)) {
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0) {
        for (size_t i = 0; i < static_cast<size_t>(CONFIG_IOMMU_MAX_BUSES);
             ++i) {
            if (g_ctx_bus_ids[i] < 0) {
                g_ctx_bus_ids[i] = static_cast<int16_t>(bdf.bus);
                for (size_t e = 0; e < 256; ++e)
                    g_ctx_tables[i][e] = Entry16{};
                slot = static_cast<int>(i);
                break;
            }
        }
    }
    if (slot < 0)
        return false; // context-table pool exhausted — fail closed

    uint64_t ctx_phys = kva_to_phys(&g_ctx_tables[slot][0]);
    Entry16 *re = &g_root_table[bdf.bus];
    re->lo = vtd::kRootEntryPresent | (ctx_phys & vtd::kRootEntryCtpMask);
    re->hi = 0;

    Entry16 *cte = &g_ctx_tables[slot][ctx_index(bdf)];
    cte->lo = vtd::kCtePresent | vtd::kCteTranslate |
              (d.sl_root_phys & vtd::kCteAsrMask);
    cte->hi = 0;
    return true;
}

void IoMmuManager::clear_attachment(int16_t idx, arch::PciBdf bdf) {
    if (idx < 0 || idx >= static_cast<int16_t>(CONFIG_IOMMU_MAX_DOMAINS))
        return;
    if (!bdf_valid(bdf))
        return; // unencodable BDF — never index out of bounds
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    if (!g_domains[idx].occupied)
        return;
    // Zero the root entry for the bus, and the context entry when a table
    // for this bus exists.
    g_root_table[bdf.bus].lo = 0;
    g_root_table[bdf.bus].hi = 0;
    for (size_t i = 0; i < static_cast<size_t>(CONFIG_IOMMU_MAX_BUSES); ++i) {
        if (g_ctx_bus_ids[i] == static_cast<int16_t>(bdf.bus)) {
            Entry16 *cte = &g_ctx_tables[i][ctx_index(bdf)];
            cte->lo = 0;
            cte->hi = 0;
            break;
        }
    }
}

uint64_t IoMmuManager::sl_root(int16_t idx) {
    if (idx < 0 || idx >= static_cast<int16_t>(CONFIG_IOMMU_MAX_DOMAINS))
        return 0;
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    if (!g_domains[idx].occupied)
        return 0;
    return g_domains[idx].sl_root_phys;
}

bool IoMmuManager::domain_valid(int16_t idx) {
    if (idx < 0 || idx >= static_cast<int16_t>(CONFIG_IOMMU_MAX_DOMAINS))
        return false;
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    return g_domains[idx].occupied;
}

size_t IoMmuManager::occupied_domains() {
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    size_t n = 0;
    for (size_t i = 0; i < static_cast<size_t>(CONFIG_IOMMU_MAX_DOMAINS); ++i)
        n += g_domains[i].occupied ? 1U : 0U;
    return n;
}

size_t IoMmuManager::mapping_count(int16_t idx) {
    if (idx < 0 || idx >= static_cast<int16_t>(CONFIG_IOMMU_MAX_DOMAINS))
        return 0;
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    if (!g_domains[idx].occupied)
        return 0;
    size_t n = 0;
    for (size_t m = 0; m < static_cast<size_t>(CONFIG_IOMMU_MAX_MAPPINGS);
         ++m)
        n += g_domains[idx].maps[m].occupied ? 1U : 0U;
    return n;
}

bool IoMmuManager::root_entry_present(uint8_t bus) {
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    return (g_root_table[bus].lo & vtd::kRootEntryPresent) != 0;
}

uint64_t IoMmuManager::context_asr(int16_t idx, arch::PciBdf bdf) {
    if (idx < 0 || idx >= static_cast<int16_t>(CONFIG_IOMMU_MAX_DOMAINS))
        return 0;
    if (!bdf_valid(bdf))
        return 0; // unencodable BDF — never index out of bounds
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    if (!g_domains[idx].occupied)
        return 0;
    for (size_t i = 0; i < static_cast<size_t>(CONFIG_IOMMU_MAX_BUSES); ++i) {
        if (g_ctx_bus_ids[i] != static_cast<int16_t>(bdf.bus))
            continue;
        Entry16 *cte = &g_ctx_tables[i][ctx_index(bdf)];
        if ((cte->lo & vtd::kCtePresent) == 0)
            return 0;
        return cte->lo & vtd::kCteAsrMask;
    }
    return 0;
}

} // namespace kernel::iommu
