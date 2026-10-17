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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
/// @brief Map a 4 KiB page in the kernel page table.
/// @param virt_addr Page-aligned virtual address.
/// @param phys_addr Page-aligned physical address.
/// @param user      If true, mark the page as user-accessible.
errors::VmmError VMM::map_page_try(uint64_t virt_addr, uint64_t phys_addr,
                                      bool user) {
#if defined(CONFIG_ARCH_RISCV64)
    // Sv39 3-level page table walk
    auto *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (kernel_pml4_ & PAGE_TABLE_ADDR_MASK));

    size_t l0_idx = (virt_addr & VMM::L0_MASK) >> VMM::L0_SHIFT;
    size_t l1_idx = (virt_addr & VMM::L1_MASK) >> VMM::L1_SHIFT;
    size_t l2_idx = (virt_addr & VMM::L2_MASK) >> VMM::L2_SHIFT;

    // Issue #152: snapshot pairing for Sv39 (see arm_test_touch_flags).
    // No identity flag on riscv64: the boot identity map is dead at
    // runtime, so low-half maps arm nothing.
    // Allow kernel-space VAs when tests are active: the Sv39 L1
    // save/restore in the snapshot mechanism undoes any 2MB-block splits.
    warn_test_kernel_touch(l0_idx == kHhdmL0Idx, TestTouchWarn::kMapL1,
                           virt_addr, l0_idx);
    arm_test_touch_flags(l0_idx == kHhdmL0Idx, false);

    auto *l1 = get_table(l0, l0_idx, true);
    if (!l1) [[unlikely]] {
        note_silent_drop_once("[MAP-DROP] l1 ", virt_addr);
        return errors::VMM_ERR_PAGE_ALLOC;
    }

    // If L1 entry is a 2MB block, split it into 512 4KB entries.
    if ((l1[l1_idx] & PAGE_PRESENT) &&
        vmm_arch_split_needed(l1[l1_idx])) {
        uint64_t new_l2_phys = PMM::alloc_page_table();
        if (!new_l2_phys) [[unlikely]] {
            note_silent_drop_once("[MAP-DROP] split ", virt_addr);
            return errors::VMM_ERR_PAGE_ALLOC;
        }
        auto *new_l2 =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + new_l2_phys);
        // Arch split fill + link (issue #363 P2).
        vmm_arch_split_fill(new_l2, l1[l1_idx]);
        l1[l1_idx] = vmm_arch_table_link(new_l2_phys);
    }

    auto *l2 = get_table(l1, l1_idx, true);
    if (!l2) [[unlikely]] {
        note_silent_drop_once("[MAP-DROP] l2 ", virt_addr);
        return errors::VMM_ERR_PAGE_ALLOC;
    }

    if (user && phys_addr < PMM::total_memory())
        ENSURE(PMM::is_user_page(phys_addr) &&
               "map_page: KERNEL page mapped as user-accessible");

    l2[l2_idx] = vmm_arch_encode_kernel_leaf(phys_addr, user);
    arch::ArchPageTable::tlb_flush(virt_addr);
    return errors::VMM_ERR_OK;
