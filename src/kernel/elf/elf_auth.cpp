/*
 * NexIOS RTOS — ELF read-only image authenticity (issue #46)
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

/// @file elf_auth.cpp
/// @brief Read-only segment CRC baseline + re-verify primitives (issue #46).
///
/// Coverage rule: every PT_LOAD segment WITHOUT PF_W, byte range
/// [vaddr, vaddr+filesz).  Writable segments mutate by design (GOT, data,
/// stack), the BSS tail carries no file signal, headers are never mapped.
/// Both the loader baseline scan and the idle re-verify slice use these
/// helpers, so the compared values are self-consistent by construction.

#include <kernel/elf/elf.hpp>
#include <kernel/elf/elf_shared.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/vmm.hpp>
#include <crc32.hpp>

namespace kernel {
namespace elf {

uint64_t collect_ro_ranges(const uint8_t *phdr_image, uint64_t phdr_buf_off,
                           uint16_t phnum, uint64_t phentsize,
                           uint64_t file_size, TaskControlBlock::RoRange *out,
                           uint64_t max_ranges) noexcept {
    uint64_t count = 0;
    for (uint16_t i = 0; i < phnum; ++i) {
        auto *phdr = reinterpret_cast<const ELF64ProgramHeader *>(
            phdr_image + phdr_buf_off + static_cast<uint64_t>(i) * phentsize);
        if (!phdr || phdr->type != PT_LOAD)
            continue;
        if (!validate_segment(phdr, file_size))
            continue;
        if (phdr->flags & PF_W)
            continue;
        if (phdr->filesz == 0)
            continue;
        if (count >= max_ranges)
            return static_cast<uint64_t>(-1); // overflow: fail closed
        out[count].vaddr = phdr->vaddr;
        out[count].len = phdr->filesz;
        ++count;
    }
    return count;
}
bool crc_user_range(uint64_t pml4, uint64_t vaddr, uint64_t len,
                    uint32_t &acc) noexcept {

    uint64_t off = 0;
    while (off < len) {
        uint64_t va = vaddr + off;
        // NOTE: virt_to_phys_in_pml4 returns frame + page-offset already —
        // do NOT add the offset again (that shifts the whole scan).
        uint64_t phys = VMM::virt_to_phys_in_pml4(va, pml4);
        if (phys == 0)
            return false; // unmapped mid-range: fail closed
        uint64_t page_off = va & (arch::PAGE_SIZE - 1);
        uint64_t chunk = arch::PAGE_SIZE - page_off;
        if (chunk > len - off)
            chunk = len - off;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        const uint8_t *bytes =
            reinterpret_cast<const uint8_t *>(arch::HHDM_OFFSET + phys);
        acc = CRC32::update(acc, bytes, chunk);
        off += chunk;
    }
    return true;
}

/// @brief Page-align a virtual address down (mirrors elf_loader.cpp).
static inline uint64_t page_align_down(uint64_t addr) {
    return addr & ~(arch::PAGE_SIZE - 1);
}

bool text_canary_slot(const uint8_t *phdr_image, uint64_t phdr_buf_off,
                      uint16_t phnum, uint64_t phentsize, uint64_t file_size,
                      uint64_t *slot_out) noexcept {
    // Mirrors install_segment_canaries (elf.cpp) exactly, including the
    // !text_base quirk: the first PF_X PT_LOAD claims the slot.
    uint64_t text_base = 0;
    for (uint16_t i = 0; i < phnum; ++i) {
        auto *phdr = reinterpret_cast<const ELF64ProgramHeader *>(
            phdr_image + phdr_buf_off + static_cast<uint64_t>(i) * phentsize);
        if (!phdr || phdr->type != PT_LOAD)
            continue;
        if (!validate_segment(phdr, file_size))
            continue;
        if ((phdr->flags & PF_X) && !text_base) {
            text_base = page_align_down(phdr->vaddr);
            break;
        }
    }
    if (text_base == 0)
        return false;
    *slot_out = text_base;
    return true;
}

bool snapshot_ro_baseline(uint64_t pml4, const uint8_t *phdr_image,
                          uint64_t phdr_buf_off, uint16_t phnum,
                          uint64_t phentsize, uint64_t file_size,
                          bool has_dynamic, uint32_t load_crc,
                          uint64_t canary_slot,
                          TaskControlBlock *tcb) noexcept {
    if (!tcb)
        return false;
    TaskControlBlock::RoRange ranges[TaskControlBlock::kMaxRoRanges];
    uint64_t n = collect_ro_ranges(phdr_image, phdr_buf_off, phnum, phentsize,
                                   file_size, ranges,
                                   TaskControlBlock::kMaxRoRanges);
    if (n == static_cast<uint64_t>(-1))
        return false; // more RO ranges than the TCB snapshot holds
    // Carve the TEXT before-canary slot out (the canary subsystem owns
    // those 8 bytes and checks them itself — they differ pre/post
    // finalize, so neither the baseline scan nor the chunk accumulation
    // covers them).  Standard interval clipping per range.
    if (canary_slot != 0) {
        const uint64_t cs = canary_slot;
        const uint64_t ce = canary_slot + 8;
        if (ce <= cs)
            return false; // address wrap: fail closed
        TaskControlBlock::RoRange carved[TaskControlBlock::kMaxRoRanges];
        uint64_t m = 0;
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t vs = ranges[i].vaddr;
            uint64_t ve = vs + ranges[i].len;
            if (vs < cs && cs < ve) {
                // Head portion before the slot.
                if (m >= TaskControlBlock::kMaxRoRanges)
                    return false;
                carved[m].vaddr = vs;
                carved[m].len = cs - vs;
                ++m;
            } else if (cs <= vs && vs < ce) {
                // Range starts inside the slot: head fully covered.
            } else {
                // No overlap (or slot strictly before): keep whole.
                if (m >= TaskControlBlock::kMaxRoRanges)
                    return false;
                carved[m] = ranges[i];
                ++m;
                continue;
            }
            if (ce < ve) {
                // Tail portion after the slot.
                if (m >= TaskControlBlock::kMaxRoRanges)
                    return false;
                carved[m].vaddr = ce;
                carved[m].len = ve - ce;
                ++m;
            }
        }
        for (uint64_t i = 0; i < m; ++i)
            ranges[i] = carved[i];
        n = m;
    }
    uint64_t total = 0;
    for (uint64_t i = 0; i < n; ++i) {
        total += ranges[i].len;
        if (total > kMaxRoScanBytes)
            return false; // oversized image: fail closed, not unbounded
    }
    uint32_t acc = CRC32::INITIAL;
    for (uint64_t i = 0; i < n; ++i) {
        if (!crc_user_range(pml4, ranges[i].vaddr, ranges[i].len, acc))
            return false;
    }
    uint32_t baseline = CRC32::finalize(acc);
    if (!has_dynamic) {
        // Static image: no relocation touched the bytes, so the post-map
        // scan must equal the incremental chunk-loop CRC.  A delta means
        // the copy path corrupted data — fail the load, never publish.
        if (baseline != CRC32::finalize(load_crc))
            return false;
    }
    // Dynamic image: relocations legitimately rewrote bytes; the post-map
    // scan is the baseline the idle slice will reproduce.
    tcb->ro_crc32_ = baseline;
    tcb->ro_seg_count_ = n;
    for (uint64_t i = 0; i < n; ++i)
        tcb->ro_ranges_[i] = ranges[i];
    // Single-writer handoff (task.hpp documents the invariant): the loader
    // owns these until completed_tcb_ publication; the idle slice owns them
    // after.  Plain stores; the lock_ → take_completed → add_task mutex
    // chain orders them.
    tcb->ro_verify_off_ = 0;
    tcb->ro_verify_acc_ = CRC32::INITIAL;
    tcb->ro_verify_state_ = TaskControlBlock::RoVerifyState::UNVERIFIED;
    return true;
}

} // namespace elf
} // namespace kernel
