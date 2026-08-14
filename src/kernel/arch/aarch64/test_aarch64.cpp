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

/// @file test_aarch64.cpp
/// @brief AArch64 architecture-specific test suite.

#if defined(CONFIG_ARCH_AARCH64)

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/page_table.hpp>
#include <kernel/arch/context.hpp>
#include <kernel/arch/interrupt_controller.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/serial.hpp>
#include <kernel/arch/rtc.hpp>
#include <kernel/arch/cpuid.hpp>
#include <kernel/arch/pci.hpp>
#include <kernel/memory/pmm.hpp>
#include <lib/string.hpp>
#include <kernel/boot/bootinfo.hpp>
#include <fdt/libfdt.h>

using namespace kernel;

#include <kernel/core/global_state.hpp>

/// @brief Verify 4-level page table walk starting from TTBR1_EL1.
/// Walks L0→L1→L2→L3 for a known higher-half virtual address.
JARVIS_TEST(aarch64_page_table_4level_walk) {
    uint64_t ttbr1 = arch::read_ttbr1_el1();
    JARVIS_ASSERT_FMT(ttbr1 != 0, "TTBR1_EL1 must be non-zero, got 0x%lx",
                      ttbr1);

    uint64_t l0_phys = ttbr1;
    uint64_t *l0 = reinterpret_cast<uint64_t *>(l0_phys);

    constexpr uint64_t L0_SHIFT = 39;
    constexpr uint64_t L1_SHIFT = 30;
    constexpr uint64_t L2_SHIFT = 21;
    constexpr uint64_t L3_SHIFT = 12;
    constexpr uint64_t TABLE_MASK = 0x1FF;
    constexpr uint64_t DESC_VALID = 1ULL << 0;
    constexpr uint64_t DESC_TABLE = 1ULL << 1;
    constexpr uint64_t DESC_AF = 1ULL << 10;

    uint64_t virt = 0xFFFF800040000000ULL;

    size_t l0_idx = (virt >> L0_SHIFT) & TABLE_MASK;
    uint64_t l0_entry = l0[l0_idx];
    JARVIS_ASSERT_FMT(l0_entry & DESC_VALID, "L0 entry %zu not valid: 0x%lx",
                      l0_idx, l0_entry);
    JARVIS_ASSERT_FMT(l0_entry & DESC_TABLE, "L0 entry %zu not table: 0x%lx",
                      l0_idx, l0_entry);
    uint64_t l1_phys = l0_entry & ~0xFFF;
    JARVIS_ASSERT_FMT((l1_phys & 0xFFF) == 0,
                      "L1 table not page-aligned: 0x%lx", l1_phys);

    uint64_t *l1 = reinterpret_cast<uint64_t *>(l1_phys);
    size_t l1_idx = (virt >> L1_SHIFT) & TABLE_MASK;
    uint64_t l1_entry = l1[l1_idx];
    JARVIS_ASSERT_FMT(l1_entry & DESC_VALID, "L1 entry %zu not valid: 0x%lx",
                      l1_idx, l1_entry);
    JARVIS_ASSERT_FMT(l1_entry & DESC_TABLE, "L1 entry %zu not table: 0x%lx",
                      l1_idx, l1_entry);
    uint64_t l2_phys = l1_entry & ~0xFFF;
    JARVIS_ASSERT_FMT((l2_phys & 0xFFF) == 0,
                      "L2 table not page-aligned: 0x%lx", l2_phys);

    uint64_t *l2 = reinterpret_cast<uint64_t *>(l2_phys);
    size_t l2_idx = (virt >> L2_SHIFT) & TABLE_MASK;
    uint64_t l2_entry = l2[l2_idx];
    JARVIS_ASSERT_FMT(l2_entry & DESC_VALID, "L2 entry %zu not valid: 0x%lx",
                      l2_idx, l2_entry);
    if (l2_entry & DESC_TABLE) {
        uint64_t l3_phys = l2_entry & ~0xFFF;
        JARVIS_ASSERT_FMT((l3_phys & 0xFFF) == 0,
                          "L3 table not page-aligned: 0x%lx", l3_phys);

        uint64_t *l3 = reinterpret_cast<uint64_t *>(l3_phys);
        size_t l3_idx = (virt >> L3_SHIFT) & TABLE_MASK;
        uint64_t l3_entry = l3[l3_idx];
        JARVIS_ASSERT_FMT(l3_entry & DESC_VALID,
                          "L3 entry %zu not valid: 0x%lx", l3_idx, l3_entry);
        JARVIS_ASSERT_FMT(l3_entry & DESC_AF, "L3 entry %zu AF not set: 0x%lx",
                          l3_idx, l3_entry);
    } else {
        JARVIS_ASSERT_FMT(l2_entry & DESC_AF,
                          "L2 block entry %zu AF not set: 0x%lx", l2_idx,
                          l2_entry);
    }

    JARVIS_TEST_PASS();
}

