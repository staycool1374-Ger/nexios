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

/// @file page_table_impl.hpp
/// @brief AArch64 4-level page table walk, map, unmap, and TLB management.

#pragma once

#include <types.hpp>
#include <kernel/arch/hal/io.hpp>
#include <kernel/arch/hal/page_flags.hpp>
#include <lib/error.hpp>
#include <lib/string.hpp>
#include <kernel/memory/pmm.hpp>

namespace arch {

/// @brief Read the current kernel page table base (TTBR1_EL1).
/// @return Physical address of the kernel page table.
inline uint64_t arch_page_table_current() {
    return read_ttbr1_el1();
}
/// @brief Activate a kernel page table by writing TTBR1_EL1.
/// @param[in] ttbr1_phys Physical address of the new kernel page table.
inline void arch_page_table_activate(uint64_t ttbr1_phys) {
    write_ttbr1_el1(ttbr1_phys);
    dsb_sy();
    isb();
}
/// @brief Flush TLB for a single virtual address (EL1).
/// @param[in] virt_addr Virtual address to invalidate.
inline void arch_page_table_tlb_flush(uint64_t virt_addr) {
    tlbi_vae1(virt_addr);
    dsb_sy();
    isb();
}
/// @brief Flush entire TLB (EL1).
inline void arch_page_table_tlb_flush_all() {
    tlbi_alle1();
    dsb_sy();
    isb();
}

/// @brief AArch64 4-level page table manager (TTBR1_EL1 kernel space).
class ArchPageTable {
  public:
    /// @brief Return the current kernel page table physical address.
    static inline uint64_t current() {
        return arch_page_table_current();
    }
    /// @brief Activate a kernel page table.
    /// @param[in] phys Physical address of the page table.
    static inline void activate(uint64_t phys) {
        arch_page_table_activate(phys);
    }
    /// @brief Flush TLB for a single virtual address.
    /// @param[in] virt_addr Virtual address to flush.
    static inline void tlb_flush(uint64_t virt_addr) {
        arch_page_table_tlb_flush(virt_addr);
    }
    /// @brief Flush the entire TLB.
    static inline void tlb_flush_all() {
        arch_page_table_tlb_flush_all();
    }

    static constexpr uint64_t PAGE_SIZE = CONFIG_PAGE_SIZE;
    static constexpr uint64_t ENTRIES = 512;

    /// @brief L0 (PML4-equivalent) index for a virtual address.
    /// @param[in] vaddr Virtual address.
    /// @return Index into the L0 table (bits 47:39).
    static inline size_t pml4_index(uint64_t vaddr) {
        return (vaddr >> L0_SHIFT) & TABLE_MASK;
    }
    /// @brief L1 (PDPT-equivalent) index for a virtual address.
    /// @param[in] vaddr Virtual address.
    /// @return Index into the L1 table (bits 38:30).
    static inline size_t pdpt_index(uint64_t vaddr) {
        return (vaddr >> L1_SHIFT) & TABLE_MASK;
    }
    /// @brief L2 (PD-equivalent) index for a virtual address.
    /// @param[in] vaddr Virtual address.
    /// @return Index into the L2 table (bits 29:21).
    static inline size_t pd_index(uint64_t vaddr) {
        return (vaddr >> L2_SHIFT) & TABLE_MASK;
    }
    /// @brief L3 (PT-equivalent) index for a virtual address.
    /// @param[in] vaddr Virtual address.
    /// @return Index into the L3 table (bits 20:12).
    static inline size_t pt_index(uint64_t vaddr) {
        return (vaddr >> L3_SHIFT) & TABLE_MASK;
    }

    /// @brief Map a 4 KiB page or 2 MiB block at a given virtual address.
    /// @param[in] virt Virtual address.
    /// @param[in] phys Physical address.
    /// @param[in] flags Page attribute flags.
    /// @return Error::OOM if intermediate tables cannot be allocated, or
    /// success.
    static kernel::ErrorOr<void> map_page(uint64_t virt, uint64_t phys,
                                          uint64_t flags);
    /// @brief Unmap a page at a given virtual address.
    /// @param[in] virt Virtual address.
    /// @return Error::NOT_FOUND if no mapping exists, or success.
    static kernel::ErrorOr<void> unmap_page(uint64_t virt);
    /// @brief Resolve the physical address for a given virtual address.
    /// @param[in] virt Virtual address.
    /// @return Physical address, or 0 if not mapped.
    static uint64_t get_physical(uint64_t virt);

