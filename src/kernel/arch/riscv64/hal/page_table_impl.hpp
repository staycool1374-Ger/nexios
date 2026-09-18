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
/// @brief RISC-V64 Sv39 3-level page table manipulation — map, unmap,
///        walk, TLB flush, and SATP CSR management.

#pragma once

#include <types.hpp>
#include <kernel/arch/hal/io.hpp>
#include <kernel/arch/hal/page_flags.hpp>
#include <lib/error.hpp>
#include <lib/string.hpp>
#include <kernel/memory/pmm.hpp>

namespace arch {

/// @brief Sv39 3-level page table layout:
///        L0 -> 1 GB regions (bits 38:30), L1 -> 2 MB regions (bits 29:21),
///        L2 -> 4 KB pages (bits 20:12). Each level has 512 entries.

/// @brief Read the current page-table root physical address from SATP.
/// @return Physical address of the root page table.
inline uint64_t arch_page_table_current() {
    uint64_t satp{};
    asm volatile("csrr %0, satp" : "=r"(satp) : : "memory");
    return (satp & 0xFFFFFFFFFFF) << 12; // PPN -> physical address
}

/// @brief Activate a page table by writing SATP with Sv39 mode.
/// @param phys_ppn_shifted Physical address of the root page table.
inline void arch_page_table_activate(uint64_t phys_ppn_shifted) {
    // Input is physical address >> 12 (PPN)
    uint64_t satp = (8ULL << 60) | (phys_ppn_shifted >> 12); // Sv39 mode | PPN
    asm volatile("csrw satp, %0" : : "r"(satp) : "memory");
    asm volatile("sfence.vma" : : : "memory");
}

/// @brief Flush TLB for a single virtual address.
/// @param virt_addr Virtual address to invalidate.
inline void arch_page_table_tlb_flush(uint64_t virt_addr) {
    asm volatile("sfence.vma %0" : : "r"(virt_addr) : "memory");
}

/// @brief Flush the entire TLB.
inline void arch_page_table_tlb_flush_all() {
    asm volatile("sfence.vma" : : : "memory");
}

/// @brief Purge one context's non-global TLB entries (issue #184).
///        Fallback: this port allocates no ASIDs, so the required semantics
///        are provided via a full fence — a safe superset of a
///        single-context purge, mirroring the x86_64 CR3-reload fallback.
///        Local CPU only; cross-CPU invalidation is out of scope.
/// @param pcid Owning address-space id, ignored (0 = untagged/kernel).
inline void tlb_purge_context([[maybe_unused]] uint16_t pcid) {
    arch_page_table_tlb_flush_all();
}

/// @brief Sv39 page table manager — wraps arch_page_table_* free functions
///        and provides map/unmap/lookup operations.
class ArchPageTable {
  public:
    /// @brief Return the current page-table root physical address.
    static inline uint64_t current() {
        return arch_page_table_current();
    }
    /// @brief Activate a page table by writing SATP.
    static inline void activate(uint64_t phys) {
        arch_page_table_activate(phys);
    }
    /// @brief Flush TLB for a single virtual address.
    static inline void tlb_flush(uint64_t virt_addr) {
        arch_page_table_tlb_flush(virt_addr);
    }
    /// @brief Flush the entire TLB.
    static inline void tlb_flush_all() {
        arch_page_table_tlb_flush_all();
    }

    /// @brief Page size in bytes (4 KB).
    static constexpr uint64_t PAGE_SIZE = CONFIG_PAGE_SIZE;
    /// @brief Number of entries per page-table level (512).
    static constexpr uint64_t ENTRIES = 512;