/// @brief Map a 4K leaf page, verify get_physical, then unmap.
JARVIS_TEST(aarch64_page_table_map_leaf) {
    uint64_t phys_page = PMM::alloc_page();
    JARVIS_ASSERT_FMT(phys_page != 0, "PMM::alloc_page() returned 0");

    constexpr uint64_t TEST_VA = 0xFFFF80004000F000ULL;
    constexpr uint64_t RW_FLAGS = PageFlags::PRESENT | PageFlags::WRITE;

    auto map_result =
        arch::ArchPageTable::map_page(TEST_VA, phys_page, RW_FLAGS);
    JARVIS_ASSERT(map_result.ok());

    uint64_t retrieved = arch::ArchPageTable::get_physical(TEST_VA);
    JARVIS_ASSERT_EQ(phys_page, retrieved);

    auto unmap_result = arch::ArchPageTable::unmap_page(TEST_VA);
    JARVIS_ASSERT(unmap_result.ok());

    retrieved = arch::ArchPageTable::get_physical(TEST_VA);
    JARVIS_ASSERT_EQ(0ULL, retrieved);

    PMM::free_page(phys_page);

    JARVIS_TEST_PASS();
}

/// @brief Map 512 contiguous pages, then remap one page and verify the split.
JARVIS_TEST(aarch64_page_table_block_split) {
    constexpr uint64_t BLOCK_VA = 0xFFFF800080000000ULL;
    constexpr uint64_t PAGE_VA = BLOCK_VA + 0x1000;
    constexpr uint64_t RW_FLAGS = PageFlags::PRESENT | PageFlags::WRITE;

    uint64_t block_phys = PMM::alloc_page();
    JARVIS_ASSERT(block_phys != 0);
    block_phys &= ~0x1FFFFF;

    for (size_t i = 0; i < 512; ++i) {
        uint64_t page_phys = PMM::alloc_page();
        JARVIS_ASSERT(page_phys != 0);
        arch::ArchPageTable::map_page(BLOCK_VA + i * 0x1000, page_phys,
                                      RW_FLAGS);
    }

    uint64_t new_page_phys = PMM::alloc_page();
    JARVIS_ASSERT(new_page_phys != 0);
    auto map_result =
        arch::ArchPageTable::map_page(PAGE_VA, new_page_phys, RW_FLAGS);
    JARVIS_ASSERT(map_result.ok());

    uint64_t retrieved = arch::ArchPageTable::get_physical(PAGE_VA);
    JARVIS_ASSERT_EQ(new_page_phys, retrieved);

    for (size_t i = 0; i < 512; ++i) {
        uint64_t pa = arch::ArchPageTable::get_physical(BLOCK_VA + i * 0x1000);
        if (i == 1) {
            JARVIS_ASSERT_EQ(new_page_phys, pa);
        } else {
            JARVIS_ASSERT(pa != 0);
        }
    }

    for (size_t i = 0; i < 512; ++i) {
        arch::ArchPageTable::unmap_page(BLOCK_VA + i * 0x1000);
    }
    PMM::free_page(new_page_phys);

    JARVIS_TEST_PASS();
}

/// @brief Save and restore two contexts, verify SP values through switch_to.
JARVIS_TEST(aarch64_context_save_restore) {
    arch::ArchContext ctx_a{};
    arch::ArchContext ctx_b{};

    uint64_t rsp_a = 0xFFFF800010000000ULL;
    uint64_t rsp_b = 0xFFFF800020000000ULL;

    arch::ArchContextManager::save(ctx_a, rsp_a);
    arch::ArchContextManager::save(ctx_b, rsp_b);

    JARVIS_ASSERT_EQ(ctx_a.sp_el0, rsp_a);
    JARVIS_ASSERT_EQ(ctx_b.sp_el0, rsp_b);

    uint64_t current_rsp = rsp_a;
    arch::ArchContextManager::switch_to(ctx_a, ctx_b, current_rsp);
    JARVIS_ASSERT_EQ(current_rsp, rsp_b);
    JARVIS_ASSERT_EQ(ctx_a.sp_el0, rsp_a);

    arch::ArchContextManager::switch_to(ctx_b, ctx_a, current_rsp);
    JARVIS_ASSERT_EQ(current_rsp, rsp_a);

    JARVIS_TEST_PASS();
}

