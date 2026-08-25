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
#include <kernel/memory/pmm.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/io.hpp>
#include <constants.hpp>
#include <kernel/arch/page_table.hpp>
#include <assert.hpp>
#include <kernel/memory/vmm_errors.hpp>
#include <kernel/arch/qemu_debugcon.hpp>

namespace kernel {

constinit uint64_t VMM::kernel_pml4_ = 0;
bool VMM::hhdm_modified_ = false;
bool VMM::identity_modified_ = false;

/// @brief Initialise the VMM: capture current PML4, zero residual bootloader
/// entries.
void VMM::init() {
    kernel_pml4_ = arch::read_cr3();

#if defined(CONFIG_ARCH_X86_64)
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (kernel_pml4_ & ~0xFFFULL));

    // Zero unused entries in all boot-constructed page tables so the page
    // table walker never follows uninitialised garbage pointers.  The boot
    // code only sets entry 0 in the PDPTs and entries 0-63 in the PDs;
    // the rest may contain residual GRUB/BIOS data with PAGE_PRESENT set.

    // Zero PDPT_IDENTITY[1-511]
    {
        uint64_t pdpt_phys = pml4[0] & ~0xFFFULL;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pdpt_ident =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pdpt_phys);
        for (size_t i = 1; i < PAGE_TABLE_ENTRIES; ++i)
            pdpt_ident[i] = 0;
    }

    // Zero PDPT_HIGHER[1-511]
    {
        uint64_t pdpt_phys = pml4[256] & ~0xFFFULL;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pdpt_higher =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pdpt_phys);
        for (size_t i = 1; i < PAGE_TABLE_ENTRIES; ++i)
            pdpt_higher[i] = 0;
    }

    // Zero PD_IDENTITY[64-511] (entries 0-63 are valid huge pages)
    {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pdpt_ident_p = reinterpret_cast<uint64_t *>(
            arch::HHDM_OFFSET + (pml4[0] & ~0xFFFULL));
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pd_ident = reinterpret_cast<uint64_t *>(
            arch::HHDM_OFFSET + (pdpt_ident_p[0] & ~0xFFFULL));
        for (size_t i = 64; i < PAGE_TABLE_ENTRIES; ++i)
            pd_ident[i] = 0;
    }

    // Zero PD_HIGHER[64-511] (entries 0-63 are valid huge pages)
    {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pdpt_higher_p = reinterpret_cast<uint64_t *>(
            arch::HHDM_OFFSET + (pml4[256] & ~0xFFFULL));
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pd_higher = reinterpret_cast<uint64_t *>(
            arch::HHDM_OFFSET + (pdpt_higher_p[0] & ~0xFFFULL));
        for (size_t i = 64; i < PAGE_TABLE_ENTRIES; ++i)
            pd_higher[i] = 0;
    }

    // Zero unused PML4 entries [1-255, 257-511] so get_table never follows
    // residual UEFI/GRUB data left by the bootloader.  The boot code only
    // sets PML4[0] and PML4[256]; the rest may contain garbage.
    for (size_t i = 1; i < PAGE_TABLE_ENTRIES; ++i) {
        if (i == 256)
            continue;
        pml4[i] = 0;
    }
#elif defined(CONFIG_ARCH_AARCH64)
    // Boot.S already set up page tables.  Identity map uses 2048 L2 entries
    // across 4 tables (0-4GB).  Higher half has 2 L2 entries for kernel
    // (0xFFFF800040000000-0xFFFF800040400000).  VMM zeroing below is
    // x86_64-specific (PDPT/PD structure mismatch) and would corrupt the
    // identity L1[1] entry needed for kernel code at PA 0x40080000.
#elif defined(CONFIG_ARCH_RISCV64)
    // Boot.S already set up Sv39 page tables with HHDM mapping.
    // Kernel maps PA 0x80200000+ to VA 0xFFFFFFC080200000+ via 2MB pages.
    // No additional zeroing needed.
#endif
}

/// @brief Walk or create a page-table entry at the current level.
/// @param table     Pointer to the current-level page table.
/// @param index     Entry index within @p table.
/// @param create    If true, allocate a new table when missing.
/// @param user_alloc If true, allocate USER-owned pages for the new table.
/// @return Pointer to the next-level table, or nullptr if not present and
/// !create.
uint64_t *VMM::get_table(uint64_t *table, size_t index, bool create,
                         bool user_alloc) {
    if (table[index] & PAGE_PRESENT) {
        uint64_t target_phys = table[index] & ~0xFFFULL;
        if (!PMM::is_allocated(target_phys)) {
            table[index] = 0;
        } else if (
#if defined(CONFIG_ARCH_AARCH64)
            (table[index] & (PAGE_PRESENT | PAGE_TABLE)) == PAGE_PRESENT
#elif defined(CONFIG_ARCH_RISCV64)
            // For RISC-V, table entry has V=1, R=W=X=0
            (table[index] & (PAGE_PRESENT | PAGE_READ | PAGE_WRITE |
                             PAGE_EXEC)) == PAGE_PRESENT
#else
            table[index] & PAGE_HUGE
#endif
        ) {
            if (!create)
                return nullptr;
            uint64_t new_page = PMM::alloc_page_table();
            if (!new_page)
                return nullptr;
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *new_table =
                reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + new_page);
            uint64_t huge_base = table[index] & ~0x1FFFFFULL;
#if defined(CONFIG_ARCH_AARCH64)
            uint64_t base_flags =
                table[index] & (PAGE_PRESENT | PAGE_AF | PAGE_UXN | PAGE_PXN);
            for (size_t i = 0; i < 512; ++i) {
                new_table[i] =
                    (huge_base + i * 0x1000) | base_flags | PAGE_TABLE;
            }
            table[index] = new_page | PAGE_PRESENT | PAGE_TABLE;
#elif defined(CONFIG_ARCH_RISCV64)
            // For RISC-V, 2MB block entry has V=1, R=1, W=1, X=1 (leaf)
            // Need to split into 512 4KB entries
            uint64_t base_flags =
                table[index] &
                (PAGE_PRESENT | PAGE_READ | PAGE_WRITE | PAGE_EXEC | PAGE_USER |
                 PAGE_GLOBAL | PAGE_ACCESSED | PAGE_DIRTY);
            for (size_t i = 0; i < 512; ++i) {
                new_table[i] = (huge_base + i * 0x1000) | base_flags |
                               PAGE_ACCESSED | PAGE_DIRTY;
            }
            // Table entry: V=1, no RWX = points to next level table
            table[index] = new_page | PAGE_PRESENT;
#else
            uint64_t base_flags =
                table[index] & (PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
            for (size_t i = 0; i < 512; ++i) {
                new_table[i] = (huge_base + i * 0x1000) | base_flags;
            }
            table[index] = new_page | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
#endif
            return new_table;
        } else {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            return reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                (table[index] & ~0xFFFULL));
        }
    }
    if (!create)
        return nullptr;

    uint64_t new_page =
        user_alloc ? PMM::alloc_user_page() : PMM::alloc_page_table();
    if (!new_page)
        return nullptr;

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *new_table =
        reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + new_page);
    for (size_t i = 0; i < PAGE_TABLE_ENTRIES; ++i) {
        new_table[i] = 0;
    }

