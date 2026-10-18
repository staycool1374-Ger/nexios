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

#include <kernel/memory/vmm.hpp>
#include <kernel/memory/vmm_path.hpp> // issue #347 Slice D
#include <kernel/memory/vmm_internal.hpp> // issue #363 shared TU internals
#include <kernel/memory/pmm.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/cap/mmio.hpp>
#include <kernel/arch/pci.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/io.hpp>
#include <constants.hpp>
#include <kernel/arch/page_table.hpp>
#include <assert.hpp>
#include <kernel/memory/vmm_errors.hpp>
#include <kernel/arch/qemu_debugcon.hpp>

namespace kernel {

/// @brief Free all user-space pages and page tables owned by a user PML4.
///        Skips kernel-owned pages (PMM::is_user_page owner-bit check).
/// @param pml4_phys Physical address of the user PML4 to tear down.
/// @brief Free all user-accessible pages in a page table.
///        Worst-case iterations: PML4_USER_COUNT (256) × 512 × 512 × 512
///        leaf visits ≈ 34 billion in a fully populated 4-level walk.
///        In practice bounded by the actual mapped range (stack + heap +
///        code segments).  Yields every 64 leaf entries so higher-priority
///        tasks can run during teardown of large address spaces.
void VMM::free_user_pages(uint64_t pml4_phys) {
#if defined(CONFIG_ARCH_RISCV64)
    auto *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (pml4_phys & PAGE_TABLE_ADDR_MASK));
    // User space: L0 indices 0-255 for Sv39 (0-256GB).  Issue #206: all
    // address codecs below are shift-based Sv39 (raw & ~mask is x86-domain
    // and decodes 4x too high); L2 non-leaves are invalid in Sv39 (no L3).
    for (int l0_idx = 0; l0_idx < 256; ++l0_idx) {
        if (!(l0[l0_idx] & PAGE_PRESENT))
            continue;
        // Check for 1GB block at L0
        if ((l0[l0_idx] & (PAGE_READ | PAGE_WRITE | PAGE_EXEC))) {
            uint64_t page = VMM::sv39_pte_phys(l0[l0_idx]);
            if (!PMM::is_user_page(page))
                continue;
            PMM::free_page(page);
            continue;
        }
        uint64_t l1_phys = VMM::sv39_pte_phys(l0[l0_idx]);
        if (!PMM::is_user_page(l1_phys))
            continue;
        auto *l1 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l1_phys);
        for (int l1_idx = 0; l1_idx < 512; ++l1_idx) {
            if (!(l1[l1_idx] & PAGE_PRESENT))
                continue;
            // Check for 2MB block at L1
            if ((l1[l1_idx] & (PAGE_READ | PAGE_WRITE | PAGE_EXEC))) {
                uint64_t page = VMM::sv39_pte_phys(l1[l1_idx]);
                if (!PMM::is_user_page(page))
                    continue;
                PMM::free_page(page);
                continue;
            }
            uint64_t l2_phys = VMM::sv39_pte_phys(l1[l1_idx]);
            if (!PMM::is_user_page(l2_phys))
                continue;
            auto *l2 =
                reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l2_phys);
            for (int l2_idx = 0; l2_idx < 512; ++l2_idx) {
                if (!(l2[l2_idx] & PAGE_PRESENT))
                    continue;
                // Leaf PTE (V=1, R|W|X=1) — 4KB page in 3-level Sv39.
                if (!VMM::sv39_is_leaf(l2[l2_idx]))
                    continue;  // invalid non-leaf: leave alone
                uint64_t leaf = VMM::sv39_pte_phys(l2[l2_idx]);
                if (!PMM::is_user_page(leaf))
                    continue;
                PMM::free_page(leaf);
                if ((l2_idx & 0x3F) == 0x3F)
                    Scheduler::cleanup_step_try();
            }
            PMM::free_page(l2_phys);
            l1[l1_idx] = 0; // clear L1 entry to prevent re-walk
        }
        PMM::free_page(l1_phys);
        l0[l0_idx] = 0;
    }