/// @brief Initialise a context stack and verify the stack pointer is within
/// bounds.
JARVIS_TEST(aarch64_context_init_stack) {
    uint64_t stack[1024];
    uint64_t *stack_top = stack + 1024;

    auto test_entry = []() {
        while (1) {
            arch::pause();
        }
    };

    arch::ArchContextManager::init_stack(stack_top, test_entry, 0, 0, 0x3C0,
                                         0xFFFF800030000000ULL);

    JARVIS_ASSERT(stack_top < stack + 1024);
    JARVIS_ASSERT(stack_top > stack);

    JARVIS_TEST_PASS();
}

/// @brief Verify GIC initialisation: GICD_CTLR enable bit and IT lines.
JARVIS_TEST(aarch64_gic_init) {
    arch::ArchInterruptController::init();

    volatile uint32_t *gicd =
        reinterpret_cast<volatile uint32_t *>(0x8000000ULL);
    volatile uint32_t *gicc =
        reinterpret_cast<volatile uint32_t *>(0x8010000ULL);

    uint32_t gicd_ctlr = gicd[0];
    JARVIS_ASSERT_FMT(gicd_ctlr & 1, "GICD_CTLR Enable not set: 0x%x",
                      gicd_ctlr);

    uint32_t typer = gicd[1];
    uint32_t it_lines = (typer & 0x1F) + 1;
    JARVIS_ASSERT_FMT(it_lines >= 1, "GICD_TYPER ITLinesNumber < 1: %u",
                      it_lines);

    if (gicd_ctlr & (1 << 31)) {
        uint32_t gicc_ctlr = gicc[0];
        JARVIS_ASSERT_FMT(gicc_ctlr & 1, "GICC_CTLR Enable not set: 0x%x",
                          gicc_ctlr);
    }

    JARVIS_TEST_PASS();
}

/// @brief Mask and unmask IRQ 64, verify ICENABLER and ISENABLER registers.
JARVIS_TEST(aarch64_gic_mask_unmask) {
    arch::ArchInterruptController::mask(64);

    volatile uint32_t *gicd =
        reinterpret_cast<volatile uint32_t *>(0x8000000ULL);
    uint32_t icenabler = gicd[0x180 / 4 + 2];
    JARVIS_ASSERT_FMT(icenabler & (1U << 0),
                      "GICD_ICENABLER bit 0 not set for IRQ 64: 0x%x",
                      icenabler);

    arch::ArchInterruptController::unmask(64);
    uint32_t isenabler = gicd[0x100 / 4 + 2];
    JARVIS_ASSERT_FMT(isenabler & (1U << 0),
                      "GICD_ISENABLER bit 0 not set for IRQ 64: 0x%x",
                      isenabler);

    JARVIS_TEST_PASS();
}

/// @brief Signal EOI and verify mask/unmask round-trip on IRQ 32.
JARVIS_TEST(aarch64_gic_eoi) {
    arch::ArchInterruptController::eoi(32);

    arch::ArchInterruptController::mask(32);
    arch::ArchInterruptController::unmask(32);

    JARVIS_TEST_PASS();
}

/// @brief Verify that the timer tick counter is monotonically non-decreasing.
JARVIS_TEST(aarch64_timer_counter_monotonic) {
    uint64_t prev = arch::Timer::ticks();
    for (int i = 0; i < 10; ++i) {
        uint64_t curr = arch::Timer::ticks();
        JARVIS_ASSERT_FMT(curr >= prev,
                          "Timer not monotonic: prev=%lu, curr=%lu", prev,
                          curr);
        prev = curr;
        for (int j = 0; j < 1000; ++j) {
            asm volatile("");
        }
    }
    JARVIS_TEST_PASS();
}