#if defined(CONFIG_ARCH_AARCH64)
    table[index] = new_page | PAGE_PRESENT | PAGE_TABLE;
#elif defined(CONFIG_ARCH_RISCV64)
    // Table entry: V=1, R=W=X=0 points to next level
    table[index] = new_page | PAGE_PRESENT;
#else
    table[index] = new_page | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
#endif

    return new_table;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
/// @brief Map a 4 KiB page in the kernel page table.
/// @param virt_addr Page-aligned virtual address.
/// @param phys_addr Page-aligned physical address.
/// @param user      If true, mark the page as user-accessible.
void VMM::map_page(uint64_t virt_addr, uint64_t phys_addr, bool user) {
#if defined(CONFIG_ARCH_RISCV64)
    // Sv39 3-level page table walk
    auto *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (kernel_pml4_ & ~0xFFFULL));

    size_t l0_idx = (virt_addr & VMM::L0_MASK) >> VMM::L0_SHIFT;
    size_t l1_idx = (virt_addr & VMM::L1_MASK) >> VMM::L1_SHIFT;
    size_t l2_idx = (virt_addr & VMM::L2_MASK) >> VMM::L2_SHIFT;

    auto *l1 = get_table(l0, l0_idx, true);
    if (!l1)
        return;

    // If L1 entry is a 2MB block, split it into 512 4KB entries
    if ((l1[l1_idx] & PAGE_PRESENT) &&
        (l1[l1_idx] & (PAGE_READ | PAGE_WRITE | PAGE_EXEC))) {
        uint64_t new_l2_phys = PMM::alloc_page_table();
        if (!new_l2_phys)
            return;
        auto *new_l2 =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + new_l2_phys);
        uint64_t block_base = l1[l1_idx] & ~0x1FFFFFULL;
        uint64_t base_flags =
            l1[l1_idx] & (PAGE_PRESENT | PAGE_READ | PAGE_WRITE | PAGE_EXEC |
                          PAGE_USER | PAGE_GLOBAL | PAGE_ACCESSED | PAGE_DIRTY);
        for (size_t i = 0; i < 512; ++i) {
            new_l2[i] = (block_base + i * 0x1000) | base_flags | PAGE_ACCESSED |
                        PAGE_DIRTY;
        }
        l1[l1_idx] = new_l2_phys | PAGE_PRESENT;
    }

    auto *l2 = get_table(l1, l1_idx, true);
    if (!l2)
        return;

    if (user)
        ENSURE(PMM::is_user_page(phys_addr) &&
               "map_page: KERNEL page mapped as user-accessible");

    uint64_t flags = PAGE_PRESENT | PAGE_READ | PAGE_WRITE | PAGE_EXEC;
    if (user)
        flags |= PAGE_USER;
    flags |= PAGE_ACCESSED | PAGE_DIRTY;

    l2[l2_idx] = phys_addr | flags;
    arch::ArchPageTable::tlb_flush(virt_addr);
#else
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (kernel_pml4_ & ~0xFFFULL));

    size_t pml4_idx = arch::ArchPageTable::pml4_index(virt_addr);

    // Allow kernel-space VAs when tests are active: PD save/restore in the
    // snapshot mechanism undoes any huge-page splits.  Boot-time calls (APIC
    // MMIO mapping, etc.) are still permitted as they run before snapshot.
    if (Scheduler::is_test_active() && pml4_idx >= arch::PML4_USER_COUNT) {
        Logger::warn("map_page: test modifying kernel-space VA 0x%lx "
                     "(pml4_idx=%zu) — PD restore will clean up",
                     virt_addr, pml4_idx);
        hhdm_modified_ = true;
    }
    // Low identity-map VAs (pml4_idx 0, PD_IDENTITY phys 0x3000): a map_page
    // here splits a boot 2 MiB identity huge entry into a PT page.  Flag it so
    // snapshot_restore restores PD_IDENTITY (the HHDM-PD gate only covers
    // pml4_idx >= PML4_USER_COUNT).
    if (Scheduler::is_test_active() && pml4_idx < arch::PML4_USER_COUNT) {
        identity_modified_ = true;
    }

    size_t pdpt_idx = arch::ArchPageTable::pdpt_index(virt_addr);
    size_t pd_idx = arch::ArchPageTable::pd_index(virt_addr);
    size_t pt_idx = arch::ArchPageTable::pt_index(virt_addr);

    auto *pdpt = get_table(pml4, pml4_idx, true);
    if (!pdpt)
        return;
    auto *pd = get_table(pdpt, pdpt_idx, true);
    if (!pd)
        return;

    // If the PD entry is a 2MB huge page, split it into 512 4KB entries
    // so we can map an individual 4KB page within it.
#if defined(CONFIG_ARCH_AARCH64)
    if ((pd[pd_idx] & (PAGE_PRESENT | PAGE_TABLE)) == PAGE_PRESENT)
