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

/// @file vmm.hpp
/// @brief Virtual Memory Manager — cross-arch page table management with
///        bounded page-walk depth (4-level x86_64/aarch64, 3-level riscv64
///        Sv39).

#pragma once

#include <types.hpp>
#include <constants.hpp>
#include <kernel/memory/vmm_errors.hpp>

namespace kernel {

namespace cap {
class FrameCap;
}

/// @brief Virtual memory manager — maps virtual to physical pages.
/// @note Page-walk depth is fixed per architecture (4 levels for
/// x86_64/aarch64,
///       3 levels for riscv64 Sv39).  Huge-page split (512 iterations) occurs
///       at most once per 2 MB region.  Both walks are bounded, meeting hard-RT
///       WCET requirements for map_page / map_page_in_pml4.
/// @note Supports user/kernel page flags, TLB flush after each mapping.
class VMM {
  public:
    /// @brief Initialises the VMM and sets up the kernel page tables.
    static void init();
    /// @brief Initialises the VMM with error code.
    /// @return VmmError code.
    static errors::VmmError init_err();

    /// @brief Maps a virtual page to a physical page in the kernel page table.
    /// @param virt_addr Virtual address (page-aligned).
    /// @param phys_addr Physical address (page-aligned).
    /// @param user      If true, sets the user-accessible flag.
    /// @note WCET is bounded: fixed-depth page walk + at most one huge-page
    ///       split (512 iterations) when the target PD entry is a 2 MB block.
    static void map_page(uint64_t virt_addr, uint64_t phys_addr,
                         bool user = false);
    /// @brief Maps a virtual page to a physical page with error code.
    /// @param virt_addr Virtual address (page-aligned).
    /// @param phys_addr Physical address (page-aligned).
    /// @param user      If true, sets the user-accessible flag.
    /// @return VmmError code.
    static errors::VmmError map_page_err(uint64_t virt_addr, uint64_t phys_addr,
                                         bool user = false);

    /// @brief Unmaps a virtual page.
    /// @param virt_addr Virtual address to unmap.
    static void unmap_page(uint64_t virt_addr);
    /// @brief Unmaps a virtual page with error code.
    /// @param virt_addr Virtual address to unmap.
    /// @return VmmError code.
    static errors::VmmError unmap_page_err(uint64_t virt_addr);

    /// @brief Translates a virtual address to its physical address.
    /// @param virt_addr Virtual address.
    /// @return Physical address, or 0 if not mapped.
    static uint64_t virt_to_phys(uint64_t virt_addr);
    /// @brief Translates a virtual address to its physical address with error
    /// code.
    /// @param virt_addr Virtual address.
    /// @param[out] out_phys_addr Physical address on success.
    /// @return VmmError code.
    static errors::VmmError virt_to_phys_err(uint64_t virt_addr,
                                             uint64_t &out_phys_addr);

    /// @brief Returns the physical address of the current PML4 table.
    /// @return Physical address of PML4.
    static uint64_t current_pml4();

    /// @brief Creates a fresh PML4: zeroes user entries (0-255),
    /// copies kernel entries (256-511). Used during exec/load —
    /// NOT for fork (which needs parent user entries).
    /// @return Physical address of the new PML4, or 0 on failure.
    static uint64_t clone_kernel_pml4();
    /// @brief Creates a fresh PML4 with error code.
    /// @param[out] out_pml4_phys Physical address of the new PML4.
    /// @return VmmError code.
    static errors::VmmError clone_kernel_pml4_err(uint64_t &out_pml4_phys);