#else
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (pml4_phys & PAGE_TABLE_ADDR_MASK));
    // v0.3.11: bounded diagnostic — log page-table pages skipped because their
    // owner bit is KERNEL (free_user_pages only frees USER-owned table pages).
    static uint64_t s_fup_skip_log = 0;
    auto log_skip = [](const char *lvl, uint64_t phys) {
        if (__atomic_load_n(&s_fup_skip_log, __ATOMIC_RELAXED) >=
            kFupSkipLogCap)
            return;
        __atomic_fetch_add(&s_fup_skip_log, 1UL, __ATOMIC_RELAXED);
        arch::QemuDebugcon::write(lvl);
        write_hex_debugcon(phys);
    };
    for (int pml4_idx = 0; pml4_idx < static_cast<int>(arch::PML4_USER_COUNT);
         ++pml4_idx) {
        if (!(pml4[pml4_idx] & PAGE_PRESENT))
            continue;
        uint64_t pdpt_phys = pml4[pml4_idx] & PAGE_TABLE_ADDR_MASK;
        if (!PMM::is_user_page(pdpt_phys)) {
            log_skip("[FUP-SKIP] pdpt p4=", pdpt_phys);
            continue;
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pdpt =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pdpt_phys);
        for (int pdpt_idx = 0; pdpt_idx < 512; ++pdpt_idx) {
            if (!(pdpt[pdpt_idx] & PAGE_PRESENT))
                continue;
            // Block/leaf test via is_table_entry (issue #363 P2/P4). The
            // 1 GiB base decode stays verbatim (issue #365 owns its
            // cross-arch re-derivation — do NOT touch kPdptBlockBaseMask).
            if (!is_table_entry(pdpt[pdpt_idx], 1)) {
                uint64_t page = pdpt[pdpt_idx] & kPdptBlockBaseMask;
                if (!PMM::is_user_page(page))
                    continue;
                PMM::free_page(page);
                continue;
            }
            uint64_t pd_phys = pdpt[pdpt_idx] & PAGE_TABLE_ADDR_MASK;
            if (!PMM::is_user_page(pd_phys)) {
                log_skip("[FUP-SKIP] pd   p4=", pd_phys);
                continue;
            }
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *pd =
                reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pd_phys);
            for (int pd_idx = 0; pd_idx < 512; ++pd_idx) {
                if (!(pd[pd_idx] & PAGE_PRESENT))
                    continue;
                // Arch block-leaf test + frame decode (issue #363 P2).
                if (!is_table_entry(pd[pd_idx], 2)) {
                    uint64_t page = vmm_arch_block_base(pd[pd_idx], true);
                    if (!PMM::is_user_page(page))
                        continue;
                    PMM::free_page(page);
                    continue;
                }
                uint64_t pt_phys = pd[pd_idx] & PAGE_TABLE_ADDR_MASK;
                if (!PMM::is_user_page(pt_phys)) {
                    log_skip("[FUP-SKIP] pt   p4=", pt_phys);
                    continue;
                }
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                auto *pt =
                    reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pt_phys);
                for (int pt_idx = 0; pt_idx < 512; ++pt_idx) {
                    if (!(pt[pt_idx] & PAGE_PRESENT))
                        continue;
                    // Arch leaf-frame decode (issue #363 P2).
                    uint64_t leaf = vmm_arch_block_base(pt[pt_idx], false);
                    if (!PMM::is_user_page(leaf))
                        continue;
                    PMM::free_page(leaf);
                    if ((pt_idx & 0x3F) == 0x3F)
                        Scheduler::cleanup_step_try();
                }
                PMM::free_page(pt_phys);
                pd[pd_idx] = 0; // clear PD entry to prevent re-walk
            }
            PMM::free_page(pd_phys);
            pdpt[pdpt_idx] = 0; // clear PDPT entry to prevent re-walk
        }
        PMM::free_page(pdpt_phys);
        pml4[pml4_idx] = 0;
    }
#endif
    // TLB flush is the caller's responsibility (via CR3 reload)
}

