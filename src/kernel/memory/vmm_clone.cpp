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
                                             (kernel_pml4_ & PAGE_TABLE_ADDR_MASK));
    auto *dst = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + phys);

    // Clear user-space entries (L0 indices 0-255 for 0-256GB)
    for (size_t i = 0; i < 256; ++i) {
        dst[i] = 0;
    }
    // Issue #205: riscv64 low half holds kernel MMIO+identity (L0[0]:
    // 1GB window with PLIC/UART/ECAM leaves + RAM identity), not user
    // space — every address space needs it (drivers touch raw-phys
    // identity; aarch64 instead uses HHDM aliases so its clear stands).
    // U=0 on all copied entries: U-mode cannot reach them (#206-safe).
    // Issue #221: L0[0] ALSO covers every user low-half mapping
    // (.text/stack/heap below 1GB).  Sharing src[0] lets get_table()
    // install one task's child tables into the shared L1 (absent path)
    // or descend into another task's L2 (table path), so same-VA images
    // hijack each other's PTEs (last writer wins) and leaf splits
    // corrupt shared kernel entries.  Deep-copy the subtree instead:
    // MMIO/identity leaves stay visible by value (U=0) for IRQ-context
    // drivers, while child tables become private.  USER entries are
    // never copied (stale hijack PTEs must not propagate; the loader
    // maps fresh pages for the new task).
    uint64_t l0_low = src[0];
    if ((l0_low & PAGE_PRESENT) == 0) {
        dst[0] = 0;
    } else if ((l0_low & (PAGE_READ | PAGE_WRITE | PAGE_EXEC)) != 0) {
        // 1GB leaf: no subtree to share, copy by value.
        dst[0] = l0_low;
    } else {
        uint64_t l1_phys = PMM::alloc_user_page();
        if (l1_phys == 0) {
            ASSERT(errors::VmmError::VMM_ERR_PML4_ALLOC);
            PMM::free_page(phys);
            return 0;
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *src_l1 = reinterpret_cast<uint64_t *>(
            arch::HHDM_OFFSET + VMM::sv39_pte_phys(l0_low));
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *dst_l1 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                    l1_phys);
        // Audit (iteration 2): the page is recycled unzeroed, and the
        // !copy_ok unwind below scans all 512 slots — zero first so
        // never-written slots cannot drive free_page on stale entries.
        __builtin_memset(dst_l1, 0, 4096);
        bool copy_ok = true;
        for (size_t l1_idx = 0; l1_idx < PAGE_TABLE_ENTRIES; ++l1_idx) {
            const uint64_t l1_entry = src_l1[l1_idx];
            if ((l1_entry & PAGE_PRESENT) == 0 ||
                (l1_entry & PAGE_USER) != 0) {
                dst_l1[l1_idx] = 0;
                continue;
            }
            if ((l1_entry & (PAGE_READ | PAGE_WRITE | PAGE_EXEC)) != 0) {
                // 2MB leaf (MMIO/identity): copy by value.
                dst_l1[l1_idx] = l1_entry;
                continue;
            }
            const uint64_t l2_phys = PMM::alloc_user_page();
            if (l2_phys == 0) {
                copy_ok = false;
                break;
            }
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *src_l2 = reinterpret_cast<uint64_t *>(
                arch::HHDM_OFFSET + VMM::sv39_pte_phys(l1_entry));
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *dst_l2 = reinterpret_cast<uint64_t *>(
                arch::HHDM_OFFSET + l2_phys);
            for (size_t l2_idx = 0; l2_idx < PAGE_TABLE_ENTRIES; ++l2_idx) {
                const uint64_t l2_entry = src_l2[l2_idx];
                dst_l2[l2_idx] =
                    ((l2_entry & PAGE_PRESENT) == 0 ||
                     (l2_entry & PAGE_USER) != 0)
                        ? 0
                        : l2_entry;
            }
            // Table entry: V=1, no RWX = points to next level table.
            dst_l1[l1_idx] =
                VMM::sv39_phys_pte(l2_phys) | PAGE_PRESENT;
        }
        if (!copy_ok) {
            for (size_t l1_idx = 0; l1_idx < PAGE_TABLE_ENTRIES; ++l1_idx) {
                const uint64_t l1_entry = dst_l1[l1_idx];
                if ((l1_entry & PAGE_PRESENT) != 0 &&
                    (l1_entry & (PAGE_READ | PAGE_WRITE | PAGE_EXEC)) == 0) {
                    PMM::free_page(VMM::sv39_pte_phys(l1_entry));
                }
            }
            PMM::free_page(l1_phys);
            PMM::free_page(phys);
            ASSERT(errors::VmmError::VMM_ERR_PML4_ALLOC);
            return 0;
        }
        dst[0] = VMM::sv39_phys_pte(l1_phys) | PAGE_PRESENT;
    }
    // Copy kernel-space entries (L0 indices 256-511 for 256GB-512GB)
    for (size_t i = 256; i < 512; ++i) {
        dst[i] = src[i];
    }
    return phys;
#else
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *src = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                             (kernel_pml4_ & PAGE_TABLE_ADDR_MASK));
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

/// @brief True when @p entry points at a next-level table (vs a huge/leaf
///        mapping).  Arch-aware: x86_64 uses the PS bit, RISC-V the R|W|X
///        bits, AArch64 the descriptor type at non-leaf levels.
/// @param entry Raw 64-bit page-table entry (must be PRESENT).
/// @param level Table level holding the entry (0=PML4/L0 .. 2=PD/L2;
///        level 3=PT/L3 entries are always leaves).
bool VMM::is_table_entry(uint64_t entry, unsigned level) {
    if (level >= 3)
        return false;
    // Arch leaf detection lives in the per-arch TU (issue #363 P2).
    return vmm_arch_is_table(entry);
}