  private:
    static constexpr uint64_t L0_SHIFT = 39;
    static constexpr uint64_t L1_SHIFT = 30;
    static constexpr uint64_t L2_SHIFT = 21;
    static constexpr uint64_t L3_SHIFT = 12;
    static constexpr uint64_t TABLE_MASK = 0x1FF;
    static constexpr uint64_t DESC_VALID = 1ULL << 0;
    static constexpr uint64_t DESC_TABLE = 1ULL << 1;
    static constexpr uint64_t DESC_BLOCK = 0ULL;
    static constexpr uint64_t DESC_AF = 1ULL << 10;
    static constexpr uint64_t DESC_SH_INNER = 3ULL << 8;
    static constexpr uint64_t DESC_NS = 1ULL << 5;
    static constexpr uint64_t ATTR_IDX_NORMAL = 1ULL << 2;
    static constexpr uint64_t ATTR_IDX_DEVICE = 0ULL;
    static constexpr uint64_t AP_RO = 2ULL << 6;
    static constexpr uint64_t AP_RW = 0ULL;
    static constexpr uint64_t UXN = 1ULL << 54;
    static constexpr uint64_t PXN = 1ULL << 53;
    // Physical-address field: bits [47:12].  A plain ~0xFFF mask is NOT enough
    // — leaf descriptors carry UXN (bit 54) / PXN (bit 53) which sit above the
    // address bits, and ~0xFFF would leak them into extracted physical
    // addresses (breaking get_physical and free_page).
    static constexpr uint64_t DESC_PHYS_MASK = ((1ULL << 48) - 1) & ~0xFFFULL;
    static constexpr uint64_t DESC_BLOCK_MASK = ((1ULL << 48) - 1) & ~0x1FFFFFULL;

    /// @brief Get or create a page table entry at a given index.
    /// @param[in] table_base Physical address of the table.
    /// @param[in] index Entry index (0-511).
    /// @param[in] create If true and the entry is invalid, allocate a new
    /// table.
    /// @return Pointer to the next-level table, or nullptr on failure.
    static uint64_t *get_table(uint64_t table_base, uint64_t index,
                               bool create);
    /// @brief Convert PageFlags into AArch64 page descriptor attributes.
    /// @param[in] flags PageFlags bitmask.
    /// @return AArch64 page descriptor bits.
    static uint64_t attr_from_flags(uint64_t flags);
    /// @brief Walk the L0 table to get or create the L1 table.
    static uint64_t *walk_l0(uint64_t l0_base, size_t l0_idx, bool create);
    /// @brief Walk the L1 table to get or create the L2 table.
    static uint64_t *walk_l1(uint64_t l1_base, size_t l1_idx, bool create);
    /// @brief Walk the L2 table to get or create the L3 table.
    static uint64_t *walk_l2(uint64_t l2_base, size_t l2_idx, bool create);
    /// @brief Walk the L3 table to get or create the final page entry.
    static uint64_t *walk_l3(uint64_t l3_base, size_t l3_idx, bool create);
};

inline uint64_t *ArchPageTable::get_table(uint64_t table_base, uint64_t index,
                                          bool create) {
    // Page-table pages are physical; access them through the HHDM direct-map
    // alias so the walk is independent of the currently-active TTBR0 (which
    // may be a user/private PML4 without the identity map).
    uint64_t *table = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                   table_base);
    uint64_t entry = table[index];