/// @brief Deep-copy user-space page tables from src_pml4 to dst_pml4.
///        Allocates new PDPT/PD/PT pages and copies data page content.
///        dst_pml4 must already have kernel entries populated.
///        Worst-case: PML4_USER_COUNT (256) × 512 × 512 × 512 leaf visits,
///        each doing a 4 KiB memcpy.  Yields every 64 leaf entries.
/// @return true on success, false on OOM.
bool VMM::deep_copy_user_pages(uint64_t src_pml4, uint64_t dst_pml4) {
#if defined(CONFIG_ARCH_X86_64) || defined(CONFIG_ARCH_AARCH64)
    auto *src = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                             (src_pml4 & PAGE_TABLE_ADDR_MASK));
    auto *dst = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                             (dst_pml4 & PAGE_TABLE_ADDR_MASK));

    for (int pml4_idx = 0; pml4_idx < static_cast<int>(arch::PML4_USER_COUNT);
         ++pml4_idx) {
        if (!(src[pml4_idx] & PAGE_PRESENT)) {
            dst[pml4_idx] = 0;
            continue;
        }

        uint64_t src_pdpt_phys = src[pml4_idx] & PAGE_TABLE_ADDR_MASK;
        uint64_t dst_pdpt_phys = PMM::alloc_user_page();
        if (!dst_pdpt_phys)
            return false;
        auto *dst_pdpt =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + dst_pdpt_phys);
        __builtin_memset(dst_pdpt, 0, 4096);
        // Arch table link (issue #363 P2: aarch64 descriptor vs x86 RWU).
        dst[pml4_idx] = vmm_arch_table_link(dst_pdpt_phys);

        auto *src_pdpt =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + src_pdpt_phys);
        for (int pdpt_idx = 0; pdpt_idx < 512; ++pdpt_idx) {
            if (!(src_pdpt[pdpt_idx] & PAGE_PRESENT))
                continue;

            // 1 GiB huge/block entry in the USER range — fail closed
            // (issue #325-E): copying by value would share one writable
            // page between parent and child (silent aliasing).  No kernel
            // path creates user huge entries (every map splits to 4 KiB),
            // so this is unreachable in practice; the caller destroys the
            // partial child on false.  Arch leaf test via is_table_entry.
            if (!is_table_entry(src_pdpt[pdpt_idx], 1)) {
                return false;
            }

            uint64_t src_pd_phys = src_pdpt[pdpt_idx] & PAGE_TABLE_ADDR_MASK;
            uint64_t dst_pd_phys = PMM::alloc_user_page();
            if (!dst_pd_phys)
                return false;
            auto *dst_pd =
                reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + dst_pd_phys);
            __builtin_memset(dst_pd, 0, 4096);
            // Arch table link (issue #363 P2).
            dst_pdpt[pdpt_idx] = vmm_arch_table_link(dst_pd_phys);

            auto *src_pd =
                reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + src_pd_phys);
            for (int pd_idx = 0; pd_idx < 512; ++pd_idx) {
                if (!(src_pd[pd_idx] & PAGE_PRESENT))
                    continue;

                // 2 MiB huge/block entry in the USER range — fail closed
                // (issue #325-E, same aliasing rationale as above).
                if (!is_table_entry(src_pd[pd_idx], 2)) {
                    return false;
                }

                uint64_t src_pt_phys = src_pd[pd_idx] & PAGE_TABLE_ADDR_MASK;
                uint64_t dst_pt_phys = PMM::alloc_user_page();
                if (!dst_pt_phys)
                    return false;
                auto *dst_pt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                            dst_pt_phys);
                __builtin_memset(dst_pt, 0, 4096);
                // Arch table link (issue #363 P2).
                dst_pd[pd_idx] = vmm_arch_table_link(dst_pt_phys);

                auto *src_pt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                            src_pt_phys);
                for (int pt_idx = 0; pt_idx < 512; ++pt_idx) {
                    if (!(src_pt[pt_idx] & PAGE_PRESENT))
                        continue;

                    uint64_t src_data =
                        vmm_arch_block_base(src_pt[pt_idx], false);
                    uint64_t flags = src_pt[pt_idx] & ~PAGE_FRAME_MASK;

                    // Issue #8 (S2): never deep-copy a non-RAM frame into the
                    // child.  A user MMIO-window PTE points at device phys
                    // (>= total RAM): memcpy'ing HHDM+device_phys reads device
                    // registers or raises a kernel-mode #PF (user-triggerable
                    // panic), and the child must not inherit a device mapping
                    // (fail-closed — the window stays unmapped in the child).
                    if (!PMM::is_user_page(src_data)) {
                        dst_pt[pt_idx] = 0;
                        continue;
                    }

                    uint64_t dst_data = PMM::alloc_user_page();
                    if (!dst_data)
                        return false;
                    __builtin_memcpy(
                        reinterpret_cast<void *>(arch::HHDM_OFFSET + dst_data),
                        reinterpret_cast<void *>(arch::HHDM_OFFSET + src_data),
                        4096);
                    dst_pt[pt_idx] = dst_data | flags;
                    if ((pt_idx & 0x3F) == 0x3F)
                        Scheduler::cleanup_step_try();
                }
            }
        }
    }
    return true;