#else
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (kernel_pml4_ & PAGE_TABLE_ADDR_MASK));

    size_t pml4_idx = arch::ArchPageTable::pml4_index(virt_addr);

    // Allow kernel-space VAs when tests are active: PD save/restore in the
    // snapshot mechanism undoes any huge-page splits.  Boot-time calls (APIC
    // MMIO mapping, etc.) are still permitted as they run before snapshot.
    warn_test_kernel_touch(pml4_idx >= arch::PML4_USER_COUNT,
                           TestTouchWarn::kMapPd, virt_addr, pml4_idx);
    arm_test_touch_flags(pml4_idx >= arch::PML4_USER_COUNT, false);
    // Low identity-map VAs (pml4_idx 0, PD_IDENTITY phys 0x3000): a map_page
    // here splits a boot 2 MiB identity huge entry into a PT page.  Flag it so
    // snapshot_restore restores PD_IDENTITY (see arm_test_touch_flags).
    arm_test_touch_flags(false, pml4_idx < arch::PML4_USER_COUNT);

    size_t pdpt_idx = arch::ArchPageTable::pdpt_index(virt_addr);
    size_t pd_idx = arch::ArchPageTable::pd_index(virt_addr);
    size_t pt_idx = arch::ArchPageTable::pt_index(virt_addr);

    auto *pdpt = get_table(pml4, pml4_idx, true);
    if (!pdpt) [[unlikely]] {
        note_silent_drop_once("[MAP-DROP] pdpt ", virt_addr);
        return errors::VMM_ERR_PAGE_ALLOC;
    }
    auto *pd = get_table(pdpt, pdpt_idx, true);
    if (!pd) [[unlikely]] {
        note_silent_drop_once("[MAP-DROP] pd ", virt_addr);
        return errors::VMM_ERR_PAGE_ALLOC;
    }

    // If the PD entry is a 2MB huge page, split it into 512 4KB entries
    // so we can map an individual 4KB page within it.
    if (vmm_arch_split_needed(pd[pd_idx])) {
        uint64_t new_pt_phys = PMM::alloc_page_table();
        if (!new_pt_phys) [[unlikely]] {
            note_silent_drop_once("[MAP-DROP] split ", virt_addr);
            return errors::VMM_ERR_PAGE_ALLOC;
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *new_pt =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + new_pt_phys);
        // Arch split fill + link (issue #363 P2): huge-base decode, flag
        // preservation, and the 512-entry fill live in the per-arch TU.
        vmm_arch_split_fill(new_pt, pd[pd_idx]);
        pd[pd_idx] = vmm_arch_table_link(new_pt_phys);
    }

    auto *pt = get_table(pd, pd_idx, true);
    if (!pt) [[unlikely]] {
        note_silent_drop_once("[MAP-DROP] pt ", virt_addr);
        return errors::VMM_ERR_PAGE_ALLOC;
    }

    if (user && phys_addr < PMM::total_memory())
        ENSURE(PMM::is_user_page(phys_addr) &&
               "map_page: KERNEL page mapped as user-accessible");

    pt[pt_idx] = vmm_arch_encode_kernel_leaf(phys_addr, user);
    arch::ArchPageTable::tlb_flush(virt_addr);
#endif
    return errors::VMM_ERR_OK;
}

errors::VmmError VMM::map_page_in_pml4_try(uint64_t virt_addr,
                                              uint64_t phys_addr, bool user,
                                              bool executable,
                                              uint64_t pml4_phys) {
#if defined(CONFIG_ARCH_RISCV64)
    // Sv39 3-level page table walk
    auto *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (pml4_phys & PAGE_TABLE_ADDR_MASK));

    size_t l0_idx = (virt_addr & VMM::L0_MASK) >> VMM::L0_SHIFT;
    size_t l1_idx = (virt_addr & VMM::L1_MASK) >> VMM::L1_SHIFT;
    size_t l2_idx = (virt_addr & VMM::L2_MASK) >> VMM::L2_SHIFT;

    // Issue #152 (mirrors #197 on x86_64): flag unconditionally — no warn
    // here (designated private-table API), but the target may be the
    // shared kernel table or share the HHDM L1 with it (riscv64 clones
    // copy L0[256..511] by value), so a split must arm the snapshot L1
    // rewind.  Without this, splits never trigger it and persist across
    // boundaries.  HHDM only (no live identity map on riscv64 — see
    // map_page note above).
    arm_test_touch_flags(l0_idx == kHhdmL0Idx, false);

    auto *l1 = get_table(l0, l0_idx, true, true);
    if (!l1) [[unlikely]] {
        note_silent_drop_once("[MAP-DROP] l1 ", virt_addr);
        return errors::VMM_ERR_PAGE_ALLOC;
    }
    auto *l2 = get_table(l1, l1_idx, true, true);
    if (!l2) [[unlikely]] {
        note_silent_drop_once("[MAP-DROP] l2 ", virt_addr);
        return errors::VMM_ERR_PAGE_ALLOC;
    }

    if (user && phys_addr < PMM::total_memory())
        ENSURE(PMM::is_user_page(phys_addr) &&
               "map_page_in_pml4: KERNEL page mapped as user-accessible");

    l2[l2_idx] = vmm_arch_encode_map_leaf(phys_addr, user, executable);
