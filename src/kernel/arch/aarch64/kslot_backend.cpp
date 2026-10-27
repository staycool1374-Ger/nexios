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

/// @file kslot_backend.cpp (aarch64)
/// @brief #434 P1: aa64 kslot window backend — TTBR1 window wiring.
/// Window: CONFIG_KSTACK_WINDOW_BASE (L0 idx 288), 16 MiB = 1xL1 + 1xL2 +
/// 8xL3 (4 KiB leaves, guard-precise). Clone inherits L0[288] by value via
/// the shared clone_kernel_pml4 kernel-half copy, so no clone change.
/// Build-selected via arch/<arch>/ discovery; no CONFIG_ARCH guard here.

#include <kernel/task/kslot_backend.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/page_table.hpp>
#include <kernel/nexios_config.h>
#include <constants.hpp>
#include <string.hpp>
#include <logger.hpp>

namespace kslot_backend {

// Layout: s_tables[0]=L1 (for L0[288]), s_tables[1]=L2 (for L1[0]),
// s_tables[2..9]=L3 (for L2[0..7]). Total 10 pages, mirrors x86 1+1+8.
static uint64_t s_tables[10] = {};
// #434 P1-C1: all-or-nothing re-entry (mirrors x86 backend). Partial wiring
// completes on retry; usable only when fully wired. Failure is loud-once.
static bool s_wired_complete = false;
static bool s_wired_warned = false;

static void warn_wire_failed_once() {
    if (!s_wired_warned) {
        s_wired_warned = true;
        kernel::Logger::warn("kslot: window wiring FAILED — callers take "
                             "the exhaustion-0 fallback, no guard page");
    }
}

static constexpr uint64_t DESC_VALID = 1ULL << 0;
static constexpr uint64_t DESC_TABLE = 1ULL << 1;
static constexpr uint64_t DESC_AF = 1ULL << 10;
static constexpr uint64_t DESC_SH_INNER = 3ULL << 8;
static constexpr uint64_t DESC_NS = 1ULL << 5;
static constexpr uint64_t ATTR_NORMAL = 1ULL << 2;
static constexpr uint64_t UXN = 1ULL << 54;
static constexpr uint64_t PXN = 1ULL << 53;
static constexpr uint64_t PHYS_MASK = ((1ULL << 48) - 1) & ~0xFFFULL;
// 48-bit VA mask (named constant: the style gate misparses a raw hex mask
// in an expression as a declaration).
static constexpr uint64_t VA48_MASK = 0x0000FFFFFFFFFFFFULL;

bool wire_window_tables() {
    if (s_wired_complete)
        return true;
    const uint64_t base_48 = CONFIG_KSTACK_WINDOW_BASE & VA48_MASK;
    const size_t l0_idx = (base_48 >> 39) & 0x1FF;
    const size_t l1_idx = (base_48 >> 30) & 0x1FF;
    const size_t l2_idx = (base_48 >> 21) & 0x1FF;
    // P1 expects the x86-compatible base: L0 288, L1 0, L2 0. A different
    // configured base still wires correctly (indices derived above), but
    // the compat checklist pins 288 for the default config.
    (void)l1_idx;
    (void)l2_idx;

    auto *l0 = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (kernel::VMM::get_kernel_pml4() & ~0xFFFULL));

    uint64_t l1_phys = 0;
    if (!(l0[l0_idx] & DESC_VALID)) {
        l1_phys = kernel::PMM::alloc_page_table();
        if (!l1_phys) {
            warn_wire_failed_once();
            return false;
        }
        __builtin_memset(
            reinterpret_cast<void *>(arch::HHDM_OFFSET + l1_phys), 0, 4096);
        l0[l0_idx] = l1_phys | DESC_VALID | DESC_TABLE;
    } else {
        l1_phys = l0[l0_idx] & PHYS_MASK;
    }
    s_tables[0] = l1_phys;