    /// @brief Maps a page into a specific page table (not the kernel one).
    /// @param virt_addr Virtual address (page-aligned).
    /// @param phys_addr Physical address (page-aligned).
    /// @param user      If true, sets user-accessible flag.
    /// @param pml4_phys Physical address of the target PML4.
    static void map_page_in_pml4(uint64_t virt_addr, uint64_t phys_addr,
                                 bool user, uint64_t pml4_phys);
    /// @brief Maps a page into a specific page table with explicit W^X
    ///        permission (VULN-H1).  When @p executable is false the
    ///        architectural NX bit is set (x86_64 bit 63, aarch64 bit 54)
    ///        or the X bit is dropped (riscv64), so the page cannot execute.
    /// @param virt_addr Virtual address (page-aligned).
    /// @param phys_addr Physical address (page-aligned).
    /// @param user      If true, sets user-accessible flag.
    /// @param executable If true, page is executable (NX clear); if false, NX set.
    /// @param pml4_phys Physical address of the target PML4.
    static void map_page_in_pml4(uint64_t virt_addr, uint64_t phys_addr,
                                 bool user, bool executable,
                                 uint64_t pml4_phys);
    /// @brief Maps a page into a specific page table with error code.
    /// @param virt_addr Virtual address (page-aligned).
    /// @param phys_addr Physical address (page-aligned).
    /// @param user      If true, sets user-accessible flag.
    /// @param pml4_phys Physical address of the target PML4.
    /// @return VmmError code.
    static errors::VmmError map_page_in_pml4_err(uint64_t virt_addr,
                                                 uint64_t phys_addr, bool user,
                                                 uint64_t pml4_phys);

    /// @brief Return the physical address of the kernel PML4 table.
    /// @return Physical address of the kernel page-table root.
    static uint64_t get_kernel_pml4() {
        return kernel_pml4_;
    }

    /// @brief Frees all user-space pages and page tables from a
    /// user PML4. Used during exec() to clean up the old address
    /// space. Skips kernel-owned pages (PMM::is_user_page owner-bit
    /// check) — v0.4.0 MP-7: no task shares another's user entries, so
    /// this is always the full teardown of a private address space.
    /// @param pml4_phys Physical address of the user PML4.
    /// @brief Deep-copy user-space page tables from src_pml4 to dst_pml4.
    ///        Allocates new PDPT/PD/PT pages and copies data page content.
    /// @param src_pml4 Physical address of source PML4.
    /// @param dst_pml4 Physical address of destination PML4 (must already
    ///        have kernel entries populated).
    /// @return true on success, false on OOM (partial state may remain).
    static bool deep_copy_user_pages(uint64_t src_pml4, uint64_t dst_pml4);
    static void free_user_pages(uint64_t pml4_phys);
    /// @brief Frees all user-space pages and page tables with error code.
    /// @param pml4_phys Physical address of the user PML4.
    /// @return VmmError code.
    static errors::VmmError free_user_pages_err(uint64_t pml4_phys);

    /// @brief Maps the frame(s) owned by a FrameCap into @p pml4_phys.
    ///        Refuses a revoked FrameCap (ROADMAP 0.4.1 CSpace) — mapping
    ///        capability-gated memory requires a live, non-revoked cap.
    /// @param fc        The FrameCap (must be pinned by the caller).
    /// @param virt_addr Base virtual address (page-aligned).
    /// @param user      If true, sets user-accessible flag.
    /// @param pml4_phys Physical address of the target PML4.
    /// @return true when every frame was mapped, false on a revoked cap or
    ///         a zero frame.
    static bool map_frame_from_cap(class cap::FrameCap *fc, uint64_t virt_addr,
                                   bool user, uint64_t pml4_phys);

    /// @brief Unmaps one page previously mapped via map_frame_from_cap().
    /// @param virt_addr Virtual address to unmap.
    /// @param pml4_phys Physical address of the target PML4.
    static void unmap_frame_from_cap(uint64_t virt_addr, uint64_t pml4_phys);