#else
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (pml4_phys & PAGE_TABLE_ADDR_MASK));

    size_t pml4_idx = arch::ArchPageTable::pml4_index(virt_addr);
    size_t pdpt_idx = arch::ArchPageTable::pdpt_index(virt_addr);
    size_t pd_idx = arch::ArchPageTable::pd_index(virt_addr);
    size_t pt_idx = arch::ArchPageTable::pt_index(virt_addr);

    // Issue #197: mirror map_page's modified-flags (no warn here — this is
    // the designated private-table API, but the target may be the shared
    // kernel table or share PD pages with it, so flag unconditionally).
    // Without this, splits (e.g. test mb2 identity re-mapping) never
    // trigger the snapshot PD rewind and persist across boundaries.
    arm_test_touch_flags(pml4_idx >= arch::PML4_USER_COUNT,
                         pml4_idx < arch::PML4_USER_COUNT);

    auto *pdpt = get_table(pml4, pml4_idx, true, true);
    if (!pdpt) [[unlikely]] {
        note_silent_drop_once("[MAP-DROP] pdpt ", virt_addr);
        return errors::VMM_ERR_PAGE_ALLOC;
    }
    auto *pd = get_table(pdpt, pdpt_idx, true, true);
    if (!pd) [[unlikely]] {
        note_silent_drop_once("[MAP-DROP] pd ", virt_addr);
        return errors::VMM_ERR_PAGE_ALLOC;
    }

    if (vmm_arch_split_needed(pd[pd_idx])) {
        uint64_t new_pt_phys = PMM::alloc_user_page();
        if (!new_pt_phys) [[unlikely]] {
            note_silent_drop_once("[MAP-DROP] split ", virt_addr);
            return errors::VMM_ERR_PAGE_ALLOC;
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *new_pt =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + new_pt_phys);
        // Arch split fill + link (issue #363 P2).
        vmm_arch_split_fill(new_pt, pd[pd_idx]);
        pd[pd_idx] = vmm_arch_table_link(new_pt_phys);
    }

    auto *pt = get_table(pd, pd_idx, true, true);
    if (!pt) [[unlikely]] {
        note_silent_drop_once("[MAP-DROP] pt ", virt_addr);
        return errors::VMM_ERR_PAGE_ALLOC;
    }

    if (user && phys_addr < PMM::total_memory())
        ENSURE(PMM::is_user_page(phys_addr) &&
               "map_page_in_pml4: KERNEL page mapped as user-accessible");

    pt[pt_idx] = vmm_arch_encode_map_leaf(phys_addr, user, executable);
#endif
    // Issue #225: invalidate like every other map/unmap site in this
    // file. Harmless when these tables are not current (local INVLPG);
    // required when they are (present→present remaps would otherwise
    // serve stale TLB/PCID entries, globals included).
    arch::ArchPageTable::tlb_flush(virt_addr);
    return errors::VMM_ERR_OK;
}

