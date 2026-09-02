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

/// @file gdt.cpp
/// @brief Global Descriptor Table — sets up segmentation and Task State Segment
/// (TSS) for privilege levels and IST.

#include <kernel/arch/gdt.hpp>
#include <types.hpp>

// Freestanding build has no <cstddef>; provide offsetof via the compiler
// builtin (same pattern as tcb_write_log.hpp).
#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

namespace arch {

/// @brief Dedicated 4 KiB stack for the double-fault handler (IST1).
/// @note Allocated in .bss (zero-initialised). Used by the CPU when vector 8
/// fires.
static uint8_t df_stack[4096] __attribute__((aligned(16)));

/// @brief GDT entry table.
GDTEntry GDT::entries_[NUM_ENTRIES] = {};
/// @brief Task State Segment plus I/O permission bitmap (single global block;
/// per-task bitmap content is swapped by arch::iopb_*).
TSSBlock GDT::tss_block_ = {};
/// @brief GDT pseudo-descriptor (base + limit) loaded by LGDT.
GDTDescriptor GDT::desc_ = {};

/// @brief Construct a GDT entry from its raw fields.
/// @param base Segment base address.
/// @param limit Segment limit.
/// @param access Access rights byte.
/// @param gran Granularity and flags byte.
/// @return A populated GDTEntry structure.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static GDTEntry make_entry(uint32_t base, uint32_t limit, uint8_t access,
                           uint8_t gran) {
    GDTEntry e = {};
    e.limit_low = limit & 0xFFFF;
    e.base_low = base & 0xFFFF;
    e.base_mid = (base >> 16) & 0xFF;
    e.access = access;
    e.granularity = gran | ((limit >> 16) & 0x0F);
    e.base_high = (base >> 24) & 0xFF;
    return e;
}

/// @brief Initialise the GDT with kernel code/data, user code/data, and TSS
/// entries. Sets up IST1 for the double-fault handler stack.
void GDT::init() {
    entries_[0] = {};

    entries_[GDT_CODE / 8] = make_entry(0, 0, 0x9A, 0x20);
    entries_[GDT_DATA / 8] = make_entry(0, 0, 0x92, 0x00);

    entries_[GDT_USER_CODE / 8] = make_entry(0, 0, 0xFA, 0x20);
    entries_[GDT_USER_DATA / 8] = make_entry(0, 0, 0xF2, 0x00);

    uint64_t tss_base = reinterpret_cast<uint64_t>(&tss_block_);
    uint32_t tss_limit = sizeof(TSSBlock) - 1;

    GDTEntry tss_low = {};
    tss_low.limit_low = tss_limit & 0xFFFF;
    tss_low.base_low = tss_base & 0xFFFF;
    tss_low.base_mid = (tss_base >> 16) & 0xFF;
    tss_low.access = 0x89;
    tss_low.granularity = (tss_limit >> 16) & 0x0F;
    tss_low.base_high = (tss_base >> 24) & 0xFF;
    entries_[GDT_TSS / 8] = tss_low;

    uint64_t *tss_high =
        reinterpret_cast<uint64_t *>(&entries_[GDT_TSS / 8 + 1]);
    *tss_high = tss_base >> 32;

    desc_.limit = sizeof(entries_) - 1;
    desc_.base = reinterpret_cast<uint64_t>(entries_);

    // Set up IST1 for double-fault handler (vector 8)
    tss_block_.tss.ist1 = reinterpret_cast<uint64_t>(df_stack + sizeof(df_stack));

    // I/O permission bitmap: default-deny (all-1s) + 0xFF terminator.
    // The bitmap lives inside the TSS segment, so the descriptor limit is
    // sizeof(TSSBlock)-1 and iopb_offset is the bitmap's position.
    for (size_t i = 0; i < sizeof(tss_block_.iopb); ++i)
        tss_block_.iopb[i] = 0xFF;
    tss_block_.iopb_terminator = 0xFF;
    tss_block_.tss.iopb_offset =
        static_cast<uint16_t>(offsetof(TSSBlock, iopb));
}

/// @brief Load the GDT and TSS into the CPU.
/// Executes LGDT, reloads data segments, and loads the TSS via LTR.
void GDT::load() {
    asm volatile("lgdt %0" : : "m"(desc_));
    asm volatile("mov %0, %%ds\n"
                 "mov %0, %%es\n"
                 "mov %0, %%fs\n"
                 "mov %0, %%gs\n"
                 "mov %0, %%ss\n"
                 :
                 : "r"((uint16_t)GDT_DATA));
    uint16_t tss_sel = GDT_TSS;
    asm volatile("ltr %0" : : "r"(tss_sel));
}

/// @brief Set the RSP0 field in the TSS (kernel stack pointer for ring-0
/// entry).
/// @param rsp The stack pointer to use when transitioning from ring-3 to
/// ring-0.
void GDT::set_tss_rsp0(uint64_t rsp) {
    tss_block_.tss.rsp0 = rsp;
}

/// @brief Load an 8 KiB I/O permission bitmap into the TSS.
void GDT::iopb_load(const uint8_t *bitmap) {
    for (size_t i = 0; i < sizeof(tss_block_.iopb); ++i)
        tss_block_.iopb[i] = bitmap[i];
}

/// @brief Set the I/O permission bitmap to all-1s (default-deny).
void GDT::iopb_mask_all() {
    for (size_t i = 0; i < sizeof(tss_block_.iopb); ++i)
        tss_block_.iopb[i] = 0xFF;
}

/// @brief Pointer to the TSS I/O permission bitmap.
uint8_t *GDT::iopb_bitmap() {
    return tss_block_.iopb;
}

/// @brief IOPB offset stored in the TSS.
uint16_t GDT::tss_iopb_offset() {
    return tss_block_.tss.iopb_offset;
}

/// @brief TSS descriptor limit (reconstructed from the GDT entry).
uint16_t GDT::tss_descriptor_limit() {
    const GDTEntry &e = entries_[GDT_TSS / 8];
    return static_cast<uint16_t>(e.limit_low |
                                 (static_cast<uint16_t>(e.granularity & 0x0F)
                                  << 16));
}

/// @brief Terminator byte after the I/O bitmap.
uint8_t GDT::iopb_terminator() {
    return tss_block_.iopb_terminator;
}

} // namespace arch
