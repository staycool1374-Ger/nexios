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

/// @file kslot_backend.cpp (x86_64)
/// @brief #434 P1: x86_64 kslot window backend — moved verbatim from
/// task.cpp init_kstack_window/map_kstack_page/unmap_kstack_page (plus
/// snapshot PT bytes + flush). Build-selected via arch/<arch>/ discovery;
/// no CONFIG_ARCH guard in this TU is needed (it only compiles on x86_64).

#include <kernel/task/kslot_backend.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/nexios_config.h>
#include <constants.hpp>
#include <logger.hpp>

namespace kslot_backend {

static uint64_t s_kstack_pt_pages[8] = {};
// #434 P1-C1: all-or-nothing re-entry. Partial wiring (OOM mid-loop) leaves
// adopted entries in place; a retry completes the missing tables. The window
// is reported usable only when fully wired.
static bool s_wired_complete = false;
static bool s_wired_warned = false;

static void warn_wire_failed_once() {
    if (!s_wired_warned) {
        s_wired_warned = true;
        kernel::Logger::warn("kslot: window wiring FAILED — callers take "
                             "the exhaustion-0 fallback, no guard page");
    }
}

bool wire_window_tables() {
    if (s_wired_complete)
        return true;
    const uint64_t addr_mask = 0x0000FFFFFFFFFFFFULL;
    uint64_t base_48 = CONFIG_KSTACK_WINDOW_BASE & addr_mask;
    unsigned pml4_idx = (base_48 >> 39) & 0x1FF;
    unsigned pdpt_idx = (base_48 >> 30) & 0x1FF;
    unsigned pd_idx = (base_48 >> 21) & 0x1FF;

    auto *pml4 = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (kernel::VMM::get_kernel_pml4() & ~0xFFFULL));
    constexpr uint64_t P = 1ULL << 0;
    constexpr uint64_t W = 1ULL << 1;

    uint64_t pdpt_phys = 0;
    if (!(pml4[pml4_idx] & P)) {
        pdpt_phys = kernel::PMM::alloc_page_table();
        if (!pdpt_phys) {
            warn_wire_failed_once();
            return false;
        }
        auto *pdpt =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pdpt_phys);
        __builtin_memset(pdpt, 0, 4096);
        pml4[pml4_idx] = pdpt_phys | P | W;
    } else {
        pdpt_phys = pml4[pml4_idx] & ~0xFFFULL;
    }

    auto *pdpt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pdpt_phys);
    uint64_t pd_phys = 0;
    if (!(pdpt[pdpt_idx] & P)) {
        pd_phys = kernel::PMM::alloc_page_table();
        if (!pd_phys) {
            warn_wire_failed_once();
            return false;
        }
        auto *pd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pd_phys);
        __builtin_memset(pd, 0, 4096);
        pdpt[pdpt_idx] = pd_phys | P | W;
    } else {
        pd_phys = pdpt[pdpt_idx] & ~0xFFFULL;
    }

    auto *pd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pd_phys);
    for (unsigned i = 0; i < 8; ++i) {
        if (!(pd[pd_idx + i] & P)) {
            s_kstack_pt_pages[i] = kernel::PMM::alloc_page_table();
            if (!s_kstack_pt_pages[i]) {
                warn_wire_failed_once();
                return false;
            }
            auto *pt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                    s_kstack_pt_pages[i]);
            __builtin_memset(pt, 0, 4096);
            pd[pd_idx + i] = s_kstack_pt_pages[i] | P | W;
        } else {
            s_kstack_pt_pages[i] = pd[pd_idx + i] & ~0xFFFULL;
        }
    }

    uint64_t cr3 = 0;
    asm volatile("mov %%cr3, %0" : "=r"(cr3) : : "memory");
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    s_wired_complete = true;
    return true;
}

void map_window_page(uint64_t virt, uint64_t phys) {
    if (virt < CONFIG_KSTACK_WINDOW_BASE ||
        virt >= CONFIG_KSTACK_WINDOW_BASE + CONFIG_KSTACK_WINDOW_SIZE)
        return;
    uint64_t offset = virt - CONFIG_KSTACK_WINDOW_BASE;
    unsigned pt_idx = offset / (512 * 4096);
    unsigned entry = (offset / 4096) & 0x1FF;
    if (pt_idx >= 8)
        return;
    constexpr uint64_t P = 1ULL << 0;
    constexpr uint64_t W = 1ULL << 1;
    // Issue #376 (boot-text W^X): kernel stacks are data — RW+NX.
    constexpr uint64_t NX = 1ULL << 63;
    const uint64_t leaf_flags = P | W | NX;
    auto *pt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            s_kstack_pt_pages[pt_idx]);
    pt[entry] = phys | leaf_flags;
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void unmap_window_page(uint64_t virt) {
    if (virt < CONFIG_KSTACK_WINDOW_BASE ||
        virt >= CONFIG_KSTACK_WINDOW_BASE + CONFIG_KSTACK_WINDOW_SIZE)
        return;
    uint64_t offset = virt - CONFIG_KSTACK_WINDOW_BASE;
    unsigned pt_idx = offset / (512 * 4096);
    unsigned entry = (offset / 4096) & 0x1FF;
    if (pt_idx >= 8)
        return;
    auto *pt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            s_kstack_pt_pages[pt_idx]);
    pt[entry] = 0;
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void flush_window_va(uint64_t virt) {
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

size_t snapshot_bytes() { return 8 * 4096; }

void snapshot_capture(uint8_t *dst) {
    for (unsigned i = 0; i < 8; ++i) {
        if (s_kstack_pt_pages[i]) {
            __builtin_memcpy(dst + i * 4096,
                             reinterpret_cast<const void *>(
                                 arch::HHDM_OFFSET + s_kstack_pt_pages[i]),
                             4096);
        } else {
            __builtin_memset(dst + i * 4096, 0, 4096);
        }
    }
}

void snapshot_restore(const uint8_t *src) {
    for (unsigned i = 0; i < 8; ++i) {
        if (s_kstack_pt_pages[i]) {
            __builtin_memcpy(
                reinterpret_cast<void *>(arch::HHDM_OFFSET +
                                         s_kstack_pt_pages[i]),
                src + i * 4096, 4096);
            uint64_t base_va = CONFIG_KSTACK_WINDOW_BASE +
                               static_cast<uint64_t>(i) * 512 * 4096;
            for (unsigned e = 0; e < 512; ++e) {
                asm volatile("invlpg (%0)"
                             :
                             : "r"(base_va + e * 4096)
                             : "memory");
            }
        }
    }
}

} // namespace kslot_backend
