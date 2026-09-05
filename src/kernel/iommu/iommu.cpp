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
/// @brief IoMmuManager implementation (issues #4 + #9).  All state is static
/// and bounded (.bss tables + per-domain PMM SL pages).  Single-owner page
/// discipline: every SL page belongs to exactly one domain and is freed
/// exactly once — either by cascade-free during unmap/rollback or by
/// domain_destroy walking only still-occupied mapping records.  PMM returns
/// unzeroed memory, so EVERY table page is memset(0) before use (a stale
/// R/W entry would grant DMA to recycled frames).
///
/// Phase-2 (#9): the live path is gated on `g_live` (real DMAR unit).  When
/// live, every map/unmap/destroy/attach that mutates SL or context entries
/// issues a queue-invalidation (IOTLB / context-cache) flush before
/// returning — a stale IOTLB translation would leak DMA across domains.
/// Register programming is serialized under the same leaf `g_lock_` and
/// never crosses a reschedule point.

#include <kernel/iommu/iommu.hpp>
#include <kernel/iommu/vtd.hpp>
#include <kernel/iommu/dmar.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <constants.hpp>
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/acpi.hpp>
#endif

namespace kernel::iommu {

using sync::SpinLock;

namespace {

SpinLock g_lock_{};
bool g_present = false;
bool g_live = false;
uint64_t g_mmio_base = 0;
bool g_translation_live = false;
/// @brief Cached DMAR probe result.  The ACPI tables / multiboot2 tags live
///        in low memory that test isolation rewinds, so a re-probe during a
///        test must NOT re-walk them — it reuses the first (boot-time) scan.
///        0 = never probed, 1 = hardware live, 2 = probed but absent.
[[maybe_unused]] int g_probe_result = 0;

/// @brief Static 4 KiB invalidation queue (phase-2, spec §6.5.1).  One
///        static page — no dynamic allocation on real-time paths.  16-byte
///        descriptors, head/tail managed by IQH/IQT registers.
alignas(arch::PAGE_SIZE) uint8_t g_iq[arch::PAGE_SIZE];

IoMmuDomain g_domains[CONFIG_IOMMU_MAX_DOMAINS];

/// @brief 16-byte root/context entry (VT-d legacy layout).
struct Entry16 {
    uint64_t lo = 0;
    uint64_t hi = 0;
};

/// @brief Root table: 256 bus entries x 16 bytes = exactly one 4 KiB page.
///        Page-aligned so its physical address is a valid RTADDR (bits 11:0
///        must be zero for the live path, phase-2).
alignas(arch::PAGE_SIZE) Entry16 g_root_table[256];
/// @brief Per-bus context tables (256 device:function entries each).  Each
///        bus table is exactly one 4 KiB page; page-aligned so the CTP field
///        (bits 51:12) is a valid context-table pointer.
alignas(arch::PAGE_SIZE) Entry16 g_ctx_tables[CONFIG_IOMMU_MAX_BUSES][256];
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

// ---------------------------------------------------------------------------
// Live VT-d register access (phase-2, issue #9).  ALL access is gated on
// g_live and serialized under g_lock_ (callers hold it).  The MMIO page is
// mapped once by probe_hardware() at HHDM_OFFSET + base (APIC pattern).
// ---------------------------------------------------------------------------

/// @brief True when a live unit is present (MMIO page mapped, VER validated).
bool live_active() {
    return g_live && g_mmio_base != 0;
}

/// @brief Read a 32-bit MMIO register of the live unit.
uint32_t mmio_read32(uint64_t off) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const volatile uint32_t *reg = reinterpret_cast<volatile uint32_t *>(
        arch::HHDM_OFFSET + g_mmio_base + off);
    return *reg;
}

/// @brief Write a 32-bit MMIO register of the live unit.
void mmio_write32(uint64_t off, uint32_t val) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    volatile uint32_t *reg = reinterpret_cast<volatile uint32_t *>(
        arch::HHDM_OFFSET + g_mmio_base + off);
    *reg = val;
}

/// @brief Read a 64-bit MMIO register of the live unit.
uint64_t mmio_read64(uint64_t off) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const volatile uint64_t *reg = reinterpret_cast<volatile uint64_t *>(
        arch::HHDM_OFFSET + g_mmio_base + off);
    return *reg;
}

/// @brief Write a 64-bit MMIO register of the live unit.
void mmio_write64(uint64_t off, uint64_t val) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    volatile uint64_t *reg = reinterpret_cast<volatile uint64_t *>(
        arch::HHDM_OFFSET + g_mmio_base + off);
    *reg = val;
}