#elif defined(CONFIG_ARCH_RISCV64)
    // RISC-V Sv39: L0 → L1 → L2 → leaf.  Issue #206: shift-based PTE
    // codec throughout (raw & ~mask decodes 4x too high); flags are bits
    // 9:0 only (kPageOffsetMask would capture PPN bits); no L3 exists in Sv39.
    auto *src = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                             (src_pml4 & PAGE_TABLE_ADDR_MASK));
    auto *dst = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                             (dst_pml4 & PAGE_TABLE_ADDR_MASK));

    for (int l0_idx = 0; l0_idx < 256; ++l0_idx) {
        if (!(src[l0_idx] & PAGE_PRESENT)) {
            dst[l0_idx] = 0;
            continue;
        }

        if ((src[l0_idx] & (PAGE_READ | PAGE_WRITE | PAGE_EXEC))) {
            // 1 GiB block in the USER range — fail closed (issue #325-E:
            // by-value copy would alias one writable page parent/child).
            return false;
        }

        uint64_t src_l1_phys = VMM::sv39_pte_phys(src[l0_idx]);
        uint64_t dst_l1_phys = PMM::alloc_user_page();
        if (!dst_l1_phys)
            return false;
        auto *dst_l1 =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + dst_l1_phys);
        __builtin_memset(dst_l1, 0, 4096);
        dst[l0_idx] = vmm_arch_table_link(dst_l1_phys);

        auto *src_l1 =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + src_l1_phys);
        for (int l1_idx = 0; l1_idx < 512; ++l1_idx) {
            if (!(src_l1[l1_idx] & PAGE_PRESENT))
                continue;

            if ((src_l1[l1_idx] & (PAGE_READ | PAGE_WRITE | PAGE_EXEC))) {
                // 2 MiB block in the USER range — fail closed (issue #325-E,
                // same aliasing rationale as above).
                return false;
            }

            uint64_t src_l2_phys = VMM::sv39_pte_phys(src_l1[l1_idx]);
            uint64_t dst_l2_phys = PMM::alloc_user_page();
            if (!dst_l2_phys)
                return false;
            auto *dst_l2 =
                reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + dst_l2_phys);
            __builtin_memset(dst_l2, 0, 4096);
            dst_l1[l1_idx] = vmm_arch_table_link(dst_l2_phys);

            auto *src_l2 =
                reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + src_l2_phys);
            for (int l2_idx = 0; l2_idx < 512; ++l2_idx) {
                if (!(src_l2[l2_idx] & PAGE_PRESENT))
                    continue;

                // RISC-V leaf entries have V=1, R|W|X=1.
                if (VMM::sv39_is_leaf(src_l2[l2_idx])) {
                    uint64_t src_data =
                        vmm_arch_block_base(src_l2[l2_idx], false);
                    uint64_t flags = src_l2[l2_idx] & 0x3FFULL;
                    if (!PMM::is_user_page(src_data)) {
                        dst_l2[l2_idx] = 0;
                        continue;
                    }
                    uint64_t dst_data = PMM::alloc_user_page();
                    if (!dst_data)
                        return false;
                    __builtin_memcpy(
                        reinterpret_cast<void *>(arch::HHDM_OFFSET + dst_data),
                        reinterpret_cast<void *>(arch::HHDM_OFFSET + src_data),
                        4096);
                    dst_l2[l2_idx] =
                        VMM::sv39_phys_pte(dst_data) | flags;
                } else {
                    // Non-leaf at L2 is invalid in Sv39 (no L3): skip.
                    dst_l2[l2_idx] = 0;
                }
            }
        }
    }
    return true;