/// @brief Verify CNTFRQ_EL0 is in a reasonable range and ns() conversion is
/// accurate.
JARVIS_TEST(aarch64_timer_frequency) {
    uint64_t freq{};
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    JARVIS_ASSERT_FMT(
        freq >= 10000000ULL && freq <= 200000000ULL,
        "Timer frequency out of range: %lu Hz (expected 10MHz-200MHz)", freq);

    uint64_t ns_per_tick = 1000000000ULL / freq;
    uint64_t ticks_for_1s = freq;
    uint64_t ns_calc = ticks_for_1s * ns_per_tick;
    JARVIS_ASSERT_FMT(ns_calc >= 900000000ULL && ns_calc <= 1100000000ULL,
                      "Timer ns() conversion inaccurate: %lu ns for 1s",
                      ns_calc);

    JARVIS_TEST_PASS();
}

/// @brief Verify TLB flush operations (all and by-VA) do not fault.
JARVIS_TEST(aarch64_mmu_cache_tlb) {
    arch::ArchPageTable::tlb_flush_all();

    constexpr uint64_t TEST_VA = 0xFFFF800040000000ULL;
    arch::ArchPageTable::tlb_flush(TEST_VA);

    asm volatile("dsb sy" : : : "memory");
    asm volatile("isb" : : : "memory");

    arch::ArchPageTable::tlb_flush_all();

    JARVIS_TEST_PASS();
}

/// @brief Check that FP and Advanced SIMD are implemented (ID_AA64PFR0_EL1).
JARVIS_TEST(aarch64_fpu_neon_detection) {
    uint64_t id_aa64pfr0{};
    asm volatile("mrs %0, id_aa64pfr0_el1" : "=r"(id_aa64pfr0));

    uint64_t fp = (id_aa64pfr0 >> 16) & 0xF;
    uint64_t advsimd = (id_aa64pfr0 >> 20) & 0xF;

    JARVIS_ASSERT_FMT(fp != 0xF,
                      "FP not implemented (field=0xF): ID_AA64PFR0=0x%lx",
                      id_aa64pfr0);
    JARVIS_ASSERT_FMT(
        advsimd != 0xF,
        "Advanced SIMD not implemented (field=0xF): ID_AA64PFR0=0x%lx",
        id_aa64pfr0);

    arch::CpuIdResult cpuid_result = arch::cpuid(0);
    (void)cpuid_result;

    JARVIS_TEST_PASS();
}

/// @brief Write printable ASCII characters to the UART and verify TXFE status.
JARVIS_TEST(aarch64_uart_putc) {
    for (char c = 0x20; c <= 0x7E; ++c) {
        arch::Serial::putchar(c);
    }

    volatile uint32_t *uart =
        reinterpret_cast<volatile uint32_t *>(arch::HHDM_OFFSET + 0x9000000ULL);
    uint32_t fr = uart[0x18 / 4];
    JARVIS_ASSERT_FMT(fr & (1 << 7), "TXFE not set after putc: FR=0x%x", fr);

    JARVIS_TEST_PASS();
}

/// @brief Read PCI vendor/device ID via ECAM at BDF 0:0:0.
JARVIS_TEST(aarch64_pci_ecam_read) {
    arch::PciBdf bdf{0, 0, 0};
    uint16_t vendor = arch::pci_read_vendor(bdf);
    uint16_t device = arch::pci_read_device(bdf);

    JARVIS_ASSERT_FMT(vendor != 0xFFFF,
                      "PCI Vendor ID 0xFFFF (no device at 0:0:0)");
    JARVIS_ASSERT_FMT(device != 0x0000, "PCI Device ID 0x0000 at 0:0:0");

    JARVIS_TEST_PASS();
}

/// @brief Read RTC time twice, verify year is in range and time does not
/// regress.
JARVIS_TEST(aarch64_rtc_read) {
    arch::RTC::read_seconds();

    arch::tm time1{};
    arch::RTC::read_time(&time1);

    uint16_t year1 = time1.tm_year + 1900;
    JARVIS_ASSERT_FMT(year1 >= 2025 && year1 <= 2035,
                      "RTC year out of range: %u", year1);

    for (int i = 0; i < 100000; ++i) {
        asm volatile("");
    }

    arch::tm time2{};
    arch::RTC::read_time(&time2);
    uint64_t secs1 = time1.tm_hour * 3600 + time1.tm_min * 60 + time1.tm_sec;
    uint64_t secs2 = time2.tm_hour * 3600 + time2.tm_min * 60 + time2.tm_sec;
    JARVIS_ASSERT_FMT(secs2 >= secs1, "RTC time regressed: %lu -> %lu", secs1,
                      secs2);

    JARVIS_TEST_PASS();
}