#else
    if (pd[pd_idx] & PAGE_HUGE)
#endif
    {
        uint64_t new_pt_phys = PMM::alloc_page_table();
        if (!new_pt_phys)
            return;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *new_pt =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + new_pt_phys);
        uint64_t huge_base = pd[pd_idx] & ~0x1FFFFFULL;
#if defined(CONFIG_ARCH_AARCH64)
        uint64_t base_flags =
            pd[pd_idx] & (PAGE_PRESENT | PAGE_AF | PAGE_UXN | PAGE_PXN);
#else
        uint64_t base_flags =
            pd[pd_idx] & (PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
#endif
        for (size_t i = 0; i < 512; ++i) {
            new_pt[i] = (huge_base + i * 0x1000) | base_flags;
        }
#if defined(CONFIG_ARCH_AARCH64)
        pd[pd_idx] = new_pt_phys | PAGE_PRESENT | PAGE_TABLE | PAGE_AF;
#else
        pd[pd_idx] = new_pt_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
#endif
    }

    auto *pt = get_table(pd, pd_idx, true);
    if (!pt)
        return;

    if (user)
        ENSURE(PMM::is_user_page(phys_addr) &&
               "map_page: KERNEL page mapped as user-accessible");

#if defined(CONFIG_ARCH_AARCH64)
    uint64_t flags = PAGE_PRESENT | PAGE_TABLE | PAGE_AF;
    if (user)
        flags |= PAGE_AP_USER | PAGE_PXN;
    else
        flags |= PAGE_UXN | PAGE_PXN;
#else
    uint64_t flags = PAGE_PRESENT | PAGE_WRITE;
    if (user)
        flags |= PAGE_USER;
#endif

    pt[pt_idx] = phys_addr | flags;
    arch::ArchPageTable::tlb_flush(virt_addr);
#endif
}

/// @brief Unmap a virtual page from the kernel page table.
/// @param virt_addr Virtual address to unmap.
void VMM::unmap_page(uint64_t virt_addr) {
#if defined(CONFIG_ARCH_RISCV64)
    auto *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (kernel_pml4_ & ~0xFFFULL));

    size_t l0_idx = (virt_addr & VMM::L0_MASK) >> VMM::L0_SHIFT;
    size_t l1_idx = (virt_addr & VMM::L1_MASK) >> VMM::L1_SHIFT;
    size_t l2_idx = (virt_addr & VMM::L2_MASK) >> VMM::L2_SHIFT;

    auto *l1 = get_table(l0, l0_idx, false);
    if (!l1)
        return;
    auto *l2 = get_table(l1, l1_idx, false);
    if (!l2)
        return;

    l2[l2_idx] = 0;
    arch::ArchPageTable::tlb_flush(virt_addr);
#else
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (kernel_pml4_ & ~0xFFFULL));

    size_t pml4_idx = arch::ArchPageTable::pml4_index(virt_addr);
    if (Scheduler::is_test_active() && pml4_idx >= arch::PML4_USER_COUNT) {
        Logger::warn("unmap_page: test unmapping kernel-space VA 0x%lx "
                     "(pml4_idx=%zu)",
                     virt_addr, pml4_idx);
    }
    // Low identity-map VAs: flag for PD_IDENTITY restore (see map_page).
    if (Scheduler::is_test_active() && pml4_idx < arch::PML4_USER_COUNT) {
        identity_modified_ = true;
    }

    size_t pdpt_idx = arch::ArchPageTable::pdpt_index(virt_addr);
    size_t pd_idx = arch::ArchPageTable::pd_index(virt_addr);
    size_t pt_idx = arch::ArchPageTable::pt_index(virt_addr);

    auto *pdpt = get_table(pml4, pml4_idx, false);
    if (!pdpt)
        return;
    auto *pd = get_table(pdpt, pdpt_idx, false);
    if (!pd)
        return;
    auto *pt = get_table(pd, pd_idx, false);
    if (!pt)
        return;

    pt[pt_idx] = 0;
    arch::ArchPageTable::tlb_flush(virt_addr);
#endif
}

/// @brief Translate a virtual address to a physical address via the kernel
/// PML4.
/// @param virt_addr Virtual address.
/// @return Physical address, or 0 if not mapped.
uint64_t VMM::virt_to_phys(uint64_t virt_addr) {
#if defined(CONFIG_ARCH_RISCV64)
    auto *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (kernel_pml4_ & ~0xFFFULL));

    size_t l0_idx = (virt_addr & VMM::L0_MASK) >> VMM::L0_SHIFT;
    size_t l1_idx = (virt_addr & VMM::L1_MASK) >> VMM::L1_SHIFT;
    size_t l2_idx = (virt_addr & VMM::L2_MASK) >> VMM::L2_SHIFT;

    auto *l1 = get_table(l0, l0_idx, false);
    if (!l1)
        return 0;

    // Check for 2MB block mapping (leaf at L1 level)
    if ((l1[l1_idx] & PAGE_PRESENT) &&
        (l1[l1_idx] & (PAGE_READ | PAGE_WRITE | PAGE_EXEC))) {
        return (l1[l1_idx] & PAGE_HUGE_FRAME_MASK) + (virt_addr & 0x1FFFFF);
    }

    auto *l2 = get_table(l1, l1_idx, false);
    if (!l2 || !(l2[l2_idx] & PAGE_PRESENT))
        return 0;

    return (l2[l2_idx] & PAGE_FRAME_MASK) + (virt_addr & 0xFFF);
#else
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (kernel_pml4_ & ~0xFFFULL));

    size_t pml4_idx = arch::ArchPageTable::pml4_index(virt_addr);
    if (Scheduler::is_test_active() && pml4_idx >= arch::PML4_USER_COUNT) {
        Logger::warn("virt_to_phys: test accessing kernel-space VA 0x%lx "
                     "(pml4_idx=%zu)",
                     virt_addr, pml4_idx);
    }

    size_t pdpt_idx = arch::ArchPageTable::pdpt_index(virt_addr);
    size_t pd_idx = arch::ArchPageTable::pd_index(virt_addr);
    size_t pt_idx = arch::ArchPageTable::pt_index(virt_addr);

    auto *pdpt = get_table(pml4, pml4_idx, false);
    if (!pdpt)
        return 0;
    auto *pd = get_table(pdpt, pdpt_idx, false);
    if (!pd)
        return 0;