/// @brief Recursive merge worker (issue #96): converge @p dst_table
///        toward @p src_table over [@p start, 512).  Missing-present
///        entries are linked (copied by value); present entries are left
///        untouched unless both sides hold tables, in which case the
///        pair is merged recursively (hole-filling).  Zero allocations.
/// @param src_table HHDM virtual address of the source table page.
/// @param dst_table HHDM virtual address of the child table page.
/// @param level Table level (0=PML4/L0 .. 3=PT/L3).
/// @param start First index to converge (kernel-half start at level 0).
void VMM::merge_table_level(uint64_t *src_table, uint64_t *dst_table,
                            unsigned level, size_t start) {
    for (size_t i = start; i < 512; ++i) {
        uint64_t src_entry = src_table[i];
        if ((src_entry & PAGE_PRESENT) == 0)
            continue;
        uint64_t dst_entry = dst_table[i];
        if ((dst_entry & PAGE_PRESENT) != 0) {
            if (is_table_entry(src_entry, level) &&
                is_table_entry(dst_entry, level)) {
                // Arch child-table decode (issue #363 P2: Sv39 PTE codec
                // vs frame mask).
                auto *src_next = reinterpret_cast<uint64_t *>(
                    arch::HHDM_OFFSET + vmm_arch_table_child(src_entry));
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                auto *dst_next = reinterpret_cast<uint64_t *>(
                    arch::HHDM_OFFSET + vmm_arch_table_child(dst_entry));
                merge_table_level(src_next, dst_next, level + 1, 0);
            }
            continue;
        }
        dst_table[i] = src_entry;
    }
}

/// @brief Recursive equality worker (issue #96): true when the two
///        tables agree on every present entry over [@p start, 512),
///        descending into table/table pairs.
bool VMM::tables_equal_level(const uint64_t *a_table,
                             const uint64_t *b_table, unsigned level,
                             size_t start) {
    for (size_t i = start; i < 512; ++i) {
        uint64_t a_entry = a_table[i];
        uint64_t b_entry = b_table[i];
        bool a_present = (a_entry & PAGE_PRESENT) != 0;
        bool b_present = (b_entry & PAGE_PRESENT) != 0;
        if (a_present != b_present)
            return false;
        if (!a_present)
            continue;
        if (is_table_entry(a_entry, level) &&
            is_table_entry(b_entry, level)) {
            // Arch child-table decode (issue #363 P2).
            auto *a_next = reinterpret_cast<const uint64_t *>(
                arch::HHDM_OFFSET + vmm_arch_table_child(a_entry));
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *b_next = reinterpret_cast<const uint64_t *>(
                arch::HHDM_OFFSET + vmm_arch_table_child(b_entry));
            if (!tables_equal_level(a_next, b_next, level + 1, 0))
                return false;
        } else if (a_entry != b_entry) {
            return false;
        }
    }
    return true;
}

bool VMM::merge_kernel_half(uint64_t src_pml4, uint64_t dst_pml4) {
    if (src_pml4 == 0 || dst_pml4 == 0 || src_pml4 == dst_pml4)
        return false;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *src = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                             (src_pml4 & PAGE_TABLE_ADDR_MASK));
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *dst = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                             (dst_pml4 & PAGE_TABLE_ADDR_MASK));
    // INV-MERGE1: single-threaded pre-CR3-switch window (same window MP-7
    // uses) — no locking; link semantics keep lower tables shared, so no
    // allocation can fail mid-merge by construction.
    merge_table_level(src, dst, 0, arch::PML4_KERNEL_START);
    return true;
}

void VMM::snapshot_kernel_template() {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *live = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (kernel_pml4_ & PAGE_TABLE_ADDR_MASK));
    for (size_t i = arch::PML4_KERNEL_START; i < PAGE_TABLE_ENTRIES; ++i)
        pml4_kernel_template_[i] = live[i];
}

bool VMM::kernel_half_equal(uint64_t a_pml4, uint64_t b_pml4) {
    if (a_pml4 == 0 || b_pml4 == 0)
        return false;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *a = reinterpret_cast<const uint64_t *>(
        arch::HHDM_OFFSET + (a_pml4 & PAGE_TABLE_ADDR_MASK));
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *b = reinterpret_cast<const uint64_t *>(
        arch::HHDM_OFFSET + (b_pml4 & PAGE_TABLE_ADDR_MASK));
    return tables_equal_level(a, b, 0, arch::PML4_KERNEL_START);
}

bool VMM::kernel_half_matches_template(uint64_t pml4_phys) {
    if (pml4_phys == 0)
        return false;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *tbl = reinterpret_cast<const uint64_t *>(
        arch::HHDM_OFFSET + (pml4_phys & PAGE_TABLE_ADDR_MASK));
    for (size_t i = arch::PML4_KERNEL_START; i < PAGE_TABLE_ENTRIES; ++i) {
        if (tbl[i] != pml4_kernel_template_[i])
            return false;
    }
    return true;
}

void VMM::assert_kernel_half_converged(uint64_t child_pml4) {
    if (!kernel_half_equal(child_pml4, kernel_pml4_))
        panic("VMM: child kernel half diverged from live kernel PML4");
#if CONFIG_DEBUG
    if (!kernel_half_matches_template(child_pml4))
        Logger::info("VMM: child kernel half drifted from bring-up "
                     "template (late mapping converged)");
#endif
}


} // namespace kernel