#endif
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
/// @brief Translate a virtual address using a specific PML4.
/// @param virt_addr Virtual address.
/// @param pml4_phys Physical address of the target PML4.
/// @return Physical address, or 0 if not mapped.
uint64_t VMM::virt_to_phys_in_pml4(uint64_t virt_addr, uint64_t pml4_phys) {
    // Issue #378: shared index computation; the translate bodies stay
    // per-arch (block-leaf handling differs: Sv39 L1 2MB vs x86 PD 2MB,
    // plus the #206 Sv39 invalid-leaf fail-closed).
    size_t idx[4] = {0, 0, 0, 0};
    vmm_arch_walk_indices(virt_addr, idx);
#if defined(CONFIG_ARCH_RISCV64)
    auto *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (pml4_phys & PAGE_TABLE_ADDR_MASK));

    if (!(l0[idx[0]] & PAGE_PRESENT))
        return 0;
    // Arch child-table decode (issue #363 P2: Sv39 PTE codec vs mask).
    auto *l1 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            vmm_arch_table_child(l0[idx[0]]));

    if (!(l1[idx[1]] & PAGE_PRESENT))
        return 0;
    auto *l2 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            vmm_arch_table_child(l1[idx[1]]));

    // Check for 2MB block mapping (leaf at L1 level).  Arch block test +
    // base decode (issue #363 P2).
    if ((l1[idx[1]] & PAGE_PRESENT) && vmm_arch_split_needed(l1[idx[1]])) {
        return vmm_arch_block_base(l1[idx[1]], true) +
               (virt_addr & kHuge2MOffsetMask);
    }

    if (!(l2[idx[2]] & PAGE_PRESENT))
        return 0;
    // Issue #206: Sv39 has no level below L2 — a V=1,RWX=0 entry here is
    // invalid (never created by this backend); fail closed, do not descend
    // into garbage as the old 4-level fallback did.
    if (!VMM::sv39_is_leaf(l2[idx[2]]))
        return 0;
    return vmm_arch_block_base(l2[idx[2]], false) +
           (virt_addr & kPageOffsetMask);
#else
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (pml4_phys & PAGE_TABLE_ADDR_MASK));

    if (!(pml4[idx[0]] & PAGE_PRESENT))
        return 0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    // Arch child-table decode (issue #363 P2).
    auto *pdpt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              vmm_arch_table_child(
                                                  pml4[idx[0]]));

    if (!(pdpt[idx[1]] & PAGE_PRESENT))
        return 0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            vmm_arch_table_child(
                                                pdpt[idx[1]]));

    // Arch block-leaf test + base decode (issue #363 P2).
    if (vmm_arch_split_needed(pd[idx[2]])) {
        return vmm_arch_block_base(pd[idx[2]], true) +
               (virt_addr & kHuge2MOffsetMask);
    }

    if (!(pd[idx[2]] & PAGE_PRESENT))
        return 0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            vmm_arch_table_child(pd[idx[2]]));

    if (!(pt[idx[3]] & PAGE_PRESENT))
        return 0;
    return vmm_arch_block_base(pt[idx[3]], false) +
           (virt_addr & kPageOffsetMask);
#endif
}

} // namespace kernel
