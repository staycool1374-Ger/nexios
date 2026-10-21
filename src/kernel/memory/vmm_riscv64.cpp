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

#if defined(CONFIG_ARCH_RISCV64)

// Issue #363 P2: Sv39 leaf codec, bit-identical to the pre-split inline
// encodings. PTEs carry PPN<<10 (issue #206: raw masking is x86-domain),
// so every phys codec below goes through sv39_pte_phys/sv39_phys_pte.

bool VMM::vmm_arch_is_table(uint64_t entry) {
    constexpr uint64_t k_rwx = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
    return (entry & k_rwx) == 0;
}

uint64_t VMM::vmm_arch_table_child(uint64_t entry) {
    return VMM::sv39_pte_phys(entry);
}

bool VMM::vmm_arch_split_needed(uint64_t entry) {
    // Sv39 is the INVERSE of the aarch64/x86 test: a V-only entry is a
    // TABLE pointer (descend), while V+R|W|X is a block LEAF (split).
    return (entry & (PAGE_READ | PAGE_WRITE | PAGE_EXEC)) != 0;
}

void VMM::vmm_arch_split_fill(uint64_t *new_table, uint64_t src_entry) {
    uint64_t huge_base = VMM::sv39_pte_phys(src_entry) & kHuge2MBaseMask;
    uint64_t base_flags = src_entry & (PAGE_PRESENT | PAGE_READ | PAGE_WRITE |
                                       PAGE_EXEC | PAGE_USER | PAGE_GLOBAL |
                                       PAGE_ACCESSED | PAGE_DIRTY);
    for (size_t i = 0; i < 512; ++i) {
        new_table[i] = VMM::sv39_phys_pte(huge_base + i * 0x1000) |
                       base_flags | PAGE_ACCESSED | PAGE_DIRTY;
    }
}

uint64_t VMM::vmm_arch_table_link(uint64_t new_phys) {
    // Table entry: V=1, no RWX = points to next level table.
    // PPN-shifted store (raw phys would decode 4x too high).
    return VMM::sv39_phys_pte(new_phys) | PAGE_PRESENT;
}

uint64_t VMM::vmm_arch_encode_kernel_leaf(uint64_t phys, bool user) {
    uint64_t flags = PAGE_PRESENT | PAGE_READ | PAGE_WRITE;
    if (user)
        flags |= PAGE_USER;
    flags |= PAGE_ACCESSED | PAGE_DIRTY;
    // PPN-shifted store (issue #206: raw phys decodes 4x too high).
    return VMM::sv39_phys_pte(phys) | flags;
}

uint64_t VMM::vmm_arch_encode_map_leaf(uint64_t phys, bool user,
                                        bool executable) {
    uint64_t flags = PAGE_PRESENT | PAGE_READ | PAGE_WRITE;
    if (executable)
        flags |= PAGE_EXEC;
    if (user)
        flags |= PAGE_USER;
    flags |= PAGE_ACCESSED | PAGE_DIRTY;
    // PPN-shifted store (issue #206: raw phys decodes 4x too high).
    return VMM::sv39_phys_pte(phys) | flags;
}

uint64_t VMM::vmm_arch_encode_ro(uint64_t leaf, bool executable) {
    (void)executable;
    // Sv39: R stays, W clears (R-only is a valid leaf encoding).
    return leaf & ~PAGE_WRITE;
}

uint64_t VMM::vmm_arch_kernel_text_exec(uint64_t leaf) {
    // Issue #376: kernel text stays executable — set X cleared by the
    // data-default 5-arg map (W was already cleared by encode_ro).
    // Kernel-half only.
    return leaf | PAGE_EXEC;
}

bool VMM::vmm_arch_leaf_is_ro(uint64_t leaf, bool *executable_out) {
    bool ro = ((leaf & PAGE_READ) != 0) && ((leaf & PAGE_WRITE) == 0);
    if (executable_out != nullptr)
        *executable_out = (leaf & PAGE_EXEC) != 0;
    return ro;
}

bool VMM::vmm_arch_leaf_is_executable(uint64_t leaf) {
    return (leaf & PAGE_EXEC) != 0;
}

uint64_t VMM::vmm_arch_block_base(uint64_t entry, bool huge) {
    (void)huge; // Sv39 codec is level-agnostic (PPN<<10 either way)
    return VMM::sv39_pte_phys(entry);
}

uint64_t VMM::vmm_arch_leaf_flags(uint64_t entry) {
    // Low-10 flag bits plus high reserved bits [63:54] (future
    // PBMT/NAPOT/custom) — never any PPN bit.  Matches the split-fill
    // preservation in ArchPageTable::split_l0_block.
    return (entry & 0x3FFULL) | (entry & (0x3FFULL << 54));
}

unsigned VMM::vmm_arch_walk_levels() { return 3; }

void VMM::vmm_arch_walk_indices(uint64_t virt_addr, size_t idx[4]) {
    idx[0] = (virt_addr & VMM::L0_MASK) >> VMM::L0_SHIFT;
    idx[1] = (virt_addr & VMM::L1_MASK) >> VMM::L1_SHIFT;
    idx[2] = (virt_addr & VMM::L2_MASK) >> VMM::L2_SHIFT;
    idx[3] = 0;
}

#endif // CONFIG_ARCH_RISCV64

} // namespace kernel