    /// @brief Translates a virtual address to a physical address
    /// using a specific PML4.
    /// @param virt_addr Virtual address to translate.
    /// @param pml4_phys Physical address of the target PML4.
    /// @return Physical address, or 0 if not mapped.
    static uint64_t virt_to_phys_in_pml4(uint64_t virt_addr,
                                         uint64_t pml4_phys);
    /// @brief Translates a virtual address to a physical address using a
    /// specific PML4 with error code.
    /// @param virt_addr Virtual address to translate.
    /// @param pml4_phys Physical address of the target PML4.
    /// @param[out] out_phys_addr Physical address on success.
    /// @return VmmError code.
    static errors::VmmError virt_to_phys_in_pml4_err(uint64_t virt_addr,
                                                     uint64_t pml4_phys,
                                                     uint64_t &out_phys_addr);

  private:
    static constexpr uint64_t PAGE_SIZE = CONFIG_PAGE_SIZE;
    static constexpr uint64_t PAGE_TABLE_ENTRIES = 512;

#if defined(CONFIG_ARCH_AARCH64)
    // aarch64 stage-1 page-table descriptor bit layout.
    // Table descriptors (L0-L2) and page descriptors (L3): bits[1:0]=11.
    // Block descriptors (L1-L2): bits[1:0]=01.
    // AP[1:0] in bits[7:6];  00=EL1-RW/EL0-*, 01=EL1-RW/EL0-RW,
    //   10=EL1-RO/EL0-*, 11=EL1-RO/EL0-RO.
    // AF (Access Flag) must be 1 for stage-1 - bit 8 (NOT bit 10).
    static constexpr uint64_t PAGE_PRESENT = 1ULL << 0;
    static constexpr uint64_t PAGE_TABLE = 1ULL << 1;
    static constexpr uint64_t PAGE_AF = 1ULL << 8; // Access Flag (stage-1)
    static constexpr uint64_t PAGE_AP_USER = 1ULL
                                             << 6;    // AP[0] - EL0 accessible
    static constexpr uint64_t PAGE_AP_RO = 1ULL << 7; // AP[1] - read-only (EL1)