#if defined(CONFIG_ARCH_AARCH64)
    if ((pd[pd_idx] & (PAGE_PRESENT | PAGE_TABLE)) == PAGE_PRESENT)
#else
    if (pd[pd_idx] & PAGE_HUGE)
#endif
    {
        return (pd[pd_idx] & PAGE_HUGE_FRAME_MASK) + (virt_addr & 0x1FFFFF);
    }

    auto *pt = get_table(pd, pd_idx, false);
    if (!pt || !(pt[pt_idx] & PAGE_PRESENT))
        return 0;

    return (pt[pt_idx] & PAGE_FRAME_MASK) + (virt_addr & 0xFFF);
#endif
}

/// @brief Read the current PML4 physical address from CR3 (or equivalent).
/// @return Physical address of the active PML4.
uint64_t VMM::current_pml4() {
    return arch::read_cr3();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
/// @brief Map a page into a specific (non-kernel) PML4 page table.
/// @param virt_addr Page-aligned virtual address.
/// @param phys_addr Page-aligned physical address.
/// @param user      If true, mark the page as user-accessible.
/// @param pml4_phys Physical address of the target PML4.
void VMM::map_page_in_pml4(uint64_t virt_addr, uint64_t phys_addr, bool user,
                           uint64_t pml4_phys) {
    // VULN-H1: preserve legacy behaviour (executable) for existing callers.
    map_page_in_pml4(virt_addr, phys_addr, user, true, pml4_phys);
}

void VMM::map_page_in_pml4(uint64_t virt_addr, uint64_t phys_addr, bool user,
                           bool executable, uint64_t pml4_phys) {
#if defined(CONFIG_ARCH_RISCV64)
    // Sv39 3-level page table walk
    auto *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (pml4_phys & ~0xFFFULL));

    size_t l0_idx = (virt_addr & VMM::L0_MASK) >> VMM::L0_SHIFT;
    size_t l1_idx = (virt_addr & VMM::L1_MASK) >> VMM::L1_SHIFT;
    size_t l2_idx = (virt_addr & VMM::L2_MASK) >> VMM::L2_SHIFT;

    auto *l1 = get_table(l0, l0_idx, true, true);
    if (!l1)
        return;
    auto *l2 = get_table(l1, l1_idx, true, true);
    if (!l2)
        return;

    if (user)
        ENSURE(PMM::is_user_page(phys_addr) &&
               "map_page_in_pml4: KERNEL page mapped as user-accessible");

    uint64_t flags = PAGE_PRESENT | PAGE_READ | PAGE_WRITE;
    if (executable)
        flags |= PAGE_EXEC;
    if (user)
        flags |= PAGE_USER;
    flags |= PAGE_ACCESSED | PAGE_DIRTY;

    l2[l2_idx] = phys_addr | flags;
#else
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (pml4_phys & ~0xFFFULL));

    size_t pml4_idx = arch::ArchPageTable::pml4_index(virt_addr);
    size_t pdpt_idx = arch::ArchPageTable::pdpt_index(virt_addr);
    size_t pd_idx = arch::ArchPageTable::pd_index(virt_addr);
    size_t pt_idx = arch::ArchPageTable::pt_index(virt_addr);

    auto *pdpt = get_table(pml4, pml4_idx, true, true);
    if (!pdpt)
        return;
    auto *pd = get_table(pdpt, pdpt_idx, true, true);
    if (!pd)
        return;

#if defined(CONFIG_ARCH_AARCH64)
    if ((pd[pd_idx] & (PAGE_PRESENT | PAGE_TABLE)) == PAGE_PRESENT)
#else
    if (pd[pd_idx] & PAGE_HUGE)
#endif
    {
        uint64_t new_pt_phys = PMM::alloc_user_page();
        if (!new_pt_phys)
            return;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *new_pt =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + new_pt_phys);
        uint64_t huge_base = pd[pd_idx] & ~0x1FFFFFULL;
#if defined(CONFIG_ARCH_AARCH64)
        uint64_t base_flags =
            pd[pd_idx] & (PAGE_PRESENT | PAGE_AF | PAGE_UXN | PAGE_PXN);
        for (size_t i = 0; i < 512; ++i) {
            new_pt[i] = (huge_base + i * 0x1000) | base_flags | PAGE_TABLE;
        }
        pd[pd_idx] = new_pt_phys | PAGE_PRESENT | PAGE_TABLE;
#else
        uint64_t base_flags =
            pd[pd_idx] & (PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        for (size_t i = 0; i < 512; ++i) {
            new_pt[i] = (huge_base + i * 0x1000) | base_flags;
        }
        pd[pd_idx] = new_pt_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
#endif
    }

    auto *pt = get_table(pd, pd_idx, true, true);
    if (!pt)
        return;

    if (user)
        ENSURE(PMM::is_user_page(phys_addr) &&
               "map_page_in_pml4: KERNEL page mapped as user-accessible");

#if defined(CONFIG_ARCH_AARCH64)
    uint64_t flags = PAGE_PRESENT | PAGE_TABLE | PAGE_AF;
    if (user)
        flags |= PAGE_AP_USER | PAGE_PXN;
    else
        flags |= PAGE_UXN | PAGE_PXN;
    if (!executable)
        flags |= PAGE_UXN; // UXN — not executable at EL0
#else
    uint64_t flags = PAGE_PRESENT | PAGE_WRITE;
    if (user)
        flags |= PAGE_USER;
    if (!executable)
        flags |= (1ULL << 63); // NX — not executable
#endif

    pt[pt_idx] = phys_addr | flags;
#endif
}