uint64_t *VMM::pte_leaf_in_pml4(uint64_t virt_addr, uint64_t pml4_phys) {
    // Walk-only leaf lookup (private helper for the RO primitive +
    // inspector; mirrors the unmap walks with create=false).
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *top = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                             (pml4_phys & PAGE_TABLE_ADDR_MASK));
#if defined(CONFIG_ARCH_RISCV64)
    size_t l0_idx = (virt_addr & VMM::L0_MASK) >> VMM::L0_SHIFT;
    size_t l1_idx = (virt_addr & VMM::L1_MASK) >> VMM::L1_SHIFT;
    size_t l2_idx = (virt_addr & VMM::L2_MASK) >> VMM::L2_SHIFT;
    // Table-or-leaf discrimination (issue #363 P2/P4): a present leaf
    // at L0/L1 is not ours to descend — is_table_entry owns the arch rule.
    if (!VMM::is_table_entry(top[l0_idx], 0))
        return nullptr;
    auto *l1 = VMM::get_table(top, l0_idx, false);
    if (!l1)
        return nullptr;
    if (!VMM::is_table_entry(l1[l1_idx], 1))
        return nullptr;
    auto *l2 = VMM::get_table(l1, l1_idx, false);
    if (!l2)
        return nullptr;
    if ((l2[l2_idx] & VMM::PAGE_PRESENT) == 0)
        return nullptr;
    if (VMM::is_table_entry(l2[l2_idx], 2))
        return nullptr; // table pointer, not a leaf
    return &l2[l2_idx];
#else
    size_t pml4_idx = arch::ArchPageTable::pml4_index(virt_addr);
    size_t pdpt_idx = arch::ArchPageTable::pdpt_index(virt_addr);
    size_t pd_idx = arch::ArchPageTable::pd_index(virt_addr);
    size_t pt_idx = arch::ArchPageTable::pt_index(virt_addr);
    auto *pdpt = VMM::get_table(top, pml4_idx, false);
    if (!pdpt)
        return nullptr;
    auto *pd = VMM::get_table(pdpt, pdpt_idx, false);
    if (!pd)
        return nullptr;
    if (!VMM::is_table_entry(pd[pd_idx], 2))
        return nullptr; // Block/huge entry: not ours.
    auto *pt = VMM::get_table(pd, pd_idx, false);
    if (!pt)
        return nullptr;
    if ((pt[pt_idx] & VMM::PAGE_PRESENT) == 0)
        return nullptr;
    return &pt[pt_idx];
#endif
}

// Issue #271 P0: shared read-only user-map primitive. Map RW first
// (identical table allocation to every other user map), then clear the
// write permission with the arch RO encoding. Tables under construction
// are never live, so no TLB window exists; the trailing flush mirrors
// every other map site (harmless when not current, required on remap).
void VMM::map_page_ro_in_pml4(uint64_t virt_addr, uint64_t phys_addr,
                              bool executable, uint64_t pml4_phys) {
    if ((virt_addr & (arch::PAGE_SIZE - 1)) != 0 ||
        (phys_addr & (arch::PAGE_SIZE - 1)) != 0) {
        return;
    }
    map_page_in_pml4(virt_addr, phys_addr, true, executable, pml4_phys);
    uint64_t *leaf = pte_leaf_in_pml4(virt_addr, pml4_phys);
    if (!leaf)
        return;
    // Arch RO encoding (issue #363 P2): aarch64 AP_RO, riscv W-clear,
    // x86_64 RW-clear (+NX when non-executable, idempotent here).
    *leaf = vmm_arch_encode_ro(*leaf, executable);
    arch::ArchPageTable::tlb_flush(virt_addr);
}

bool VMM::pte_is_ro_in_pml4(uint64_t virt_addr, uint64_t pml4_phys,
                            bool *executable_out) {
    uint64_t *leaf = pte_leaf_in_pml4(virt_addr, pml4_phys);
    if (!leaf)
        return false;
    return vmm_arch_leaf_is_ro(*leaf, executable_out);
}

/// @brief Test inspector: reports whether the mapped leaf is executable
///        per the arch NX encoding (walk-only, never allocates).
/// @param virt_addr Virtual address to inspect.
/// @param pml4_phys Physical address of the target PML4.
/// @return false when unmapped (or a huge/block entry, not ours).
bool VMM::pte_is_executable_in_pml4(uint64_t virt_addr, uint64_t pml4_phys) {
    uint64_t *leaf = pte_leaf_in_pml4(virt_addr, pml4_phys);
    if (!leaf) {
        return false;
    }
    return vmm_arch_leaf_is_executable(*leaf);
}