    /// @brief Map a virtual page to a physical page with the given flags.
    /// @param virt  Virtual address (must be 4 KB aligned for page mappings).
    /// @param phys  Physical address to map.
    /// @param flags PageFlags bitmask (PRESENT, WRITE, USER, NX, etc.).
    /// @return ErrorOr<void> — OOM if page-table allocation fails.
    static kernel::ErrorOr<void> map_page(uint64_t virt, uint64_t phys,
                                          uint64_t flags);
    /// @brief Unmap a virtual page (zeros the PTE).
    /// @param virt Virtual address to unmap.
    /// @return ErrorOr<void> — NOT_FOUND if the mapping does not exist.
    static kernel::ErrorOr<void> unmap_page(uint64_t virt);
    /// @brief Look up the physical address mapped to a virtual address.
    /// @param virt Virtual address to look up.
    /// @return Physical address, or 0 if not mapped.
    static uint64_t get_physical(uint64_t virt);

  private:
    static constexpr uint64_t L0_SHIFT = 30;
    static constexpr uint64_t L1_SHIFT = 21;
    static constexpr uint64_t L2_SHIFT = 12;
    static constexpr uint64_t TABLE_MASK = 0x1FF;
    /// @brief Page-table descriptor: valid bit.
    static constexpr uint64_t DESC_VALID = 1ULL << 0;
    /// @brief Page-table descriptor: table pointer (points to next level).
    static constexpr uint64_t DESC_TABLE = 1ULL << 1;
    /// @brief Page-table descriptor: 4 KB page leaf (L2).
    static constexpr uint64_t DESC_PAGE = 1ULL << 1;
    /// @brief Page-table descriptor: 2 MB / 1 GB block leaf.
    static constexpr uint64_t DESC_BLOCK = 0ULL;
    /// @brief Page-table descriptor: accessed flag.
    static constexpr uint64_t DESC_AF = 1ULL << 6;
    /// @brief Page-table descriptor: global mapping.
    static constexpr uint64_t DESC_G = 1ULL << 5;
    /// @brief Page-table descriptor: user-accessible.
    static constexpr uint64_t DESC_USER = 1ULL << 4;
    /// @brief Page-table descriptor: readable.
    static constexpr uint64_t DESC_R = 1ULL << 1;
    /// @brief Page-table descriptor: writable.
    static constexpr uint64_t DESC_W = 1ULL << 2;
    /// @brief Page-table descriptor: executable.
    static constexpr uint64_t DESC_X = 1ULL << 3;
    /// @brief Page-table descriptor: accessed (same as AF on some impls).
    static constexpr uint64_t DESC_A = 1ULL << 6;
    /// @brief Page-table descriptor: dirty.
    static constexpr uint64_t DESC_D = 1ULL << 7;