    if (entry & DESC_VALID) {
        if (entry & DESC_TABLE) {
            return reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                (entry & DESC_PHYS_MASK));
        }
        return nullptr;
    }

    if (!create)
        return nullptr;

    uint64_t new_page = kernel::PMM::alloc_page_table();
    if (!new_page)
        return nullptr;

    void *new_page_ptr =
        reinterpret_cast<void *>(arch::HHDM_OFFSET + new_page);
    memset(new_page_ptr, 0, PAGE_SIZE);
    table[index] = new_page | DESC_VALID | DESC_TABLE;
    return reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + new_page);
}

inline uint64_t ArchPageTable::attr_from_flags(uint64_t flags) {
    uint64_t attr = DESC_VALID | DESC_AF | DESC_SH_INNER | DESC_NS;
    if (flags & kernel::PageFlags::PRESENT)
        attr |= DESC_VALID;
    if (flags & kernel::PageFlags::WRITE)
        attr |= AP_RW;
    else
        attr |= AP_RO;
    if (flags & kernel::PageFlags::USER) {
        // User accessible; PXN (SMEP parity — EL1 must not execute user
        // pages).  UXN left clear so EL0 may execute.
        attr |= PXN;
    } else {
        attr |= UXN | PXN;
    }
    if (flags & kernel::PageFlags::NX)
        attr |= UXN | PXN;
    if (flags & kernel::PageFlags::CACHE_DISABLED)
        attr |= ATTR_IDX_DEVICE;
    else
        attr |= ATTR_IDX_NORMAL;
    return attr;
}

inline uint64_t *ArchPageTable::walk_l0(uint64_t l0_base, size_t l0_idx,
                                        bool create) {
    return get_table(l0_base, l0_idx, create);
}

inline uint64_t *ArchPageTable::walk_l1(uint64_t l1_base, size_t l1_idx,
                                        bool create) {
    return get_table(l1_base, l1_idx, create);
}

inline uint64_t *ArchPageTable::walk_l2(uint64_t l2_base, size_t l2_idx,
                                        bool create) {
    return get_table(l2_base, l2_idx, create);
}

inline uint64_t *ArchPageTable::walk_l3(uint64_t l3_base, size_t l3_idx,
                                        bool create) {
    return get_table(l3_base, l3_idx, create);
}

inline kernel::ErrorOr<void>
ArchPageTable::map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t l0_idx = (virt >> L0_SHIFT) & TABLE_MASK;
    uint64_t l1_idx = (virt >> L1_SHIFT) & TABLE_MASK;
    uint64_t l2_idx = (virt >> L2_SHIFT) & TABLE_MASK;
    uint64_t l3_idx = (virt >> L3_SHIFT) & TABLE_MASK;

    uint64_t l0_phys = current();
    uint64_t *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l0_phys);

    uint64_t *l1 = walk_l0(l0_phys, l0_idx, true);
    if (!l1)
        return kernel::Error::OOM;
    uint64_t l1_phys = l0[l0_idx] & DESC_PHYS_MASK;

    uint64_t *l2 = walk_l1(l1_phys, l1_idx, true);
    if (!l2)
        return kernel::Error::OOM;
    uint64_t l2_phys = l1[l1_idx] & DESC_PHYS_MASK;

    uint64_t entry_flags = attr_from_flags(flags);

    if ((virt & ((1ULL << L2_SHIFT) - 1)) == 0 &&
        (phys & ((1ULL << L2_SHIFT) - 1)) == 0) {
        l2[l2_idx] = phys | entry_flags | DESC_BLOCK | (1ULL << 10);
        dsb_sy();
        isb();
        return {};
    }

    uint64_t *l3 = walk_l2(l2_phys, l2_idx, true);
    if (!l3)
        return kernel::Error::OOM;

    l3[l3_idx] = phys | entry_flags | DESC_TABLE | (1ULL << 10);
    dsb_sy();
    isb();
    return {};
}