/// @brief Create a new PML4: zeroes user entries, copies kernel entries.
/// @return Physical address of new PML4, or 0 on failure.
uint64_t VMM::clone_kernel_pml4() {
    uint64_t phys = PMM::alloc_page();
    if (!phys) {
        ASSERT(errors::VmmError::VMM_ERR_PML4_ALLOC);
        return 0;
    }

#if defined(CONFIG_ARCH_RISCV64)
    auto *src = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                             (kernel_pml4_ & ~0xFFFULL));
    auto *dst = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + phys);

    // Clear user-space entries (L0 indices 0-255 for 0-256GB)
    for (size_t i = 0; i < 256; ++i) {
        dst[i] = 0;
    }
    // Copy kernel-space entries (L0 indices 256-511 for 256GB-512GB)
    for (size_t i = 256; i < 512; ++i) {
        dst[i] = src[i];
    }
    return phys;
#else
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *src = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                             (kernel_pml4_ & ~0xFFFULL));
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *dst = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + phys);
    // Clear user-space entries (0-255) — the kernel's boot identity-map must
    // not leak into user page tables.  Fork shares user entries by copying them
    // from the PARENT's PML4, not from the kernel PML4.
    for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i) {
        dst[i] = 0;
    }
    // Copy kernel-space entries (256-511) so user tasks can access kernel
    // mappings.
    for (size_t i = arch::PML4_KERNEL_START; i < PAGE_TABLE_ENTRIES; ++i) {
        dst[i] = src[i];
    }
    return phys;