    /// @brief Get or create a page-table entry at a given index.
    /// @param table_base Physical address of the current-level table.
    /// @param index      Entry index within the table.
    /// @param create     If true and the entry is invalid, allocate a new page.
    /// @return Pointer to the next-level table, or nullptr on failure.
    static uint64_t *get_table(uint64_t table_base, uint64_t index,
                               bool create);
    /// @brief Convert PageFlags bitmask to Sv39 descriptor attributes.
    /// @param flags PageFlags bitmask.
    /// @return Sv39 descriptor attribute bits.
    static uint64_t attr_from_flags(uint64_t flags);
    /// @brief Walk L0 table (bits 38:30) and return the L1 table pointer.
    static uint64_t *walk_l0(uint64_t l0_base, size_t l0_idx, bool create);
    /// @brief Walk L1 table (bits 29:21) and return the L2 table pointer.
    static uint64_t *walk_l1(uint64_t l1_base, size_t l1_idx, bool create);
    /// @brief Walk L2 table (bits 20:12) and return the page entry.
    static uint64_t *walk_l2(uint64_t l2_base, size_t l2_idx, bool create);
};

/// @brief Get or create a page-table entry at the given index.
/// @param table_base Physical address of the table.
/// @param index      Entry index (0–511).
/// @param create     If true and the entry is invalid, allocate a zeroed page.
/// @return Pointer to the next-level table, or nullptr if !create and invalid,
///         or on OOM.
inline uint64_t *ArchPageTable::get_table(uint64_t table_base, uint64_t index,
                                          bool create) {
    uint64_t *table = reinterpret_cast<uint64_t *>(table_base);
    uint64_t entry = table[index];

    if (entry & DESC_VALID) {
        return reinterpret_cast<uint64_t *>(entry & ~0xFFF);
    }

    if (!create)
        return nullptr;

    uint64_t new_page = kernel::PMM::alloc_page_table();
    if (!new_page)
        return nullptr;

    void *new_page_ptr = reinterpret_cast<void *>(new_page);
    memset(new_page_ptr, 0, PAGE_SIZE);
    // Mark as valid, readable, writable table pointer
    table[index] =
        (new_page & ~0xFFF) | DESC_VALID | DESC_TABLE | DESC_R | DESC_W;
    return reinterpret_cast<uint64_t *>(new_page);
}

/// @brief Convert kernel PageFlags to Sv39 descriptor attributes.
/// @param flags PageFlags bitmask (PRESENT, WRITE, USER, NX, etc.).
/// @return Sv39 descriptor bits (V, R, W, X, U, A).
inline uint64_t ArchPageTable::attr_from_flags(uint64_t flags) {
    uint64_t attr = DESC_VALID | DESC_AF;
    (void)DESC_G;
    if (flags & kernel::PageFlags::PRESENT)
        attr |= DESC_VALID;
    if (!(flags & kernel::PageFlags::NX))
        attr |= DESC_X;
    if (flags & kernel::PageFlags::WRITE)
        attr |= DESC_W;
    if (flags & kernel::PageFlags::USER)
        attr |= DESC_USER;
    // Default to readable unless explicitly no-access
    if (flags & kernel::PageFlags::PRESENT)
        attr |= DESC_R;
    return attr;
}

/// @brief Walk L0 (bits 38:30) to get or create the L1 table.
/// @param l0_base Physical address of the root table.
/// @param l0_idx  L0 index.
/// @param create  If true, allocate a new table if missing.
/// @return Pointer to L1 table, or nullptr.
inline uint64_t *ArchPageTable::walk_l0(uint64_t l0_base, size_t l0_idx,
                                        bool create) {
    return get_table(l0_base, l0_idx, create);
}

/// @brief Walk L1 (bits 29:21) to get or create the L2 table.
/// @param l1_base Physical address of the L1 table.
/// @param l1_idx  L1 index.
/// @param create  If true, allocate a new table if missing.
/// @return Pointer to L2 table, or nullptr.
inline uint64_t *ArchPageTable::walk_l1(uint64_t l1_base, size_t l1_idx,
                                        bool create) {
    return get_table(l1_base, l1_idx, create);
}

/// @brief Walk L2 (bits 20:12) to get or create the page entry.
/// @param l2_base Physical address of the L2 table.
/// @param l2_idx  L2 index.
/// @param create  If true, allocate a new table if missing.
/// @return Pointer to the L2 entry, or nullptr.
inline uint64_t *ArchPageTable::walk_l2(uint64_t l2_base, size_t l2_idx,
                                        bool create) {
    return get_table(l2_base, l2_idx, create);
}

/// @brief Map a virtual address to a physical address with Sv39 attributes.
/// @param virt  Virtual address.
/// @param phys  Physical address.
/// @param flags PageFlags bitmask.
/// @return ErrorOr<void> — OOM if page-table page allocation fails.
inline kernel::ErrorOr<void>
ArchPageTable::map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t l0_idx = (virt >> L0_SHIFT) & TABLE_MASK;
    uint64_t l1_idx = (virt >> L1_SHIFT) & TABLE_MASK;
    uint64_t l2_idx = (virt >> L2_SHIFT) & TABLE_MASK;

    uint64_t l0_phys = current();
    uint64_t *l1 = walk_l0(l0_phys, l0_idx, true);
    if (!l1)
        return kernel::Error::OOM;