inline kernel::ErrorOr<void> ArchPageTable::unmap_page(uint64_t virt) {
    uint64_t l0_idx = (virt >> L0_SHIFT) & TABLE_MASK;
    uint64_t l1_idx = (virt >> L1_SHIFT) & TABLE_MASK;
    uint64_t l2_idx = (virt >> L2_SHIFT) & TABLE_MASK;
    uint64_t l3_idx = (virt >> L3_SHIFT) & TABLE_MASK;

    uint64_t l0_phys = current();
    uint64_t *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l0_phys);

    if (!(l0[l0_idx] & DESC_VALID))
        return kernel::Error::NOT_FOUND;
    uint64_t *l1 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                (l0[l0_idx] & DESC_PHYS_MASK));
    if (!(l1[l1_idx] & DESC_VALID))
        return kernel::Error::NOT_FOUND;
    uint64_t l2_phys = l1[l1_idx] & DESC_PHYS_MASK;
    uint64_t *l2 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l2_phys);

    if ((virt & ((1ULL << L2_SHIFT) - 1)) == 0 && (l2[l2_idx] & DESC_BLOCK)) {
        l2[l2_idx] = 0;
        dsb_sy();
        isb();
        return {};
    }

    if (!(l2[l2_idx] & DESC_VALID))
        return kernel::Error::NOT_FOUND;
    uint64_t l3_phys = l2[l2_idx] & DESC_PHYS_MASK;
    uint64_t *l3 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l3_phys);

    if (!(l3[l3_idx] & DESC_VALID))
        return kernel::Error::NOT_FOUND;
    l3[l3_idx] = 0;
    dsb_sy();
    isb();
    // The leaf translation for `virt` is gone; invalidate it so no stale TLB
    // entry can keep resolving into a freed/reused table page.
    arch_page_table_tlb_flush(virt);

    // Reclaim intermediate tables once they become entirely empty.  Only
    // map-allocated tables reach the empty state (the boot L1/L2 pages stay
    // populated), so freeing them here never releases a boot-static page.
    // Guard on DESC_TABLE: a 2 MiB block descriptor (bit[1]=0) at L2 must
    // never be misinterpreted as an L3 table pointer and freed.
    if ((l2[l2_idx] & DESC_TABLE) != 0) {
        bool l3_empty = true;
        for (size_t i = 0; i < ENTRIES; ++i) {
            if (l3[i] != 0) {
                l3_empty = false;
                break;
            }
        }
        if (l3_empty) {
            l2[l2_idx] = 0;
            kernel::PMM::free_page(l3_phys);
            bool l2_empty = true;
            for (size_t i = 0; i < ENTRIES; ++i) {
                if (l2[i] != 0) {
                    l2_empty = false;
                    break;
                }
            }
            if (l2_empty) {
                l1[l1_idx] = 0;
                kernel::PMM::free_page(l2_phys);
            }
        }
    }
    dsb_sy();
    isb();
    return {};
}

inline uint64_t ArchPageTable::get_physical(uint64_t virt) {
    uint64_t l0_idx = (virt >> L0_SHIFT) & TABLE_MASK;
    uint64_t l1_idx = (virt >> L1_SHIFT) & TABLE_MASK;
    uint64_t l2_idx = (virt >> L2_SHIFT) & TABLE_MASK;
    uint64_t l3_idx = (virt >> L3_SHIFT) & TABLE_MASK;

    uint64_t l0_phys = current();
    uint64_t *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l0_phys);

    if (!(l0[l0_idx] & DESC_VALID))
        return 0;
    uint64_t *l1 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                (l0[l0_idx] & DESC_PHYS_MASK));
    if (!(l1[l1_idx] & DESC_VALID))
        return 0;
    uint64_t *l2 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                (l1[l1_idx] & DESC_PHYS_MASK));

    if ((virt & ((1ULL << L2_SHIFT) - 1)) == 0 && (l2[l2_idx] & DESC_BLOCK)) {
        return (l2[l2_idx] & DESC_BLOCK_MASK) | (virt & 0x1FFFFF);
    }

    if (!(l2[l2_idx] & DESC_VALID))
        return 0;
    uint64_t *l3 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                (l2[l2_idx] & DESC_PHYS_MASK));
    if (!(l3[l3_idx] & DESC_VALID))
        return 0;

    return (l3[l3_idx] & DESC_PHYS_MASK) | (virt & 0xFFF);
}

} // namespace arch
