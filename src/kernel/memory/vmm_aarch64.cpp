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

#if defined(CONFIG_ARCH_AARCH64)

// Issue #363 P2: aarch64 leaf codec, bit-identical to the pre-split inline
// encodings. Shared walk skeletons (VMM policy) call these.

bool VMM::vmm_arch_is_table(uint64_t entry) {
    return (entry & 0x3ULL) == 0x3ULL;
}

uint64_t VMM::vmm_arch_table_child(uint64_t entry) {
    return entry & PAGE_TABLE_ADDR_MASK;
}

bool VMM::vmm_arch_split_needed(uint64_t entry) {
    return (entry & (PAGE_PRESENT | PAGE_TABLE)) == PAGE_PRESENT;
}

void VMM::vmm_arch_split_fill(uint64_t *new_table, uint64_t src_entry) {
    uint64_t huge_base = src_entry & kHuge2MBaseMask;
    uint64_t base_flags = src_entry & (PAGE_PRESENT | PAGE_AF | PAGE_UXN |
                                       PAGE_PXN | PAGE_ATTR_NORMAL);
    for (size_t i = 0; i < 512; ++i) {
        // L3 page leaves: bits[1:0]=11 (PAGE_TABLE set) — bit1=0 at L3
        // is a block-at-invalid-level encoding and faults as a
        // translation fault.
        new_table[i] = (huge_base + i * 0x1000) | base_flags | PAGE_TABLE |
                       PAGE_ATTR_NORMAL;
    }
}

uint64_t VMM::vmm_arch_table_link(uint64_t new_phys) {
    // Table descriptor: bits[1:0]=11 only — AF (bit 10) is a block/page
    // attribute and RES0 in table descriptors.
    return new_phys | PAGE_PRESENT | PAGE_TABLE;
}

uint64_t VMM::vmm_arch_encode_kernel_leaf(uint64_t phys, bool user) {
    // L3 page descriptors are bits[1:0]=11 (PAGE_TABLE set). AttrIndx=1
    // selects the Normal WB MAIR attribute; Device memory is never
    // executable and must not back user/kernel code.
    uint64_t flags = PAGE_PRESENT | PAGE_TABLE | PAGE_AF | PAGE_ATTR_NORMAL;
    if (user)
        flags |= PAGE_AP_USER | PAGE_PXN;
    else
        flags |= PAGE_UXN | PAGE_PXN;
    return phys | flags;
}

uint64_t VMM::vmm_arch_encode_map_leaf(uint64_t phys, bool user,
                                        bool executable) {
    // Same base shape as the kernel leaf; the NX policy adds UXN.
    uint64_t out = PAGE_PRESENT | PAGE_TABLE | PAGE_AF | PAGE_ATTR_NORMAL;
    if (user)
        out |= PAGE_AP_USER | PAGE_PXN;
    else
        out |= PAGE_UXN | PAGE_PXN;
    if (!executable)
        out |= PAGE_UXN; // UXN — not executable at EL0
    return phys | out;
}

uint64_t VMM::vmm_arch_encode_ro(uint64_t leaf, bool executable) {
    (void)executable;
    // AP[1:0] 01 -> 11 (EL1-RW/EL0-RW -> EL1-RO/EL0-RO). EL0 accessibility
    // preserved; UXN/PXN already set by the 5-arg map per @p executable.
    return leaf | PAGE_AP_RO;
}

bool VMM::vmm_arch_leaf_is_ro(uint64_t leaf, bool *executable_out) {
    bool ro = (leaf & (PAGE_AP_USER | PAGE_AP_RO)) ==
              (PAGE_AP_USER | PAGE_AP_RO);
    if (executable_out != nullptr)
        *executable_out = (leaf & PAGE_UXN) == 0;
    return ro;
}

bool VMM::vmm_arch_leaf_is_executable(uint64_t leaf) {
    return (leaf & PAGE_UXN) == 0;
}

uint64_t VMM::vmm_arch_block_base(uint64_t entry, bool huge) {
    (void)huge; // frame masks already select the level geometry below
    return entry & (huge ? PAGE_HUGE_FRAME_MASK : PAGE_FRAME_MASK);
}

unsigned VMM::vmm_arch_walk_levels() { return 4; }

void VMM::vmm_arch_walk_indices(uint64_t virt_addr, size_t idx[4]) {
    idx[0] = arch::ArchPageTable::pml4_index(virt_addr);
    idx[1] = arch::ArchPageTable::pdpt_index(virt_addr);
    idx[2] = arch::ArchPageTable::pd_index(virt_addr);
    idx[3] = arch::ArchPageTable::pt_index(virt_addr);
}

#endif // CONFIG_ARCH_AARCH64

} // namespace kernel