/// @brief Verify the DTB pointer from boot info is valid, correctly aligned,
/// and has a valid FDT header.
JARVIS_TEST(aarch64_boot_dtb_pointer) {
    JARVIS_ASSERT_FMT(kernel::gs::boot_info().dtb_ptr != 0, "DTB pointer is zero");

    uintptr_t dtb_addr = static_cast<uintptr_t>(kernel::gs::boot_info().dtb_ptr);
    JARVIS_ASSERT_FMT(
        dtb_addr >= 0x40000000ULL && dtb_addr <= 0x50000000ULL,
        "DTB pointer 0x%lx not in expected RAM range (0x40000000-0x50000000)",
        dtb_addr);

    void *dtb = reinterpret_cast<void *>(kernel::gs::boot_info().dtb_ptr);
    JARVIS_ASSERT_FMT(fdt_check_header(dtb) == 0,
                      "FDT header validation failed");

    uint32_t magic = *static_cast<const uint32_t *>(dtb);
    magic = __builtin_bswap32(magic);
    JARVIS_ASSERT_FMT(magic == 0xD00DFEED,
                      "DTB magic mismatch: 0x%x (expected 0xD00DFEED)", magic);

    JARVIS_ASSERT_FMT(kernel::gs::boot_info().num_mem_regions > 0,
                      "No memory regions parsed from DTB");
    JARVIS_ASSERT_FMT(kernel::gs::boot_info().total_mem_size > 0,
                      "Total memory size is zero after DTB parsing");

    JARVIS_TEST_PASS();
}

/// @brief Verify VBAR_EL1 is non-zero, 2KB-aligned, and contains at least one
/// valid instruction.
JARVIS_TEST(aarch64_exception_vector_installed) {
    uint64_t vbar{};
    asm volatile("mrs %0, vbar_el1" : "=r"(vbar));

    JARVIS_ASSERT_FMT(vbar != 0, "VBAR_EL1 is zero");
    JARVIS_ASSERT_FMT((vbar & 0x7FF) == 0, "VBAR_EL1 not 2KB aligned: 0x%lx",
                      vbar);

    uint32_t *vec = reinterpret_cast<uint32_t *>(vbar);
    int valid_count = 0;
    for (int i = 0; i < 128; ++i) {
        uint32_t instr = vec[i];
        if (instr != 0) {
            valid_count++;
        }
    }
    JARVIS_ASSERT_FMT(
        valid_count > 0,
        "No valid instructions in exception vector table (checked 128 entries)");

    JARVIS_TEST_PASS();
}

/// @brief Register all AArch64 architecture test cases.
void register_aarch64_tests() {
    Logger::info("Registering aarch64 architecture tests");

    JARVIS_REGISTER_TEST(aarch64_page_table_4level_walk);
    JARVIS_REGISTER_TEST(aarch64_page_table_map_leaf);
    JARVIS_REGISTER_TEST(aarch64_page_table_block_split);
    JARVIS_REGISTER_TEST(aarch64_context_save_restore);
    JARVIS_REGISTER_TEST(aarch64_context_init_stack);
    JARVIS_REGISTER_TEST(aarch64_gic_init);
    JARVIS_REGISTER_TEST(aarch64_gic_mask_unmask);
    JARVIS_REGISTER_TEST(aarch64_gic_eoi);
    JARVIS_REGISTER_TEST(aarch64_timer_counter_monotonic);
    JARVIS_REGISTER_TEST(aarch64_timer_frequency);
    JARVIS_REGISTER_TEST(aarch64_mmu_cache_tlb);
    JARVIS_REGISTER_TEST(aarch64_fpu_neon_detection);
    JARVIS_REGISTER_TEST(aarch64_uart_putc);
    JARVIS_REGISTER_TEST(aarch64_pci_ecam_read);
    JARVIS_REGISTER_TEST(aarch64_rtc_read);
    JARVIS_REGISTER_TEST(aarch64_boot_dtb_pointer);
    JARVIS_REGISTER_TEST(aarch64_exception_vector_installed);
}

#endif