/// @brief Bounded spin on a GSTS status bit.  Returns true when the bit set
///        within the retry budget; false on timeout (fail-closed — the
///        caller must NOT proceed with a half-enabled unit).
bool wait_gsts(uint32_t bit) {
    constexpr uint32_t kRetries = 100000;
    for (uint32_t i = 0; i < kRetries; ++i) {
        if ((mmio_read32(vtd::mmio::kGsts) & bit) != 0)
            return true;
    }
    return false;
}

/// @brief Submits a queue-invalidation descriptor of @p type (IOTLB / CTX)
///        and waits for the hardware to consume it (IQH catches IQT).
/// @return True when the invalidation completed; false on queue timeout.
bool qi_submit(uint64_t type) {
    if (!live_active())
        return true; // no hardware — nothing to flush (software-only mode)
    // The queue must be enabled first (GCMD.QIE, GSTS.QIES).
    if ((mmio_read32(vtd::mmio::kGcmd) & vtd::mmio::kGcmdQie) == 0) {
        uint64_t iq_phys = kva_to_phys(&g_iq[0]);
        if ((iq_phys & (arch::PAGE_SIZE - 1)) != 0)
            return false; // unaligned queue — never submit
        __builtin_memset(g_iq, 0, sizeof(g_iq));
        mmio_write64(vtd::mmio::kIqh, 0); // reset head to 0 before QEN
        mmio_write64(vtd::mmio::kIqa, (iq_phys & vtd::mmio::kIqaAddrMask) |
                                          vtd::mmio::kIqaQen);
        mmio_write32(vtd::mmio::kGcmd,
                     mmio_read32(vtd::mmio::kGcmd) | vtd::mmio::kGcmdQie);
        if (!wait_gsts(vtd::mmio::kGstsQies))
            return false;
    }
    // IQT/IQH hold the descriptor byte offset >> 4 (16-byte descriptors).
    uint32_t tail = static_cast<uint32_t>(mmio_read64(vtd::mmio::kIqt) >> 4);
    size_t idx = static_cast<size_t>(tail & 0xFF);
    size_t off = idx * 16;
    uint64_t *desc = reinterpret_cast<uint64_t *>(g_iq + off);
    desc[0] = type | vtd::kQiGranGlobal;
    desc[1] = 0;
    // NOTE: the static g_iq buffer is write-back cached; QEMU's memory model
    // is coherent so no explicit cache flush is needed here.  On real
    // non-coherent hardware the descriptor would need a wbinvd/clflush before
    // the IQT advance makes it visible to the IOMMU — documented follow-up.
    // Advance the tail: write the new byte offset (index << 4).
    uint32_t new_tail = (tail + 1) & 0xFF;
    mmio_write64(vtd::mmio::kIqt, static_cast<uint64_t>(new_tail) << 4);
    // Wait for IQH to reach the new tail (descriptor consumed).
    constexpr uint32_t kRetries = 200000;
    for (uint32_t i = 0; i < kRetries; ++i) {
        if ((mmio_read64(vtd::mmio::kIqh) >> 4) == new_tail)
            return true;
    }
    return false;
}

/// @brief Global IOTLB invalidation (after SL-table mutation).
bool iotlb_invalidate() {
    return qi_submit(vtd::kQiTypeIotlb);
}

/// @brief Global context-cache invalidation (after root/context mutation).
bool ctx_cache_invalidate() {
    return qi_submit(vtd::kQiTypeCtx);
}