#endif
}

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
                                            (pml4_phys & ~0xFFFULL));
    // User space: L0 indices 0-255 for Sv39 (0-256GB)
    for (int l0_idx = 0; l0_idx < 256; ++l0_idx) {
        if (!(l0[l0_idx] & PAGE_PRESENT))
            continue;
        // Check for 1GB block at L0
        if ((l0[l0_idx] & (PAGE_READ | PAGE_WRITE | PAGE_EXEC))) {
            uint64_t page = l0[l0_idx] & ~0x3FFFFFFFULL;
            if (!PMM::is_user_page(page))
                continue;
            PMM::free_page(page);
            continue;
        }
        uint64_t l1_phys = l0[l0_idx] & ~0xFFFULL;
        if (!PMM::is_user_page(l1_phys))
            continue;
        auto *l1 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l1_phys);
        for (int l1_idx = 0; l1_idx < 512; ++l1_idx) {
            if (!(l1[l1_idx] & PAGE_PRESENT))
                continue;
            // Check for 2MB block at L1
            if ((l1[l1_idx] & (PAGE_READ | PAGE_WRITE | PAGE_EXEC))) {
                uint64_t page = l1[l1_idx] & ~0x1FFFFFULL;
                if (!PMM::is_user_page(page))
                    continue;
                PMM::free_page(page);
                continue;
            }
            uint64_t l2_phys = l1[l1_idx] & ~0xFFFULL;
            if (!PMM::is_user_page(l2_phys))
                continue;
            auto *l2 =
                reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l2_phys);
            for (int l2_idx = 0; l2_idx < 512; ++l2_idx) {
                if (!(l2[l2_idx] & PAGE_PRESENT))
                    continue;
                // Check for leaf PTE (V=1, R|W|X=1) — 3-level Sv39 format
                if ((l2[l2_idx] & (PAGE_READ | PAGE_WRITE | PAGE_EXEC))) {
                    uint64_t leaf = l2[l2_idx] & PAGE_FRAME_MASK;
                    if (!PMM::is_user_page(leaf))
                        continue;
                    PMM::free_page(leaf);
                    if ((l2_idx & 0x3F) == 0x3F)
                        Scheduler::cleanup_step();
                    continue;
                }
                // Table entry (V=1, R=W=X=0) — old 4-level format with L3
                // beneath
                uint64_t l3_phys = l2[l2_idx] & ~0xFFFULL;
                if (!PMM::is_user_page(l3_phys))
                    continue;
                auto *l3 =
                    reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l3_phys);
                for (int l3_idx = 0; l3_idx < 512; ++l3_idx) {
                    if (!(l3[l3_idx] & PAGE_PRESENT))
                        continue;
                    uint64_t leaf = l3[l3_idx] & PAGE_FRAME_MASK;
                    if (!PMM::is_user_page(leaf))
                        continue;
                    PMM::free_page(leaf);
                    if ((l3_idx & 0x3F) == 0x3F)
                        Scheduler::cleanup_step();
                }
                PMM::free_page(l3_phys);
                l2[l2_idx] = 0; // clear L2 entry to prevent re-walk
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
                                              (pml4_phys & ~0xFFFULL));
    // v0.3.11: bounded diagnostic — log page-table pages skipped because their
    // owner bit is KERNEL (free_user_pages only frees USER-owned table pages).
    static uint64_t s_fup_skip_log = 0;
    auto log_skip = [](const char *lvl, uint64_t phys) {
        if (__atomic_load_n(&s_fup_skip_log, __ATOMIC_RELAXED) >= 24)
            return;
        __atomic_fetch_add(&s_fup_skip_log, 1UL, __ATOMIC_RELAXED);
        arch::QemuDebugcon::write(lvl);
        char buf[24];
        int p = 0;
        buf[p++] = '0';
        buf[p++] = 'x';
        bool started = false;
        for (int sh = 60; sh >= 0; sh -= 4) {
            unsigned nib = static_cast<unsigned>((phys >> sh) & 0xF);
            if (nib || started || sh == 0) {
                buf[p++] = "0123456789abcdef"[nib];
                started = true;
            }
        }
        buf[p++] = '\n';
        arch::QemuDebugcon::write(buf, static_cast<size_t>(p));
    };
    for (int pml4_idx = 0; pml4_idx < static_cast<int>(arch::PML4_USER_COUNT);
         ++pml4_idx) {
        if (!(pml4[pml4_idx] & PAGE_PRESENT))
            continue;
        uint64_t pdpt_phys = pml4[pml4_idx] & ~0xFFFULL;
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
#if defined(CONFIG_ARCH_AARCH64)
            if ((pdpt[pdpt_idx] & (PAGE_PRESENT | PAGE_TABLE)) == PAGE_PRESENT)
#else
            if (pdpt[pdpt_idx] & PAGE_HUGE)
#endif
            {
                uint64_t page = pdpt[pdpt_idx] & ~0x3FFFFFULL;
                if (!PMM::is_user_page(page))
                    continue;
                PMM::free_page(page);
                continue;
            }
            uint64_t pd_phys = pdpt[pdpt_idx] & ~0xFFFULL;
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
#if defined(CONFIG_ARCH_AARCH64)
                if ((pd[pd_idx] & (PAGE_PRESENT | PAGE_TABLE)) == PAGE_PRESENT)
#else
                if (pd[pd_idx] & PAGE_HUGE)
#endif
                {
                    uint64_t page = pd[pd_idx] & PAGE_HUGE_FRAME_MASK;
                    if (!PMM::is_user_page(page))
                        continue;
                    PMM::free_page(page);
                    continue;
                }
                uint64_t pt_phys = pd[pd_idx] & ~0xFFFULL;
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
                    uint64_t leaf = pt[pt_idx] & PAGE_FRAME_MASK;
                    if (!PMM::is_user_page(leaf))
                        continue;
                    PMM::free_page(leaf);
                    if ((pt_idx & 0x3F) == 0x3F)
                        Scheduler::cleanup_step();
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
                                             (src_pml4 & ~0xFFFULL));
    auto *dst = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                             (dst_pml4 & ~0xFFFULL));

    for (int pml4_idx = 0; pml4_idx < static_cast<int>(arch::PML4_USER_COUNT);
         ++pml4_idx) {
        if (!(src[pml4_idx] & PAGE_PRESENT)) {
            dst[pml4_idx] = 0;
            continue;
        }

        uint64_t src_pdpt_phys = src[pml4_idx] & ~0xFFFULL;
        uint64_t dst_pdpt_phys = PMM::alloc_user_page();
        if (!dst_pdpt_phys)
            return false;
        auto *dst_pdpt =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + dst_pdpt_phys);
        __builtin_memset(dst_pdpt, 0, 4096);
        dst[pml4_idx] = dst_pdpt_phys | VMM::PAGE_PRESENT | VMM::PAGE_WRITE |
                        VMM::PAGE_USER;

        auto *src_pdpt =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + src_pdpt_phys);
        for (int pdpt_idx = 0; pdpt_idx < 512; ++pdpt_idx) {
            if (!(src_pdpt[pdpt_idx] & PAGE_PRESENT))
                continue;

#if defined(CONFIG_ARCH_AARCH64)
            if ((src_pdpt[pdpt_idx] & (PAGE_PRESENT | PAGE_TABLE)) ==
                PAGE_PRESENT)
#else
            if (src_pdpt[pdpt_idx] & VMM::PAGE_HUGE)
#endif
            {
                // 1 GiB huge page — leave as-is in child too.
                dst_pdpt[pdpt_idx] = src_pdpt[pdpt_idx];
                continue;
            }

            uint64_t src_pd_phys = src_pdpt[pdpt_idx] & ~0xFFFULL;
            uint64_t dst_pd_phys = PMM::alloc_user_page();
            if (!dst_pd_phys)
                return false;
            auto *dst_pd =
                reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + dst_pd_phys);
            __builtin_memset(dst_pd, 0, 4096);
            dst_pdpt[pdpt_idx] = dst_pd_phys | VMM::PAGE_PRESENT |
                                 VMM::PAGE_WRITE | VMM::PAGE_USER;

            auto *src_pd =
                reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + src_pd_phys);
            for (int pd_idx = 0; pd_idx < 512; ++pd_idx) {
                if (!(src_pd[pd_idx] & PAGE_PRESENT))
                    continue;

#if defined(CONFIG_ARCH_AARCH64)
                if ((src_pd[pd_idx] & (PAGE_PRESENT | PAGE_TABLE)) ==
                    PAGE_PRESENT)
#else
                if (src_pd[pd_idx] & VMM::PAGE_HUGE)
#endif
                {
                    // 2 MiB huge page — copy as-is.
                    dst_pd[pd_idx] = src_pd[pd_idx];
                    continue;
                }

                uint64_t src_pt_phys = src_pd[pd_idx] & ~0xFFFULL;
                uint64_t dst_pt_phys = PMM::alloc_user_page();
                if (!dst_pt_phys)
                    return false;
                auto *dst_pt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                            dst_pt_phys);
                __builtin_memset(dst_pt, 0, 4096);
                dst_pd[pd_idx] = dst_pt_phys | VMM::PAGE_PRESENT |
                                 VMM::PAGE_WRITE | VMM::PAGE_USER;

                auto *src_pt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                            src_pt_phys);
                for (int pt_idx = 0; pt_idx < 512; ++pt_idx) {
                    if (!(src_pt[pt_idx] & PAGE_PRESENT))
                        continue;

                    uint64_t src_data = src_pt[pt_idx] & PAGE_FRAME_MASK;
                    uint64_t flags = src_pt[pt_idx] & ~PAGE_FRAME_MASK;

                    uint64_t dst_data = PMM::alloc_user_page();
                    if (!dst_data)
                        return false;
                    __builtin_memcpy(
                        reinterpret_cast<void *>(arch::HHDM_OFFSET + dst_data),
                        reinterpret_cast<void *>(arch::HHDM_OFFSET + src_data),
                        4096);
                    dst_pt[pt_idx] = dst_data | flags;
                    if ((pt_idx & 0x3F) == 0x3F)
                        Scheduler::cleanup_step();
                }
            }
        }
    }
    return true;