    // Compatibility aliases (used in flag-building expressions)
    static constexpr uint64_t PAGE_WRITE = 0; // no-op - writable by default
    static constexpr uint64_t PAGE_USER = PAGE_AP_USER;
    static constexpr uint64_t PAGE_HUGE = 0; // not a flag; detected via level
#elif defined(CONFIG_ARCH_RISCV64)
    // RISC-V Sv39 PTE format (from privilege spec)
    // bit 0: V (Valid)
    // bit 1: R (Readable)
    // bit 2: W (Writable)
    // bit 3: X (Executable)
    // bit 4: U (User)
    // bit 5: G (Global)
    // bit 6: A (Accessed)
    // bit 7: D (Dirty)
    // bit 8: RSW (reserved for software)
    // bits 9-53: PPN[0..44] (physical page number)
    // bits 54-63: reserved for future use
    static constexpr uint64_t PAGE_PRESENT = 1ULL << 0;  // V
    static constexpr uint64_t PAGE_READ = 1ULL << 1;     // R
    static constexpr uint64_t PAGE_WRITE = 1ULL << 2;    // W
    static constexpr uint64_t PAGE_EXEC = 1ULL << 3;     // X
    static constexpr uint64_t PAGE_USER = 1ULL << 4;     // U
    static constexpr uint64_t PAGE_GLOBAL = 1ULL << 5;   // G
    static constexpr uint64_t PAGE_ACCESSED = 1ULL << 6; // A
    static constexpr uint64_t PAGE_DIRTY = 1ULL << 7;    // D
    // Table entry: V=1, R=W=X=0 (points to next level)
    static constexpr uint64_t PAGE_TABLE =
        PAGE_PRESENT; // V=1, no RWX = table pointer
    // Block/leaf detection: if V=1 and (R|W|X)=1 then leaf page
    static constexpr uint64_t PAGE_HUGE = 0; // detected via R|W|X bits
#else
    static constexpr uint64_t PAGE_PRESENT = 1ULL << 0;
    static constexpr uint64_t PAGE_WRITE = 1ULL << 1;
    static constexpr uint64_t PAGE_USER = 1ULL << 2;
    static constexpr uint64_t PAGE_HUGE = 1ULL << 7;
#endif

#if defined(CONFIG_ARCH_AARCH64)
    // aarch64: physical frame bits [47:12] (48-bit PA).  Masks off flags,
    // UXN (bit 54) and PXN (bit 55).
    static constexpr uint64_t PAGE_FRAME_MASK = 0x0000FFFFFFFFF000ULL;
    static constexpr uint64_t PAGE_HUGE_FRAME_MASK = 0x0000FFFFFFE00000ULL;
#elif defined(CONFIG_ARCH_RISCV64)
    // Sv39: PPN bits [43:12] (34-bit PA).  Masks off flags + reserved bits.
    static constexpr uint64_t PAGE_FRAME_MASK = 0x00000003FFFFFFF000ULL;
    static constexpr uint64_t PAGE_HUGE_FRAME_MASK = 0x00000003FFFFE00000ULL;
#else
    // x86_64: physical frame bits [51:12] (52-bit PA).  Masks off all flags
    // INCLUDING NX (bit 63) — without this, a leaf PTE with NX set yields a
    // "physical address" whose bit 63 wraps HHDM_OFFSET + phys to a
    // non-canonical address (VULN-H1 GPF).
    static constexpr uint64_t PAGE_FRAME_MASK = 0x000FFFFFFFFFF000ULL;
    static constexpr uint64_t PAGE_HUGE_FRAME_MASK = 0x000FFFFFFFE00000ULL;
#endif

#if defined(CONFIG_ARCH_RISCV64)
    // Sv39: 3-level page table
    // L0 (1GB): bits 38:30
    // L1 (2MB): bits 29:21
    // L2 (4KB): bits 20:12
    static constexpr uint64_t L0_SHIFT = 30;
    static constexpr uint64_t L1_SHIFT = 21;
    static constexpr uint64_t L2_SHIFT = 12;
    static constexpr uint64_t TABLE_MASK = 0x1FFULL;
    static constexpr uint64_t L0_MASK = TABLE_MASK << L0_SHIFT;
    static constexpr uint64_t L1_MASK = TABLE_MASK << L1_SHIFT;
    static constexpr uint64_t L2_MASK = TABLE_MASK << L2_SHIFT;
#endif

    static constinit uint64_t kernel_pml4_;

  public:
    /// @brief Set to true when a test modifies kernel-space page tables
    ///        (HHDM range).  Reset after PD restore in snapshot_restore.
    static bool hhdm_modified_;

    /// @brief Set to true when a test modifies the LOW identity map
    ///        (PD_IDENTITY, phys 0x3000 — VAs in PML4[0] below
    ///        PML4_USER_COUNT).  Reset after PD restore in snapshot_restore.
    static bool identity_modified_;

    /// @brief Returns true if any test has modified the HHDM page tables
    ///        since the last clear_hhdm_modified() call.
    static bool hhdm_was_modified() { return hhdm_modified_; }
    /// @brief Reset the HHDM modification flag (called after PD restore).
    static void clear_hhdm_modified() { hhdm_modified_ = false; }

    /// @brief Returns true if any test has modified the low identity page
    ///        tables since the last clear_identity_modified() call.
    static bool identity_was_modified() { return identity_modified_; }
    /// @brief Reset the identity modification flag (called after PD restore).
    static void clear_identity_modified() { identity_modified_ = false; }

  private:

    /// @brief Walks or creates a page-table entry at the given level.
    /// @param table Pointer to the current-level page table.
    /// @param index Index into the table.
    /// @param create If true, allocates a new table if missing.
    /// @param user_alloc If true, allocate page table page as USER-owned.
    /// @return Pointer to the next-level table, or nullptr.
    static uint64_t *get_table(uint64_t *table, size_t index, bool create,
                               bool user_alloc = false);
};

} // namespace kernel