/// @brief Enumerates PCI config space and programs a T=0 passthrough
///        context entry for every kernel-owned device (AHCI class 0x0106,
///        virtio vendor 0x1AF4).  Returns false (fail-closed, TE stays off)
///        if the context-table pool exhausts or any BDF is unencodable.
bool passthrough_kernel_devices() {
    int n = 0;
    // Scan the PCI buses that can host kernel DMA devices.  Bounded by the
    // context-table pool (CONFIG_IOMMU_MAX_BUSES) — a bus beyond the pool
    // cannot get a passthrough entry, so the pre-pass fails closed rather
    // than leaving a DMA-capable kernel device without a context entry.
    for (uint8_t bus = 0; bus < static_cast<uint8_t>(CONFIG_IOMMU_MAX_BUSES);
         ++bus) {
        for (uint8_t dev = 0; dev < 32; ++dev) {
            for (uint8_t fn = 0; fn < 8; ++fn) {
                arch::PciBdf bdf{bus, dev, fn};
                uint64_t addr = arch::pci_make_addr(bdf, 0);
                uint16_t vendor = arch::pci_config_readw(addr);
                if (vendor == 0xFFFF || vendor == 0x0000)
                    continue; // no device present at this BDF
                // AHCI: class 0x0106 at config offset 0x0A (word).
                uint16_t class_rev = arch::pci_config_readw(
                    arch::pci_make_addr(bdf, 0x0A));
                uint8_t base_class = static_cast<uint8_t>(class_rev >> 8);
                uint8_t sub_class = static_cast<uint8_t>(class_rev & 0xFF);
                bool is_ahci = (base_class == 0x01 && sub_class == 0x06);
                bool is_virtio = (vendor == 0x1AF4);
                if (!is_ahci && !is_virtio)
                    continue; // not a kernel DMA device — leave to user
                if (n >= static_cast<int>(CONFIG_IOMMU_MAX_KERNEL_DEVICES))
                    return false;
                // Find or claim a context-table slot for the bus.
                int slot = -1;
                for (size_t i = 0;
                     i < static_cast<size_t>(CONFIG_IOMMU_MAX_BUSES); ++i) {
                    if (g_ctx_bus_ids[i] == static_cast<int16_t>(bdf.bus)) {
                        slot = static_cast<int>(i);
                        break;
                    }
                }
                if (slot < 0) {
                    for (size_t i = 0;
                         i < static_cast<size_t>(CONFIG_IOMMU_MAX_BUSES); ++i) {
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
                    return false; // context-table pool exhausted
                uint64_t ctx_phys = kva_to_phys(&g_ctx_tables[slot][0]);
                Entry16 *cte = &g_ctx_tables[slot][ctx_index(bdf)];
                // A device already attached to a translation domain (T=1)
                // must NOT be downgraded to T=0 passthrough — that would
                // silently defeat its DMA isolation.  (attach_device re-points
                // the entry later; this pre-pass only adds a T=0 fallback for
                // kernel DMA.)
                if ((cte->lo & vtd::kCtePresent) != 0 &&
                    (cte->lo & vtd::kCteTtMask) != vtd::kCteTtPassthrough)
                    continue;
                Entry16 *re = &g_root_table[bdf.bus];
                re->lo = vtd::kRootEntryPresent |
                         (ctx_phys & vtd::kRootEntryCtpMask);
                re->hi = 0;
                // T=0 passthrough: Present + TT=10b (spec §9.3).
                cte->lo = vtd::kCtePresent | vtd::kCteTtPassthrough;
                cte->hi = 0;
                ++n;
            }
        }
    }
    return true;
}

} // namespace

bool IoMmuManager::probe() {
    return g_present || g_live;
}

void IoMmuManager::force_present(bool present) {
    g_present = present;
    if (!present) {
        // A software-only presence means no live unit authority.  Clearing
        // g_live here keeps the cap_iommu "no hardware" tests deterministic
        // even when a live unit was detected at boot (q35 variant): they
        // force-absent the manager and expect probe()==false.  g_mmio_base
        // is a pure hardware fact (persists; probe_hardware() restores the
        // live authority from the cached result).
        g_live = false;
        g_translation_live = false;
    }
}

bool IoMmuManager::probe_hardware() {
#if defined(CONFIG_ARCH_X86_64)
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    // NOTE: the initial ACPI scan + MMIO map_page run under g_lock_.  This
    // is safe because the scan path executes only at boot (single-threaded,
    // before the scheduler spawns tasks) — the cached re-probe path used by
    // tests (g_probe_result==1/2) never re-scans or re-maps.  Holding the
    // lock here keeps the g_mmio_base/g_live write atomic with the scan.
    // The DMAR scan reads multiboot2/ACPI tables in low memory.  Test
    // isolation rewinds low memory between tests, so a re-probe during a
    // test must reuse the cached boot-time result rather than re-walking
    // (which would dereference a stale multiboot2 info pointer).
    if (g_live)
        return true; // already probed and live
    if (g_probe_result == 1) {
        // Probed at boot, unit live; force_present(false) dropped g_live.
        // Restore the live authority from the cached result without
        // re-scanning ACPI.
        g_live = true;
        g_present = true;
        return true;
    }
    if (g_probe_result == 2)
        return false; // probed earlier, unit absent — stable
    dmar::DmarInfo info = acpi::scan_dmar();
    if (!info.found || !info.unit.present) {
        g_probe_result = 2;
        return false; // no DMAR / no DRHD — software-only path
    }
    g_mmio_base = info.unit.base_phys;
    // Map the remapping unit's MMIO page (APIC pattern).
    kernel::VMM::map_page(arch::HHDM_OFFSET + g_mmio_base, g_mmio_base,
                          false);
    uint32_t ver = mmio_read32(vtd::mmio::kVerReg);
    // VER_REG is non-zero on every VT-d unit; the encoding differs between
    // QEMU (0x10) and real hardware (e.g. 0x100 = v1.0), so only reject an
    // absent/zero unit (fail closed on a non-VT-d MMIO page).
    if (ver == 0) {
        g_mmio_base = 0;
        g_probe_result = 2;
        return false; // not a live VT-d unit — fail closed
    }
    g_live = true;
    g_present = true;
    g_probe_result = 1;
    return true;
#else
    (void)0;
    return false;
#endif
}

bool IoMmuManager::enable_translation() {
    if (!live_active())
        return false;
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    // Never re-enable: if TE is already live, report success.
    if ((mmio_read32(vtd::mmio::kGsts) & vtd::mmio::kGstsTes) != 0) {
        g_translation_live = true;
        return true;
    }
    // 1) RTADDR must point at the root table BEFORE TE.  Fail-closed: an
    //    unaligned root table would be an invalid root-table pointer.
    uint64_t root_phys = kva_to_phys(&g_root_table[0]);
    if ((root_phys & (arch::PAGE_SIZE - 1)) != 0)
        return false;
    mmio_write64(vtd::mmio::kRtaddr, root_phys & vtd::mmio::kRtaddrAddrMask);
    mmio_write32(vtd::mmio::kGcmd,
                 mmio_read32(vtd::mmio::kGcmd) | vtd::mmio::kGcmdSrtp);
    if (!wait_gsts(vtd::mmio::kGstsRtps))
        return false; // RTADDR never took — do NOT enable TE
    // 2) Passthrough pre-pass: kernel devices must have T=0 entries before
    //    TE=1, otherwise kernel DMA is blocked (device hang).
    if (!passthrough_kernel_devices())
        return false;
    if (!ctx_cache_invalidate())
        return false;
    // 3) TE last.
    mmio_write32(vtd::mmio::kGcmd,
                 mmio_read32(vtd::mmio::kGcmd) | vtd::mmio::kGcmdTe);
    if (!wait_gsts(vtd::mmio::kGstsTes)) {
        // Fail-closed: clear TE and drop live state so no half-enabled unit.
        // g_mmio_base is a pure hardware fact — keep it so the cached
        // re-probe (g_probe_result==1) can restore the live authority and a
        // later enable_translation() retry can re-arm the unit.
        mmio_write32(vtd::mmio::kGcmd,
                     mmio_read32(vtd::mmio::kGcmd) & ~vtd::mmio::kGcmdTe);
        g_live = false;
        g_translation_live = false;
        return false;
    }
    g_translation_live = true;
    return true;
}

void IoMmuManager::read_clear_faults() {
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    if (!live_active())
        return;
    // FECTL: mask the fault-event interrupt (IM=1) so no spurious MSI;
    // read-clear FSTS (writing the pending bits clears them).
    mmio_write32(vtd::mmio::kFectl, vtd::mmio::kFectlIm |
                                        mmio_read32(vtd::mmio::kFectl));
    uint32_t fsts = mmio_read32(vtd::mmio::kFsts);
    if (fsts != 0)
        mmio_write32(vtd::mmio::kFsts, fsts);
}

uint64_t IoMmuManager::mmio_base() {
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    return g_mmio_base;
}

bool IoMmuManager::translation_live() {
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    return g_translation_live && live_active();
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
    // Live path: flush the IOTLB so a cached translation cannot reach the
    // now-freed SL pages (DMA into recycled frames would be a UAF).
    iotlb_invalidate();
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
    // Live path: flush the IOTLB so no stale translation for this IOVA
    // survives (spec §6.5).  A flush failure leaves the mapping already
    // programmed but reports false — the caller fails closed and tears the
    // domain down (revoke) rather than trusting an unflushed mapping.
    return iotlb_invalidate();
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
    // Live path: flush the IOTLB so the removed mapping cannot be reached
    // through a cached translation.
    return iotlb_invalidate();
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
    cte->lo = vtd::kCtePresent | vtd::kCteTtTranslate |
              (d.sl_root_phys & vtd::kCteAsrMask);
    cte->hi = 0;
    // Live path: flush the context cache so the device immediately sees the
    // new root (spec §6.5.2.2).
    return ctx_cache_invalidate();
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
    // Live path: flush the context cache so the removal is visible.
    ctx_cache_invalidate();
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

uint64_t IoMmuManager::context_entry(arch::PciBdf bdf) {
    if (!bdf_valid(bdf))
        return 0; // unencodable BDF — never index out of bounds
    SpinLockGuard<sync::SpinLock> guard(g_lock_);
    for (size_t i = 0; i < static_cast<size_t>(CONFIG_IOMMU_MAX_BUSES); ++i) {
        if (g_ctx_bus_ids[i] != static_cast<int16_t>(bdf.bus))
            continue;
        Entry16 *cte = &g_ctx_tables[i][ctx_index(bdf)];
        if ((cte->lo & vtd::kCtePresent) == 0)
            return 0;
        return cte->lo;
    }
    return 0;
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
