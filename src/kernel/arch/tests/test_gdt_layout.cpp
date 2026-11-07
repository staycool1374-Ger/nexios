/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as
 * published by the Free Software Foundation.
 */

/// @file test_gdt_layout.cpp
/// @brief GDT descriptor-layout tests (milestone v0.4.3 issue #115).
///        Reads the LIVE GDT through the architecturally loaded GDTR
///        (sgdt) and asserts byte-level descriptor layout: code/data
///        access bytes, DPL, L-bit, TSS base/limit, selector wiring and
///        the IOPB contract.  No private GDT access — the CPU itself
///        provides the table pointer.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/gdt.hpp>

using namespace kernel;
using arch::GDT;
using arch::GDTEntry;
using arch::TSSBlock;

namespace {

struct GDTR {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

GDTR read_gdtr() {
    GDTR gdtr{};
    asm volatile("sgdt %0" : : "m"(gdtr));
    return gdtr;
}

/// @brief Analyzer-proven non-null view of the loaded descriptor table
/// (the CPU always holds a real GDT base; guard makes that explicit).
const arch::GDTEntry *gdt_entries(const GDTR &gdtr) {
    if (gdtr.base == 0 || gdtr.limit < 8)
        return nullptr;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<const arch::GDTEntry *>(gdtr.base);
}

uint16_t read_cs() {
    uint16_t cs = 0;
    asm volatile("mov %%cs, %0" : "=r"(cs));
    return cs;
}

uint16_t read_ss() {
    uint16_t ss = 0;
    asm volatile("mov %%ss, %0" : "=r"(ss));
    return ss;
}

/// @brief Access-byte field helpers.
constexpr uint8_t access_type(uint8_t access) {
    return access & 0x0F;
}
constexpr uint8_t access_dpl(uint8_t access) {
    return (access >> 5) & 0x03;
}
constexpr bool access_present(uint8_t access) {
    return (access & 0x80) != 0;
}
constexpr bool access_system(uint8_t access) {
    return (access & 0x10) == 0;
}

} // namespace

// Runmode: kernel
// Testidea: The loaded GDTR describes exactly 7 entries (limit = 7*8-1 =
// 55) at a kernel-half base address — the pseudo-descriptor the kernel
// programmed must still be what the CPU is using.
// Input: sgdt on the live CPU.
// Expect: limit == 55; base in the kernel half (> 0xFFFF0000_00000000).
// Depends: arch::GDT
JARVIS_TEST(gdt_gdtr_limit_and_base, "PRE: iocd | POST: none") {
    GDTR gdtr = read_gdtr();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(55), static_cast<uint64_t>(gdtr.limit));
    JARVIS_ASSERT(gdtr.base > 0xFFFF000000000000ULL);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Null descriptor (slot 0) is all-zero — selector 0 can never
// accidentally reference a live segment.
// Input: Read entry 0 through the GDTR base.
// Expect: All 8 bytes zero.
// Depends: arch::GDT
JARVIS_TEST(gdt_null_descriptor_zero, "PRE: iocd | POST: none") {
    GDTR gdtr = read_gdtr();
    auto *entries = gdt_entries(gdtr);
    JARVIS_ASSERT(entries != nullptr);
    const uint8_t *raw = reinterpret_cast<const uint8_t *>(&entries[0]);
    for (size_t i = 0; i < sizeof(GDTEntry); ++i) {
        JARVIS_ASSERT_EQ(0, raw[i]);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Kernel code segment (slot GDT_CODE/8) is a ring-0,
// present, executable/readable code segment with the 64-bit L flag set
// and flat limit fields (base 0, limit 0, no granularity scaling).
// Input: Read the code descriptor bytes.
// Expect: access == 0x9A (P=1, DPL=0, S=1, type=code/exec/read), base
//         fields 0, limit_low 0, granularity 0x20 (L=1, G=0, limit_hi=0).
// Depends: arch::GDT
JARVIS_TEST(gdt_kernel_code_descriptor, "PRE: iocd | POST: none") {
    GDTR gdtr = read_gdtr();
    auto *entries = gdt_entries(gdtr);
    JARVIS_ASSERT(entries != nullptr);
    const GDTEntry &e = entries[arch::GDT_CODE / 8];
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x9A),
                     static_cast<uint64_t>(e.access & 0xFE));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x20),
                     static_cast<uint64_t>(e.granularity));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(e.base_low));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(e.base_mid));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(e.base_high));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(e.limit_low));
    JARVIS_ASSERT(access_present(e.access));
    JARVIS_ASSERT(!access_system(e.access));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(access_dpl(e.access)));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Kernel data segment (slot GDT_DATA/8) is a ring-0 present