#elif defined(CONFIG_ARCH_RISCV64)
    // RISC-V Sv39: L0 → L1 → L2 → leaf
    auto *src = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                             (src_pml4 & ~0xFFFULL));
    auto *dst = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                             (dst_pml4 & ~0xFFFULL));

    for (int l0_idx = 0; l0_idx < 256; ++l0_idx) {
        if (!(src[l0_idx] & PAGE_PRESENT)) {
            dst[l0_idx] = 0;
            continue;
        }

        if ((src[l0_idx] & (PAGE_READ | PAGE_WRITE | PAGE_EXEC))) {
            // 1 GiB block — copy as-is.
            dst[l0_idx] = src[l0_idx];
            continue;
        }

        uint64_t src_l1_phys = src[l0_idx] & ~0xFFFULL;
        uint64_t dst_l1_phys = PMM::alloc_user_page();
        if (!dst_l1_phys)
            return false;
        auto *dst_l1 =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + dst_l1_phys);
        __builtin_memset(dst_l1, 0, 4096);
        dst[l0_idx] = dst_l1_phys | VMM::PAGE_PRESENT;

        auto *src_l1 =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + src_l1_phys);
        for (int l1_idx = 0; l1_idx < 512; ++l1_idx) {
            if (!(src_l1[l1_idx] & PAGE_PRESENT))
                continue;

            if ((src_l1[l1_idx] & (PAGE_READ | PAGE_WRITE | PAGE_EXEC))) {
                // 2 MiB block — copy as-is.
                dst_l1[l1_idx] = src_l1[l1_idx];
                continue;
            }

            uint64_t src_l2_phys = src_l1[l1_idx] & ~0xFFFULL;
            uint64_t dst_l2_phys = PMM::alloc_user_page();
            if (!dst_l2_phys)
                return false;
            auto *dst_l2 =
                reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + dst_l2_phys);
            __builtin_memset(dst_l2, 0, 4096);
            dst_l1[l1_idx] = dst_l2_phys | VMM::PAGE_PRESENT;

            auto *src_l2 =
                reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + src_l2_phys);
            for (int l2_idx = 0; l2_idx < 512; ++l2_idx) {
                if (!(src_l2[l2_idx] & PAGE_PRESENT))
                    continue;

                // RISC-V leaf entries have V=1, R|W|X=1
                if ((src_l2[l2_idx] & (PAGE_READ | PAGE_WRITE | PAGE_EXEC))) {
                    uint64_t src_data = src_l2[l2_idx] & ~0xFFFULL;
                    uint64_t flags = src_l2[l2_idx] & 0xFFFULL;
                    uint64_t dst_data = PMM::alloc_user_page();
                    if (!dst_data)
                        return false;
                    __builtin_memcpy(
                        reinterpret_cast<void *>(arch::HHDM_OFFSET + dst_data),
                        reinterpret_cast<void *>(arch::HHDM_OFFSET + src_data),
                        4096);
                    dst_l2[l2_idx] = dst_data | flags;
                } else {
                    // Table entry: 4-level format with L3 beneath
                    uint64_t src_l3_phys = src_l2[l2_idx] & ~0xFFFULL;
                    uint64_t dst_l3_phys = PMM::alloc_user_page();
                    if (!dst_l3_phys)
                        return false;
                    auto *dst_l3 = reinterpret_cast<uint64_t *>(
                        arch::HHDM_OFFSET + dst_l3_phys);
                    __builtin_memset(dst_l3, 0, 4096);
                    dst_l2[l2_idx] = dst_l3_phys | VMM::PAGE_PRESENT;

                    auto *src_l3 = reinterpret_cast<uint64_t *>(
                        arch::HHDM_OFFSET + src_l3_phys);
                    for (int l3_idx = 0; l3_idx < 512; ++l3_idx) {
                        if (!(src_l3[l3_idx] & PAGE_PRESENT))
                            continue;
                        uint64_t src_data = src_l3[l3_idx] & ~0xFFFULL;
                        uint64_t flags = src_l3[l3_idx] & 0xFFFULL;
                        uint64_t dst_data = PMM::alloc_user_page();
                        if (!dst_data)
                            return false;
                        __builtin_memcpy(reinterpret_cast<void *>(
                                             arch::HHDM_OFFSET + dst_data),
                                         reinterpret_cast<void *>(
                                             arch::HHDM_OFFSET + src_data),
                                         4096);
                        dst_l3[l3_idx] = dst_data | flags;
                    }
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
#if defined(CONFIG_ARCH_RISCV64)
    auto *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (pml4_phys & ~0xFFFULL));

    size_t l0_idx = (virt_addr & VMM::L0_MASK) >> VMM::L0_SHIFT;
    size_t l1_idx = (virt_addr & VMM::L1_MASK) >> VMM::L1_SHIFT;
    size_t l2_idx = (virt_addr & VMM::L2_MASK) >> VMM::L2_SHIFT;

    if (!(l0[l0_idx] & PAGE_PRESENT))
        return 0;
    auto *l1 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (l0[l0_idx] & ~0xFFFULL));

    if (!(l1[l1_idx] & PAGE_PRESENT))
        return 0;
    auto *l2 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (l1[l1_idx] & ~0xFFFULL));

    // Check for 2MB block mapping (leaf at L1 level)
    if ((l2[l1_idx] & PAGE_PRESENT) &&
        (l2[l1_idx] & (PAGE_READ | PAGE_WRITE | PAGE_EXEC))) {
        return (l2[l1_idx] & PAGE_HUGE_FRAME_MASK) + (virt_addr & 0x1FFFFF);
    }

    if (!(l2[l2_idx] & PAGE_PRESENT))
        return 0;
    auto *l3 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (l2[l2_idx] & ~0xFFFULL));

    if (!(l3[l2_idx] & PAGE_PRESENT))
        return 0;
    return (l3[l2_idx] & PAGE_FRAME_MASK) + (virt_addr & 0xFFF);
#else
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (pml4_phys & ~0xFFFULL));

    size_t pml4_idx = arch::ArchPageTable::pml4_index(virt_addr);
    size_t pdpt_idx = arch::ArchPageTable::pdpt_index(virt_addr);
    size_t pd_idx = arch::ArchPageTable::pd_index(virt_addr);
    size_t pt_idx = arch::ArchPageTable::pt_index(virt_addr);

    if (!(pml4[pml4_idx] & PAGE_PRESENT))
        return 0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pdpt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (pml4[pml4_idx] & ~0xFFFULL));

    if (!(pdpt[pdpt_idx] & PAGE_PRESENT))
        return 0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (pdpt[pdpt_idx] & ~0xFFFULL));