void VMM::remap_range_ro_in_pml4(uint64_t pml4_phys, uint64_t vaddr,
                                 uint64_t size, bool executable) {
    if (size == 0)
        return;
    const uint64_t mask = arch::PAGE_SIZE - 1;
    uint64_t base = vaddr & ~mask;
    if (vaddr + size < vaddr)
        return; // overflow: no-op
    uint64_t end = (vaddr + size + mask) & ~mask;
    if (end <= base)
        return;
    for (uint64_t va = base; va < end; va += arch::PAGE_SIZE) {
        uint64_t phys = virt_to_phys_in_pml4(va, pml4_phys);
        if (!phys)
            continue; // unmapped hole: skip
        map_page_ro_in_pml4(va, phys, executable, pml4_phys);
    }
}

void VMM::unmap_page_in_pml4(uint64_t virt_addr, uint64_t pml4_phys) {
    if ((virt_addr & (arch::PAGE_SIZE - 1)) != 0) {
        return;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *top = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                             (pml4_phys & PAGE_TABLE_ADDR_MASK));
#if defined(CONFIG_ARCH_RISCV64)
    // Sv39 3-level walk (mirrors map_page_in_pml4 riscv branch).
    size_t l0_idx = (virt_addr & VMM::L0_MASK) >> VMM::L0_SHIFT;
    size_t l1_idx = (virt_addr & VMM::L1_MASK) >> VMM::L1_SHIFT;
    size_t l2_idx = (virt_addr & VMM::L2_MASK) >> VMM::L2_SHIFT;
    // Issue #152: no identity flag on riscv64 (dead boot map — see
    // map_page note above). Task-private low tables need no snapshot.
    // Block/leaf discrimination via is_table_entry (issue #363 P2/P4).
    if (!VMM::is_table_entry(top[l0_idx], 0)) {
        return; // 1 GiB block: not ours to clear.
    }
    auto *l1 = get_table(top, l0_idx, false);
    if (!l1) [[unlikely]] {
        note_silent_drop_once("[UNMAP-DROP] l1 ", virt_addr);
        return;
    }
    if (!VMM::is_table_entry(l1[l1_idx], 1)) {
        return; // 2 MiB block: not ours to clear.
    }
    auto *l2 = get_table(l1, l1_idx, false);
    if (!l2) [[unlikely]] {
        note_silent_drop_once("[UNMAP-DROP] l2 ", virt_addr);
        return;
    }
    l2[l2_idx] = 0;
#else
    size_t pml4_idx = arch::ArchPageTable::pml4_index(virt_addr);
    size_t pdpt_idx = arch::ArchPageTable::pdpt_index(virt_addr);
    size_t pd_idx = arch::ArchPageTable::pd_index(virt_addr);
    size_t pt_idx = arch::ArchPageTable::pt_index(virt_addr);
    // Issue #197: mirror unmap_page — flag low-VA surgery for the
    // PD_IDENTITY restore (see map_page_in_pml4 note above).
    arm_test_touch_flags(false, pml4_idx < arch::PML4_USER_COUNT);
    auto *pdpt = get_table(top, pml4_idx, false);
    if (!pdpt) [[unlikely]] {
        note_silent_drop_once("[UNMAP-DROP] pdpt ", virt_addr);
        return;
    }
    auto *pd = get_table(pdpt, pdpt_idx, false);
    if (!pd) [[unlikely]] {
        note_silent_drop_once("[UNMAP-DROP] pd ", virt_addr);
        return;
    }
    if (!is_table_entry(pd[pd_idx], 2)) {
        return; // Block/huge entry: not ours to clear.
    }
    auto *pt = get_table(pd, pd_idx, false);
    if (!pt) [[unlikely]] {
        note_silent_drop_once("[UNMAP-DROP] pt ", virt_addr);
        return;
    }
    pt[pt_idx] = 0;
#endif
    arch::ArchPageTable::tlb_flush(virt_addr);
}


} // namespace kernel