    uint64_t *l2 = walk_l1(reinterpret_cast<uint64_t>(l1), l1_idx, true);
    if (!l2)
        return kernel::Error::OOM;

    uint64_t entry_flags = attr_from_flags(flags);

    // Try 2MB block mapping if aligned
    if ((virt & ((1ULL << L1_SHIFT) - 1)) == 0 &&
        (phys & ((1ULL << L1_SHIFT) - 1)) == 0) {
        l2[l1_idx] =
            (phys & ~0x1FFFFF) | entry_flags | DESC_BLOCK | (1ULL << 6);
        return {};
    }

    // 4KB page at L2
    l2[l2_idx] =
        (phys & ~0xFFF) | entry_flags | DESC_PAGE | (1ULL << 6) | (1ULL << 7);
    return {};
}

/// @brief Unmap a virtual address by zeroing its PTE.
/// @param virt Virtual address to unmap.
/// @return ErrorOr<void> — NOT_FOUND if no valid mapping exists.
inline kernel::ErrorOr<void> ArchPageTable::unmap_page(uint64_t virt) {
    uint64_t l0_idx = (virt >> L0_SHIFT) & TABLE_MASK;
    uint64_t l1_idx = (virt >> L1_SHIFT) & TABLE_MASK;
    uint64_t l2_idx = (virt >> L2_SHIFT) & TABLE_MASK;

    uint64_t l0_phys = current();
    uint64_t *l0 = reinterpret_cast<uint64_t *>(l0_phys);

    if (!(l0[l0_idx] & DESC_VALID))
        return kernel::Error::NOT_FOUND;
    uint64_t *l1 = reinterpret_cast<uint64_t *>(l0[l0_idx] & ~0xFFF);
    if (!(l1[l1_idx] & DESC_VALID))
        return kernel::Error::NOT_FOUND;
    uint64_t *l2 = reinterpret_cast<uint64_t *>(l1[l1_idx] & ~0xFFF);

    // Check for 2MB block
    if ((virt & ((1ULL << L1_SHIFT) - 1)) == 0 && !(l2[l1_idx] & DESC_TABLE)) {
        l2[l1_idx] = 0;
        return {};
    }

    if (!(l2[l2_idx] & DESC_VALID))
        return kernel::Error::NOT_FOUND;
    l2[l2_idx] = 0;
    return {};
}

/// @brief Walk the page table to find the physical address for a virtual
/// address.
/// @param virt Virtual address to translate.
/// @return Physical address, or 0 if not mapped.
inline uint64_t ArchPageTable::get_physical(uint64_t virt) {
    uint64_t l0_idx = (virt >> L0_SHIFT) & TABLE_MASK;
    uint64_t l1_idx = (virt >> L1_SHIFT) & TABLE_MASK;
    uint64_t l2_idx = (virt >> L2_SHIFT) & TABLE_MASK;

    uint64_t l0_phys = current();
    uint64_t *l0 = reinterpret_cast<uint64_t *>(l0_phys);

    if (!(l0[l0_idx] & DESC_VALID))
        return 0;
    uint64_t *l1 = reinterpret_cast<uint64_t *>(l0[l0_idx] & ~0xFFF);
    if (!(l1[l1_idx] & DESC_VALID))
        return 0;
    uint64_t *l2 = reinterpret_cast<uint64_t *>(l1[l1_idx] & ~0xFFF);

    // 2MB block mapping
    if ((virt & ((1ULL << L1_SHIFT) - 1)) == 0 && !(l2[l1_idx] & DESC_TABLE)) {
        return (l2[l1_idx] & ~0x1FFFFF) | (virt & 0x1FFFFF);
    }

    if (!(l2[l2_idx] & DESC_VALID))
        return 0;
    return (l2[l2_idx] & ~0xFFF) | (virt & 0xFFF);
}

} // namespace arch