// read/write data segment, 32-bit semantics (L=0) — data segments must
// NOT carry the L bit.
// Input: Read the data descriptor bytes.
// Expect: access == 0x92 (P=1, DPL=0, S=1, type=data/read-write),
//         granularity 0x00, flat base/limit.
// Depends: arch::GDT
JARVIS_TEST(gdt_kernel_data_descriptor, "PRE: iocd | POST: none") {
    GDTR gdtr = read_gdtr();
    auto *entries = gdt_entries(gdtr);
    JARVIS_ASSERT(entries != nullptr);
    const GDTEntry &e = entries[arch::GDT_DATA / 8];
    // The CPU sets the accessed bit (0x01) when SS is loaded with this
    // selector (Intel SDM 3.4.5.1) — compare the programmed type bits.
    if ((e.access & 0xFE) != 0x92) {
        JARVIS_FAIL("data desc access=0x%x gran=0x%x base_lo=0x%x",
                    e.access, e.granularity, e.base_low);
    }
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x92),
                     static_cast<uint64_t>(e.access & 0xFE));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x00),
                     static_cast<uint64_t>(e.granularity));
    JARVIS_ASSERT(access_present(e.access));
    JARVIS_ASSERT(!access_system(e.access));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), static_cast<uint64_t>(access_dpl(e.access)));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: User segments (slots GDT_USER_CODE/GDT_USER_DATA) are ring-3
// with the same type semantics as their kernel counterparts (code 0xFA
// with L-bit, data 0xF2 without) — DPL=3 is the privilege boundary.
// Input: Read both user descriptors.
// Expect: USER_CODE access 0xFA gran 0x20; USER_DATA access 0xF2 gran
//         0x00; DPL==3 and present on both.
// Depends: arch::GDT
JARVIS_TEST(gdt_user_descriptors_ring3, "PRE: iocd | POST: none") {
    GDTR gdtr = read_gdtr();
    auto *entries = gdt_entries(gdtr);
    JARVIS_ASSERT(entries != nullptr);
    const GDTEntry &code = entries[arch::GDT_USER_CODE / 8];
    const GDTEntry &data = entries[arch::GDT_USER_DATA / 8];
    // Accessed bit (0x01) may be set by the CPU once a selector is loaded.
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0xFA),
                     static_cast<uint64_t>(code.access & 0xFE));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x20),
                     static_cast<uint64_t>(code.granularity));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0xF2),
                     static_cast<uint64_t>(data.access & 0xFE));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x00),
                     static_cast<uint64_t>(data.granularity));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(3),
                     static_cast<uint64_t>(access_dpl(code.access)));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(3),
                     static_cast<uint64_t>(access_dpl(data.access)));
    JARVIS_ASSERT(access_present(code.access));
    JARVIS_ASSERT(access_present(data.access));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: TSS descriptor (slot GDT_TSS/8) is a 64-bit available-TSS
