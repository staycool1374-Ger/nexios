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

#if defined(CONFIG_ARCH_X86_64)

// Issue #363 P2: x86_64 leaf codec, bit-identical to the pre-split inline
// encodings. Shared walk skeletons (VMM policy) call these.

bool VMM::vmm_arch_is_table(uint64_t entry) {
    return (entry & PAGE_HUGE) == 0;
}

uint64_t VMM::vmm_arch_table_child(uint64_t entry) {
    return entry & PAGE_TABLE_ADDR_MASK;
}

bool VMM::vmm_arch_split_needed(uint64_t entry) {
    return (entry & PAGE_HUGE) != 0;
}

void VMM::vmm_arch_split_fill(uint64_t *new_table, uint64_t src_entry) {
    uint64_t huge_base = src_entry & kHuge2MBaseMask;
    uint64_t base_flags = src_entry & (PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
    for (size_t i = 0; i < 512; ++i) {
        new_table[i] = (huge_base + i * 0x1000) | base_flags;
    }
}

uint64_t VMM::vmm_arch_table_link(uint64_t new_phys) {
    return new_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
}

uint64_t VMM::vmm_arch_encode_kernel_leaf(uint64_t phys, bool user) {
    // Issue #376 slice 1: runtime kernel-half maps are data (MMIO, DMA,
    // ACPI, framebuffer — enumerated audit, no X consumer), so they map
    // non-executable.  NXE is enabled at boot (boot.asm LME|NXE).  Boot-
    // established text/HHDM mappings stay RWX (epic remainder, see #376).
    uint64_t flags = PAGE_PRESENT | PAGE_WRITE | arch::ArchPageTable::NX;
    if (user)
        flags |= PAGE_USER;
    return phys | flags;
}

uint64_t VMM::vmm_arch_encode_map_leaf(uint64_t phys, bool user,
                                        bool executable) {
    uint64_t flags = PAGE_PRESENT | PAGE_WRITE;
    if (user)
        flags |= PAGE_USER;
    if (!executable)
        flags |= arch::ArchPageTable::NX; // NX — not executable
    return phys | flags;
}

uint64_t VMM::vmm_arch_encode_ro(uint64_t leaf, bool executable) {
    leaf &= ~PAGE_WRITE;
    if (!executable)
        leaf |= arch::ArchPageTable::NX; // NX (already set by 5-arg map)
    return leaf;
}

bool VMM::vmm_arch_leaf_is_ro(uint64_t leaf, bool *executable_out) {
    bool ro = ((leaf & PAGE_PRESENT) != 0) && ((leaf & PAGE_WRITE) == 0);
    if (executable_out != nullptr)
        *executable_out = (leaf & arch::ArchPageTable::NX) == 0;
    return ro;
}

bool VMM::vmm_arch_leaf_is_executable(uint64_t leaf) {
    return (leaf & arch::ArchPageTable::NX) == 0;
}

uint64_t VMM::vmm_arch_block_base(uint64_t entry, bool huge) {
    return entry & (huge ? PAGE_HUGE_FRAME_MASK : PAGE_FRAME_MASK);
}

unsigned VMM::vmm_arch_walk_levels() { return 4; }

void VMM::vmm_arch_walk_indices(uint64_t virt_addr, size_t idx[4]) {
    idx[0] = arch::ArchPageTable::pml4_index(virt_addr);
    idx[1] = arch::ArchPageTable::pdpt_index(virt_addr);
    idx[2] = arch::ArchPageTable::pd_index(virt_addr);
    idx[3] = arch::ArchPageTable::pt_index(virt_addr);
}

#endif // CONFIG_ARCH_X86_64

} // namespace kernel