    auto *l1 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l1_phys);
    // Window is 16 MiB under one L1 entry (L1[0] for the default base).
    const size_t win_l1 = (base_48 >> 30) & 0x1FF;
    uint64_t l2_phys = 0;
    if (!(l1[win_l1] & DESC_VALID)) {
        l2_phys = kernel::PMM::alloc_page_table();
        if (!l2_phys) {
            warn_wire_failed_once();
            return false;
        }
        __builtin_memset(
            reinterpret_cast<void *>(arch::HHDM_OFFSET + l2_phys), 0, 4096);
        l1[win_l1] = l2_phys | DESC_VALID | DESC_TABLE;
    } else {
        l2_phys = l1[win_l1] & PHYS_MASK;
    }
    s_tables[1] = l2_phys;

    auto *l2 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l2_phys);
    const size_t win_l2 = (base_48 >> 21) & 0x1FF;
    for (size_t i = 0; i < 8; ++i) {
        if (!(l2[win_l2 + i] & DESC_VALID)) {
            uint64_t l3_phys = kernel::PMM::alloc_page_table();
            if (!l3_phys) {
                warn_wire_failed_once();
                return false;
            }
            __builtin_memset(
                reinterpret_cast<void *>(arch::HHDM_OFFSET + l3_phys), 0,
                4096);
            l2[win_l2 + i] = l3_phys | DESC_VALID | DESC_TABLE;
            s_tables[2 + i] = l3_phys;
        } else {
            s_tables[2 + i] = l2[win_l2 + i] & PHYS_MASK;
        }
    }

    arch::ArchPageTable::tlb_flush_all();
    s_wired_complete = true;
    return true;
}

static uint64_t *l3_for(uint64_t virt) {
    if (virt < CONFIG_KSTACK_WINDOW_BASE ||
        virt >= CONFIG_KSTACK_WINDOW_BASE + CONFIG_KSTACK_WINDOW_SIZE)
        return nullptr;
    uint64_t offset = virt - CONFIG_KSTACK_WINDOW_BASE;
    // 16 MiB window: L2 slot (0..7) + L3 index. Base is L2-aligned for the
    // default config; derive relative to the window start generally.
    const uint64_t base_48 = CONFIG_KSTACK_WINDOW_BASE & VA48_MASK;
    const size_t win_l2 = (base_48 >> 21) & 0x1FF;
    size_t l2_slot = (offset >> 21) & 0x1FF;
    // Window spans win_l2..win_l2+7; reject anything outside.
    if (l2_slot >= 8)
        return nullptr;
    (void)win_l2;
    uint64_t l3_phys = s_tables[2 + l2_slot];
    if (!l3_phys)
        return nullptr;
    return reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l3_phys);
}

void map_window_page(uint64_t virt, uint64_t phys) {
    uint64_t *l3 = l3_for(virt);
    if (!l3)
        return;
    size_t l3_idx = (virt >> 12) & 0x1FF;
    // Kernel stack data: RW, Normal WB, inner-shareable, AF, UXN+PXN
    // (EL1/EL0 no-exec — matches attr_from_flags kernel-data shape).
    const uint64_t leaf = phys | DESC_VALID | DESC_TABLE | DESC_AF |
                          DESC_SH_INNER | DESC_NS | ATTR_NORMAL | UXN | PXN;
    l3[l3_idx] = leaf;
    arch::ArchPageTable::tlb_flush(virt);
}

void unmap_window_page(uint64_t virt) {
    uint64_t *l3 = l3_for(virt);
    if (!l3)
        return;
    size_t l3_idx = (virt >> 12) & 0x1FF;
    l3[l3_idx] = 0;
    arch::ArchPageTable::tlb_flush(virt);
}

void flush_window_va(uint64_t virt) {
    arch::ArchPageTable::tlb_flush(virt);
}

size_t snapshot_bytes() { return 10 * 4096; }

void snapshot_capture(uint8_t *dst) {
    for (size_t i = 0; i < 10; ++i) {
        if (s_tables[i]) {
            __builtin_memcpy(dst + i * 4096,
                             reinterpret_cast<const void *>(
                                 arch::HHDM_OFFSET + s_tables[i]),
                             4096);
        } else {
            __builtin_memset(dst + i * 4096, 0, 4096);
        }
    }
}

void snapshot_restore(const uint8_t *src) {
    for (size_t i = 0; i < 10; ++i) {
        if (s_tables[i]) {
            __builtin_memcpy(
                reinterpret_cast<void *>(arch::HHDM_OFFSET + s_tables[i]),
                src + i * 4096, 4096);
        }
    }
    for (uint64_t off = 0; off < CONFIG_KSTACK_WINDOW_SIZE; off += 4096)
        arch::ArchPageTable::tlb_flush(CONFIG_KSTACK_WINDOW_BASE + off);
}

} // namespace kslot_backend