// system descriptor (access 0x89) whose base points at the real TSSBlock
// and whose limit covers the whole block (TSS + IOPB + terminator).
// Input: Read the TSS descriptor; cross-check base against
//        iopb_bitmap() - offsetof(TSSBlock, iopb) and limit against
//        tss_descriptor_limit() / sizeof(TSSBlock)-1.
// Expect: access 0x89, system bit set; base matches; reconstructed 20-bit
//         limit == sizeof(TSSBlock)-1 == tss_descriptor_limit().
// Depends: arch::GDT
JARVIS_TEST(gdt_tss_descriptor_base_limit, "PRE: iocd | POST: none") {
    GDTR gdtr = read_gdtr();
    auto *entries = gdt_entries(gdtr);
    JARVIS_ASSERT(entries != nullptr);
    const GDTEntry &e = entries[arch::GDT_TSS / 8];
    // LTR sets the busy bit (0x02) on a 64-bit TSS descriptor (Intel SDM
    // 7.2.3) — 0x8B in the live table PROVES the TSS is loaded; 0x89
    // would mean LTR never happened.
    if (e.access != 0x8B) {
        JARVIS_FAIL("tss desc access=0x%x gran=0x%x limit_lo=0x%x",
                    e.access, e.granularity, e.limit_low);
    }
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x8B), static_cast<uint64_t>(e.access));
    JARVIS_ASSERT(access_system(e.access));
    JARVIS_ASSERT(access_present(e.access));

    // 64-bit descriptors: the low 32 base bits sit in the TSS entry, the
    // upper 32 bits in the following canonic 8-byte entry (exactly how
    // GDT::init() programs it).
    uint64_t base_low32 = static_cast<uint64_t>(e.base_low) |
                          (static_cast<uint64_t>(e.base_mid) << 16) |
                          (static_cast<uint64_t>(e.base_high) << 24);
    uint64_t base_high32 =
        *reinterpret_cast<const uint64_t *>(&entries[arch::GDT_TSS / 8 + 1]);
    uint64_t base = base_low32 | (base_high32 << 32);
    uint64_t expected_base =
        reinterpret_cast<uint64_t>(GDT::iopb_bitmap()) -
        __builtin_offsetof(TSSBlock, iopb);
    JARVIS_ASSERT_EQ(expected_base, base);

    uint32_t limit = static_cast<uint32_t>(e.limit_low) |
                     ((static_cast<uint32_t>(e.granularity & 0x0F)) << 16);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(sizeof(TSSBlock) - 1),
                     static_cast<uint64_t>(limit));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(GDT::tss_descriptor_limit()),
                     static_cast<uint64_t>(limit));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The IOPB contract inside the TSS: iopb_offset points at the
// bitmap within the TSS block, the terminator byte is 0xFF, and after
// iopb_mask_all() the bitmap is default-deny (all 0xFF).
// Input: Public accessors tss_iopb_offset(), iopb_terminator(),
//        iopb_bitmap(); iopb_mask_all() then sample the bitmap.
// Expect: offset == offsetof(TSSBlock, iopb) (104); terminator 0xFF;
//         sampled bytes all 0xFF (spot-check 0, mid, last); the mask call
//         is the boot default so no state is changed for later tests.
// Depends: arch::GDT
JARVIS_TEST(gdt_iopb_contract, "PRE: iocd | POST: none") {
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(104),
                     static_cast<uint64_t>(GDT::tss_iopb_offset()));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x68),
                     static_cast<uint64_t>(__builtin_offsetof(TSSBlock, iopb)));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0xFF),
                     static_cast<uint64_t>(GDT::iopb_terminator()));
    uint8_t *bitmap = GDT::iopb_bitmap();
    JARVIS_ASSERT(bitmap != nullptr);
    GDT::iopb_mask_all();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0xFF), static_cast<uint64_t>(bitmap[0]));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0xFF),
                     static_cast<uint64_t>(bitmap[4096]));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0xFF), static_cast<uint64_t>(bitmap[8191]));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The live CPU selectors match the GDT wiring: kernel code runs
// on GDT_CODE (0x08) and kernel data/stack on GDT_DATA (0x10).
// Input: mov %%cs / mov %%ss.
// Expect: cs == 0x08, ss == 0x10, and both RPLs are 0.
// Depends: arch::GDT
JARVIS_TEST(gdt_live_selectors, "PRE: iocd | POST: none") {
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(arch::GDT_CODE),
                     static_cast<uint64_t>(read_cs()));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(arch::GDT_DATA),
                     static_cast<uint64_t>(read_ss()));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                     static_cast<uint64_t>(read_cs() & 0x03));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                     static_cast<uint64_t>(read_ss() & 0x03));
    JARVIS_TEST_PASS();
}

void register_gdt_layout_tests() {
    Logger::info("Registering GDT layout tests");
    JARVIS_REGISTER_TEST(gdt_gdtr_limit_and_base);
    JARVIS_REGISTER_TEST(gdt_null_descriptor_zero);
    JARVIS_REGISTER_TEST(gdt_kernel_code_descriptor);
    JARVIS_REGISTER_TEST(gdt_kernel_data_descriptor);
    JARVIS_REGISTER_TEST(gdt_user_descriptors_ring3);
    JARVIS_REGISTER_TEST(gdt_tss_descriptor_base_limit);
    JARVIS_REGISTER_TEST(gdt_iopb_contract);
    JARVIS_REGISTER_TEST(gdt_live_selectors);
}
#endif // CONFIG_ARCH_X86_64