#if defined(CONFIG_ARCH_AARCH64)
    if ((pd[pd_idx] & (PAGE_PRESENT | PAGE_TABLE)) == PAGE_PRESENT)
#else
    if (pd[pd_idx] & PAGE_HUGE)
#endif
    {
        return (pd[pd_idx] & PAGE_HUGE_FRAME_MASK) + (virt_addr & 0x1FFFFF);
    }

    if (!(pd[pd_idx] & PAGE_PRESENT))
        return 0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (pd[pd_idx] & ~0xFFFULL));

    if (!(pt[pt_idx] & PAGE_PRESENT))
        return 0;
    return (pt[pt_idx] & PAGE_FRAME_MASK) + (virt_addr & 0xFFF);
#endif
}

} // namespace kernel

// --- Error-returning overloads ---
namespace kernel {

using namespace errors;

/// @brief Initialise VMM with error-code return.
/// @return VMM_ERR_OK.
VmmError VMM::init_err() {
    init();
    return VMM_ERR_OK;
}

/// @brief Map a page in the kernel table with alignment validation and
/// error-code return.
/// @param virt_addr Page-aligned virtual address.
/// @param phys_addr Page-aligned physical address.
/// @param user      If true, mark as user-accessible.
/// @return VmmError code.
VmmError VMM::map_page_err(uint64_t virt_addr, uint64_t phys_addr, bool user) {
    if ((virt_addr & 0xFFF) != 0 || (phys_addr & 0xFFF) != 0) {
        return VMM_ERR_INVALID_ADDR;
    }
    map_page(virt_addr, phys_addr, user);
    return VMM_ERR_OK;
}

/// @brief Unmap a page with alignment validation and error-code return.
/// @param virt_addr Virtual address to unmap.
/// @return VmmError code.
VmmError VMM::unmap_page_err(uint64_t virt_addr) {
    if ((virt_addr & 0xFFF) != 0) {
        return VMM_ERR_INVALID_ADDR;
    }
    unmap_page(virt_addr);
    return VMM_ERR_OK;
}

/// @brief Translate virtual to physical via kernel PML4 with error-code return.
/// @param virt_addr Virtual address.
/// @param[out] out_phys_addr Physical address on success.
/// @return VmmError code.
VmmError VMM::virt_to_phys_err(uint64_t virt_addr, uint64_t &out_phys_addr) {
    if ((virt_addr & 0xFFF) != 0) {
        return VMM_ERR_INVALID_ADDR;
    }
    out_phys_addr = virt_to_phys(virt_addr);
    return out_phys_addr != 0 ? VMM_ERR_OK : VMM_ERR_NOT_MAPPED;
}

/// @brief Clone kernel PML4 with error-code return.
/// @param[out] out_pml4_phys Physical address of new PML4 on success.
/// @return VmmError code.
VmmError VMM::clone_kernel_pml4_err(uint64_t &out_pml4_phys) {
    uint64_t phys = clone_kernel_pml4();
    if (!phys) {
        return VMM_ERR_PML4_ALLOC;
    }
    out_pml4_phys = phys;
    return VMM_ERR_OK;
}

/// @brief Map page into a specific PML4 with alignment validation and
/// error-code return.
/// @param virt_addr Page-aligned virtual address.
/// @param phys_addr Page-aligned physical address.
/// @param user      If true, mark as user-accessible.
/// @param pml4_phys Physical address of the target PML4.
/// @return VmmError code.
VmmError VMM::map_page_in_pml4_err(uint64_t virt_addr, uint64_t phys_addr,
                                   bool user, uint64_t pml4_phys) {
    if ((virt_addr & 0xFFF) != 0 || (phys_addr & 0xFFF) != 0) {
        return VMM_ERR_INVALID_ADDR;
    }
    map_page_in_pml4(virt_addr, phys_addr, user, pml4_phys);
    return VMM_ERR_OK;
}

/// @brief Free all user pages from a PML4 with error-code return.
/// @param pml4_phys Physical address of the user PML4.
/// @return VmmError code.
VmmError VMM::free_user_pages_err(uint64_t pml4_phys) {
    free_user_pages(pml4_phys);
    return VMM_ERR_OK;
}

/// @brief Translate virtual to physical in a specific PML4 with error-code
/// return.
/// @param virt_addr Virtual address.
/// @param pml4_phys Physical address of the target PML4.
/// @param[out] out_phys_addr Physical address on success.
/// @return VmmError code.
VmmError VMM::virt_to_phys_in_pml4_err(uint64_t virt_addr, uint64_t pml4_phys,
                                       uint64_t &out_phys_addr) {
    if ((virt_addr & 0xFFF) != 0) {
        return VMM_ERR_INVALID_ADDR;
    }
    out_phys_addr = virt_to_phys_in_pml4(virt_addr, pml4_phys);
    return out_phys_addr != 0 ? VMM_ERR_OK : VMM_ERR_NOT_MAPPED;
}

/// @brief Maps every frame owned by a FrameCap into @p pml4_phys.
///        Refuses a revoked cap — capability-gated memory must not map after
///        revocation (ROADMAP 0.4.1 CSpace).  The caller must hold a pin
///        (ScopedRef / live capability slot) on @p fc.
bool VMM::map_frame_from_cap(cap::FrameCap *fc, uint64_t virt_addr, bool user,
                             uint64_t pml4_phys) {
    if (!fc || fc->revoked())
        return false;
    if (fc->phys == 0 || fc->count == 0)
        return false;
    for (size_t i = 0; i < fc->count; ++i) {
        map_page_in_pml4(virt_addr + i * arch::PAGE_SIZE,
                         fc->phys + i * arch::PAGE_SIZE, user, pml4_phys);
    }
    return true;
}

/// @brief Unmaps one page previously mapped via map_frame_from_cap().
void VMM::unmap_frame_from_cap(uint64_t virt_addr, uint64_t pml4_phys) {
    map_page_in_pml4(virt_addr, 0, false, pml4_phys);
}

} // namespace kernel
