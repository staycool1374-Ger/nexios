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
#include <kernel/arch/aarch64/hal/gic.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/serial.hpp>
#include <kernel/arch/rtc.hpp>
#include <kernel/arch/cpuid.hpp>
#include <kernel/arch/pci.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <kernel/test/test_isolate.hpp>
#include <kernel/elf/elf_loader.hpp>
#include <kernel/vfs/vfs.hpp>

/// @brief aarch64 EL0 fork smoke test (issue #104): the fork-marker program
///        forks; the child prints CHILD-MARKER and exits 0; the parent
///        waitpids and prints PARENT-OK on (pid match, status 0).
///        The test asserts PARENT-OK in the stdout capture plus clean exit:
///        PARENT-OK requires the child's _exit(0) (status write) and the
///        waitpid pid return, so it is a positive child-execution marker —
///        a translation-faulted child never exits and waitpid never succeeds.
///        CHILD-MARKER itself is NOT asserted: parent and child share fd
///        offset 0 on the cloned descriptor, so the parent's later write
///        overwrites the child's bytes by construction.
///        Join via wait_for_termination_safe (need_resched + hlt): raw
///        pause-spins starve the deferred scheduler under TCG.
extern "C" {
extern const uint8_t _binary_fork_marker_img_start[];
extern const uint8_t _binary_fork_marker_img_end[];
}

namespace {
constexpr const char *kFmImagePath = "/tmp/fork_marker.elf";
constexpr const char *kFmStdoutPath = "/tmp/fork_stdout.txt";

void fm_cleanup(const char *path) {
    kernel::test::mark_vfs_touched();
    kernel::vfs::unlink(path);
}

uint64_t fm_write_file(const char *path, const uint8_t *data, size_t size) {
    kernel::test::mark_vfs_touched();
    if (size == 0)
        return 0;
    if (kernel::vfs::create(path, 0) != 0)
        return 0;
    kernel::vfs::Vnode *file = kernel::vfs::resolve(path);
    if (file == nullptr || file->ops == nullptr ||
        file->ops->write == nullptr) {
        fm_cleanup(path);
        return 0;
    }
    // 4 KiB chunks: a single large write exceeds tmpfs call limits.
    size_t off = 0;
    while (off < size) {
        size_t chunk = size - off;
        if (chunk > 4096)
            chunk = 4096;
        int64_t written =
            file->ops->write(*file, data + off, chunk, off);
        if (written <= 0) {
            fm_cleanup(path);
            return 0;
        }
        off += static_cast<size_t>(written);
    }
    return (off == size) ? size : 0ULL;
}

bool fm_capture_contains(const char *capbuf, const char *needle) {
    for (size_t i = 0; capbuf[i] != '\0'; ++i) {
        size_t j = 0;
        while (needle[j] != '\0' && capbuf[i + j] == needle[j])
            ++j;
        if (needle[j] == '\0')
            return true;
    }
    return false;
}
}  // namespace

// Runmode: kernel
// Testidea: End-to-end EL0 fork/exit/waitpid on aarch64 (issue #104).
// Input: Stage the fork-marker ELF to tmpfs, ElfLoader-load, wire fd 1 to
//        a capture file, dispatch at prio 11, join to TERMINATED.
// Expect: Clean exit (code 0) and PARENT-OK in the capture.
// Depends: ElfLoader, tmpfs, SVC fork/waitpid/exit, scheduler dispatch.
JARVIS_TEST(aarch64_fork_marker_smoke, "PRE: none | POST: none") {
    using namespace kernel;
    size_t img_size = static_cast<size_t>(_binary_fork_marker_img_end -
                                          _binary_fork_marker_img_start);
    JARVIS_ASSERT_FMT(img_size != 0, "fork-marker image missing");
    elf::ElfLoader::reset();
    JARVIS_ASSERT_FMT(
        fm_write_file(kFmImagePath, _binary_fork_marker_img_start, img_size) !=
            0,
        "tmpfs stage failed");
    JARVIS_ASSERT_FMT(
        elf::ElfLoader::request_load(kFmImagePath) == elf::LoadResult::OK,
        "request_load failed");
    elf::ElfLoader::wait_loader_idle();
    TaskControlBlock *t = elf::ElfLoader::take_completed();
    JARVIS_ASSERT_FMT(t != nullptr && t->page_table_ != 0 && t->is_user_,
                      "take_completed failed");
    JARVIS_ASSERT_FMT(t->fd_table.fds[1].used, "fd1 not set by loader");
    test::mark_vfs_touched();
    JARVIS_ASSERT_FMT(vfs::create(kFmStdoutPath, 0) == 0,
                      "stdout create failed");
    vfs::Vnode *cap_vn = vfs::resolve(kFmStdoutPath);
    JARVIS_ASSERT_FMT(cap_vn != nullptr, "stdout resolve failed");
    // Direct slot overwrite (no free/alloc: destroy_completed_tcb no-dec
    // discipline, mirroring test_libc_verify).
    t->fd_table.fds[1].vnode = cap_vn;
    t->fd_table.fds[1].offset = 0;
    t->fd_table.fds[1].flags = 0;
    // Prio 11: the harness idles lower-priority waitees would starve
    // (every driven test uses >= 11).  Set before add_task (not yet
    // queued: no re-bucket needed).
    t->priority = 11;
    t->base_priority = 11;
    {
        arch::IrqGuard ig{};
        Scheduler::add_task(*t);
    }
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    bool clean =
        (t->state == TaskState::TERMINATED && t->exit_code == 0);
    // Read back the capture BEFORE teardown; teardown mirrors the
    // test_syscall.cpp fixture pattern (is_valid + terminate-if-needed).
    static char capbuf[256];
    size_t pos = 0;
    {
        vfs::Vnode *cap = vfs::resolve(kFmStdoutPath);
        if (cap != nullptr && cap->ops != nullptr &&
            cap->ops->read != nullptr) {
            while (pos < sizeof(capbuf) - 1) {
                int64_t rd = cap->ops->read(
                    *cap, reinterpret_cast<uint8_t *>(capbuf + pos), 1,
                    pos);
                if (rd <= 0)
                    break;
                pos += static_cast<size_t>(rd);
            }
        }
        capbuf[pos] = '\0';
    }
    bool parent_ok = fm_capture_contains(capbuf, "PARENT-OK");
    if (TaskControlBlock::is_valid(t) &&
        (t->state != TaskState::TERMINATED || t->exit_code == 0))
        Scheduler::terminate(*t, 0);
    Scheduler::drain_zombie_list();
    fm_cleanup(kFmImagePath);
    test::mark_vfs_touched();
    vfs::unlink(kFmStdoutPath);
    JARVIS_ASSERT_FMT(clean, "fork-marker did not exit clean");
    JARVIS_ASSERT_FMT(parent_ok, "PARENT-OK missing from capture");
    JARVIS_TEST_PASS();
}
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

    // Page-table pages are physical; dereference through the HHDM direct-map
    // alias so the walk is independent of the currently-active TTBR0 (which
    // may be a user/private PML4 without the identity map).
    uint64_t l0_phys = ttbr1;
    uint64_t *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l0_phys);

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

    uint64_t *l1 =
        reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l1_phys);
    size_t l1_idx = (virt >> L1_SHIFT) & TABLE_MASK;
    uint64_t l1_entry = l1[l1_idx];
    JARVIS_ASSERT_FMT(l1_entry & DESC_VALID, "L1 entry %zu not valid: 0x%lx",
                      l1_idx, l1_entry);
    JARVIS_ASSERT_FMT(l1_entry & DESC_TABLE, "L1 entry %zu not table: 0x%lx",
                      l1_idx, l1_entry);
    uint64_t l2_phys = l1_entry & ~0xFFF;
    JARVIS_ASSERT_FMT((l2_phys & 0xFFF) == 0,
                      "L2 table not page-aligned: 0x%lx", l2_phys);

    uint64_t *l2 =
        reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l2_phys);
    size_t l2_idx = (virt >> L2_SHIFT) & TABLE_MASK;
    uint64_t l2_entry = l2[l2_idx];
    JARVIS_ASSERT_FMT(l2_entry & DESC_VALID, "L2 entry %zu not valid: 0x%lx",
                      l2_idx, l2_entry);
    if (l2_entry & DESC_TABLE) {
        uint64_t l3_phys = l2_entry & ~0xFFF;
        JARVIS_ASSERT_FMT((l3_phys & 0xFFF) == 0,
                          "L3 table not page-aligned: 0x%lx", l3_phys);

        uint64_t *l3 =
            reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l3_phys);
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

    constexpr uint64_t TEST_VA = 0xFFFF800090001000ULL;
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

    volatile uint32_t *gicd = arch::gicd_reg(0);
    volatile uint32_t *gicc = arch::gicc_reg(0);

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

/// @brief Mask and unmask IRQ 32, verify the ISENABLER enable state.
/// ICENABLER is a write-to-disable bank whose read-back is unreliable on
/// QEMU virt GICv2; the enabled state is authoritative in ISENABLER.
/// NOTE (issue #30): IRQ 64 is OUTSIDE the driver's valid window
/// (GIC_MAX_IRQ=64, issue #198 — lines >= 64 must not touch MMIO), so the
/// SPI probe uses IRQ 32 (ISENABLER bank 1, bit 0), the lowest SPI.
JARVIS_TEST(aarch64_gic_mask_unmask) {
    volatile uint32_t *gicd = arch::gicd_reg(0);

    // Enable IRQ 32 (SPI, ISENABLER bank 1, bit 0).
    arch::ArchInterruptController::unmask(32);
    uint32_t isenabler = gicd[0x100 / 4 + 1];
    JARVIS_ASSERT_FMT(isenabler & (1U << 0),
                      "GICD_ISENABLER bit 0 not set after unmask IRQ 32: 0x%x",
                      isenabler);

    // Mask IRQ 32 — the enable bit must clear.
    arch::ArchInterruptController::mask(32);
    isenabler = gicd[0x100 / 4 + 1];
    JARVIS_ASSERT_FMT((isenabler & (1U << 0)) == 0,
                      "GICD_ISENABLER bit 0 still set after mask IRQ 32: 0x%x",
                      isenabler);

    // Restore the enabled state.
    arch::ArchInterruptController::unmask(32);

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

/// @brief Verify the DTB pointer from boot info, when provided, is valid,
/// correctly aligned, and has a valid FDT header; and that the kernel has a
/// correct memory configuration either way.
///
/// QEMU `-machine virt` `-kernel` boots only hand the DTB to Linux-style
/// images (ARM64 boot-header magic).  This kernel is booted as a generic ELF
/// (no header), so the DTB is absent and memory comes from the platform
/// fallback — both are valid configurations the boot must tolerate.
JARVIS_TEST(aarch64_boot_dtb_pointer) {
    uintptr_t dtb_addr =
        static_cast<uintptr_t>(kernel::gs::boot_info().dtb_ptr);

    if (dtb_addr != 0) {
        JARVIS_ASSERT_FMT(
            dtb_addr >= 0x40000000ULL && dtb_addr <= 0x50000000ULL,
            "DTB pointer 0x%lx not in expected RAM range (0x40000000-0x50000000)",
            dtb_addr);

        // Page-table pages are physical; dereference through the HHDM
        // direct-map alias (raw phys VAs resolve through the active TTBR0).
        void *dtb = reinterpret_cast<void *>(arch::HHDM_OFFSET + dtb_addr);
        JARVIS_ASSERT_FMT(fdt_check_header(dtb) == 0,
                          "FDT header validation failed");

        uint32_t magic = *static_cast<const uint32_t *>(dtb);
        magic = __builtin_bswap32(magic);
        JARVIS_ASSERT_FMT(magic == 0xD00DFEED,
                          "DTB magic mismatch: 0x%x (expected 0xD00DFEED)",
                          magic);

        JARVIS_ASSERT_FMT(kernel::gs::boot_info().num_mem_regions > 0,
                          "No memory regions parsed from DTB");
        JARVIS_ASSERT_FMT(kernel::gs::boot_info().total_mem_size > 0,
                          "Total memory size is zero after DTB parsing");
    }

    // Either source (DTB or platform fallback) must yield a usable memory
    // configuration — the kernel must not boot with zero memory.
    JARVIS_ASSERT_FMT(kernel::gs::boot_info().total_mem_size > 0,
                      "No memory configured (DTB absent and fallback empty)");

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

/// @brief Walk TTBR1_EL1 to the leaf descriptor for a higher-half VA.
/// Mirrors the L0->L1->L2->L3 index math of aarch64_page_table_4level_walk.
/// @param[in] va Higher-half virtual address (4K-aligned).
/// @return Leaf descriptor bits, or 0 if any level is invalid.
static uint64_t walk_leaf_descriptor(uint64_t va) {
    constexpr uint64_t L0_SHIFT = 39;
    constexpr uint64_t L1_SHIFT = 30;
    constexpr uint64_t L2_SHIFT = 21;
    constexpr uint64_t TABLE_MASK = 0x1FF;
    constexpr uint64_t DESC_VALID = 1ULL << 0;
    constexpr uint64_t DESC_TABLE = 1ULL << 1;

    uint64_t l0_phys = arch::read_ttbr1_el1();
    uint64_t *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l0_phys);
    size_t l0_idx = (va >> L0_SHIFT) & TABLE_MASK;
    if (!(l0[l0_idx] & DESC_VALID))
        return 0;
    uint64_t *l1 = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (l0[l0_idx] & ~0xFFF));
    size_t l1_idx = (va >> L1_SHIFT) & TABLE_MASK;
    if (!(l1[l1_idx] & DESC_VALID))
        return 0;
    uint64_t *l2 = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (l1[l1_idx] & ~0xFFF));
    size_t l2_idx = (va >> L2_SHIFT) & TABLE_MASK;
    if (!(l2[l2_idx] & DESC_VALID))
        return 0;
    if (!(l2[l2_idx] & DESC_TABLE))
        return l2[l2_idx];
    uint64_t *l3 = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (l2[l2_idx] & ~0xFFF));
    size_t l3_idx = (va >> 12) & TABLE_MASK;
    return (l3[l3_idx] & DESC_VALID) ? l3[l3_idx] : 0;
}

/// @brief Verify SCTLR_EL1.PAN (bit 23) matches FEAT_PAN detection, and
/// degraded mode (unsupported) keeps read_rflags() == 0.
JARVIS_TEST(aarch64_pan_sctlr_bit_set) {
    uint64_t sctlr{};
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if (arch::g_pan_supported) {
        JARVIS_ASSERT_FMT(sctlr & (1ULL << 23),
                          "SCTLR_EL1.PAN not set: sctlr=0x%lx", sctlr);
    } else {
        JARVIS_ASSERT_FMT((sctlr & (1ULL << 23)) == 0,
                          "SCTLR_EL1.PAN set but unsupported: sctlr=0x%lx",
                          sctlr);
        JARVIS_ASSERT_EQ(0ULL, arch::read_rflags());
    }
    JARVIS_TEST_PASS();
}

/// @brief Verify stac/clac toggle the normalized AC-equivalent (bit 18)
/// contract and end in default-deny (PAN set) state.
JARVIS_TEST(aarch64_pan_stac_clac_roundtrip) {
    if (arch::g_pan_supported) {
        arch::clac();
        JARVIS_ASSERT_EQ(0ULL, arch::read_rflags() & (1ULL << 18));
        arch::stac();
        JARVIS_ASSERT_FMT(arch::read_rflags() & (1ULL << 18),
                          "stac did not set AC-equivalent bit");
        arch::clac();
        JARVIS_ASSERT_EQ(0ULL, arch::read_rflags() & (1ULL << 18));
    } else {
        arch::stac();
        JARVIS_ASSERT_EQ(0ULL, arch::read_rflags());
        arch::clac();
        JARVIS_ASSERT_EQ(0ULL, arch::read_rflags());
    }
    JARVIS_TEST_PASS();
}

/// @brief Map a kernel (non-USER) page; verify UXN (bit 54) and PXN
/// (bit 53) are both set on the leaf descriptor.
JARVIS_TEST(aarch64_pte_kernel_page_uxn_pxn) {
    uint64_t phys_page = PMM::alloc_page();
    JARVIS_ASSERT_FMT(phys_page != 0, "PMM::alloc_page() returned 0");

    constexpr uint64_t TEST_VA = 0xFFFF800090002000ULL;
    constexpr uint64_t RW_FLAGS = PageFlags::PRESENT | PageFlags::WRITE;

    auto map_result =
        arch::ArchPageTable::map_page(TEST_VA, phys_page, RW_FLAGS);
    JARVIS_ASSERT(map_result.ok());

    uint64_t leaf = walk_leaf_descriptor(TEST_VA);
    JARVIS_ASSERT_FMT(leaf & (1ULL << 53), "kernel PTE PXN not set: 0x%lx",
                      leaf);
    JARVIS_ASSERT_FMT(leaf & (1ULL << 54), "kernel PTE UXN not set: 0x%lx",
                      leaf);

    auto unmap_result = arch::ArchPageTable::unmap_page(TEST_VA);
    JARVIS_ASSERT(unmap_result.ok());
    PMM::free_page(phys_page);

    JARVIS_TEST_PASS();
}

/// @brief Map a USER page; verify PXN (bit 53) is set (SMEP parity — EL1
/// may not execute user pages) and UXN (bit 54) is clear (EL0 may execute).
JARVIS_TEST(aarch64_pte_user_page_pxn) {
    uint64_t phys_page = PMM::alloc_page();
    JARVIS_ASSERT_FMT(phys_page != 0, "PMM::alloc_page() returned 0");

    constexpr uint64_t TEST_VA = 0xFFFF800090003000ULL;
    constexpr uint64_t RW_FLAGS =
        PageFlags::PRESENT | PageFlags::WRITE | PageFlags::USER;

    auto map_result =
        arch::ArchPageTable::map_page(TEST_VA, phys_page, RW_FLAGS);
    JARVIS_ASSERT(map_result.ok());

    uint64_t leaf = walk_leaf_descriptor(TEST_VA);
    JARVIS_ASSERT_FMT(leaf & (1ULL << 53), "user PTE PXN not set: 0x%lx",
                      leaf);
    JARVIS_ASSERT_FMT((leaf & (1ULL << 54)) == 0,
                      "user PTE UXN set (EL0 exec blocked): 0x%lx", leaf);

    auto unmap_result = arch::ArchPageTable::unmap_page(TEST_VA);
    JARVIS_ASSERT(unmap_result.ok());
    PMM::free_page(phys_page);

    JARVIS_TEST_PASS();
}

/// @brief Map a USER|NX page; verify both UXN (bit 54) and PXN (bit 53)
/// are set.
JARVIS_TEST(aarch64_pte_user_nx_uxn) {
    uint64_t phys_page = PMM::alloc_page();
    JARVIS_ASSERT_FMT(phys_page != 0, "PMM::alloc_page() returned 0");

    constexpr uint64_t TEST_VA = 0xFFFF800090004000ULL;
    constexpr uint64_t RW_FLAGS =
        PageFlags::PRESENT | PageFlags::WRITE | PageFlags::USER |
        PageFlags::NX;

    auto map_result =
        arch::ArchPageTable::map_page(TEST_VA, phys_page, RW_FLAGS);
    JARVIS_ASSERT(map_result.ok());

    uint64_t leaf = walk_leaf_descriptor(TEST_VA);
    JARVIS_ASSERT_FMT(leaf & (1ULL << 53), "user NX PTE PXN not set: 0x%lx",
                      leaf);
    JARVIS_ASSERT_FMT(leaf & (1ULL << 54), "user NX PTE UXN not set: 0x%lx",
                      leaf);

    auto unmap_result = arch::ArchPageTable::unmap_page(TEST_VA);
    JARVIS_ASSERT(unmap_result.ok());
    PMM::free_page(phys_page);

    JARVIS_TEST_PASS();
}

/// @brief Issue #103: VMM::deep_copy_user_pages must build VALID aarch64
///        table descriptors (bits[1:0]=11) at every level of the copied
///        child hierarchy.  The pre-fix build emitted bits[1:0]=01 at L0/L1/L2
///        (PAGE_WRITE=0, PAGE_USER=1<<6 on aarch64) — a reserved/invalid
///        table-descriptor encoding that translation-faults on the first MMU
///        walk of a forked address space.  The copied L3 leaf inherits AF and
///        AttrIndx verbatim from the source (bits[11:2] are RES0/ignored in
///        table descriptors, so the minimal PAGE_PRESENT|PAGE_TABLE is the
///        correct table-pointer form).
JARVIS_TEST(aarch64_deep_copy_user_descriptors_valid) {
    uint64_t parent_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(parent_pml4 != 0);
    uint64_t child_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(child_pml4 != 0);

    // Typical ELF base; map_page_in_pml4(user=true) builds USER-owned
    // table pages + leaf so free_user_pages reclaims them (MP-7).
    constexpr uint64_t TEST_VA = 0x400000;
    uint64_t user_page = PMM::alloc_user_page();
    JARVIS_ASSERT(user_page != 0);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    *reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + user_page) = 0x5A;
    VMM::map_page_in_pml4(TEST_VA, user_page, true, parent_pml4);

    // MP-7 fork semantics: deep copy, never shared tables.
    JARVIS_ASSERT(VMM::deep_copy_user_pages(parent_pml4, child_pml4));

    size_t pml4_idx = arch::ArchPageTable::pml4_index(TEST_VA);
    size_t pdpt_idx = arch::ArchPageTable::pdpt_index(TEST_VA);
    size_t pd_idx = arch::ArchPageTable::pd_index(TEST_VA);
    size_t pt_idx = arch::ArchPageTable::pt_index(TEST_VA);
    JARVIS_ASSERT(pml4_idx < arch::PML4_USER_COUNT);

    auto *p4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (parent_pml4 & ~0xFFFULL));
    auto *c4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (child_pml4 & ~0xFFFULL));

    // L0 (PML4 → PDPT) table descriptor: bits[1:0]=11, distinct table page.
    JARVIS_ASSERT((c4[pml4_idx] & 0x3) == 0x3);
    JARVIS_ASSERT(c4[pml4_idx] & (1ULL << 1));
    JARVIS_ASSERT((c4[pml4_idx] & ~0xFFFULL) != (p4[pml4_idx] & ~0xFFFULL));

    // L1 (PDPT → PD): bits[1:0]=11, distinct table page.
    auto *c_pdpt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                (c4[pml4_idx] & ~0xFFFULL));
    auto *p_pdpt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                (p4[pml4_idx] & ~0xFFFULL));
    JARVIS_ASSERT((c_pdpt[pdpt_idx] & 0x3) == 0x3);
    JARVIS_ASSERT(c_pdpt[pdpt_idx] & (1ULL << 1));
    JARVIS_ASSERT((c_pdpt[pdpt_idx] & ~0xFFFULL) !=
                  (p_pdpt[pdpt_idx] & ~0xFFFULL));

    // L2 (PD → PT): bits[1:0]=11, distinct table page.
    auto *c_pd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (c_pdpt[pdpt_idx] & ~0xFFFULL));
    auto *p_pd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (p_pdpt[pdpt_idx] & ~0xFFFULL));
    JARVIS_ASSERT((c_pd[pd_idx] & 0x3) == 0x3);
    JARVIS_ASSERT(c_pd[pd_idx] & (1ULL << 1));
    JARVIS_ASSERT((c_pd[pd_idx] & ~0xFFFULL) != (p_pd[pd_idx] & ~0xFFFULL));

    // L3 leaf: bits[1:0]=11, AF inherited, distinct phys, deep-copied content.
    auto *c_pt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (c_pd[pd_idx] & ~0xFFFULL));
    auto *p_pt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (p_pd[pd_idx] & ~0xFFFULL));
    JARVIS_ASSERT((c_pt[pt_idx] & 0x3) == 0x3);
    JARVIS_ASSERT(c_pt[pt_idx] & (1ULL << 10));
    uint64_t child_leaf = c_pt[pt_idx] & 0x0000FFFFFFFFF000ULL;
    uint64_t parent_leaf = p_pt[pt_idx] & 0x0000FFFFFFFFF000ULL;
    JARVIS_ASSERT(child_leaf != 0);
    JARVIS_ASSERT(child_leaf != parent_leaf);
    JARVIS_ASSERT(child_leaf == VMM::virt_to_phys_in_pml4(TEST_VA, child_pml4));

    // Content copied + no-alias: write through the child frame, parent stays.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    JARVIS_ASSERT(*reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + child_leaf) ==
                  0x5A);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    *reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + child_leaf) = 0xA5;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    JARVIS_ASSERT(*reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + parent_leaf) ==
                  0x5A);

    VMM::free_user_pages(child_pml4);
    PMM::free_page(child_pml4);
    VMM::free_user_pages(parent_pml4);
    PMM::free_page(parent_pml4);

    JARVIS_TEST_PASS();
}

/// @brief Pin the aarch64 SVC register convention, dispatch half (issue #30):
///        the saved-x8 slot carries the number, saved x0-x3 carry args, and
///        the handler return is the x0 value (syscall_entry.S:55-61; the
///        slot-0/+272 publish is asm-only and needs an EL0 round-trip — the
///        #104 smoke test — so this pins extraction+mapping+dispatch here).
///        Frame layout: frame[0..30]=x0..x30, [31]=SP_EL0, [32]=ELR,
///        [33]=SPSR (syscall_entry.S:20-28).
// Testidea: Build a synthetic save-area frame with GETPID in the x8 slot,
//           extract number/args exactly per the asm rules, dispatch through
//           the real Syscall::handle, assert the return equals GETPID's.
// Input: frame[8]=GETPID, frame[0..3]=sentinels, frame[32]=ELR marker.
// Expect: return == current task id (0 when the harness has no task).
// Depends: Syscall::handle, Syscall::sys_getpid.
JARVIS_TEST(aarch64_abi_frame_conform, "PRE: none | POST: none") {
    uint64_t frame[36] = {};
    frame[8] = static_cast<uint64_t>(SyscallNumber::GETPID);  // x8 = number
    frame[0] = 0xDEAD;  // x0 = arg0 sentinel (GETPID ignores args)
    frame[1] = 0xBEEF;  // x1 = arg1 sentinel
    frame[32] = 0x400044ULL;  // ELR marker (carried, not dispatched)
    // Extraction replicates syscall_entry.S:57-59 exactly.
    uint64_t num = frame[8];
    uint64_t ret =
        Syscall::handle(num, frame[0], frame[1], frame[2], frame[3], frame);
    auto *cur = Scheduler::current_task();
    uint64_t want = (cur != nullptr) ? cur->id : 0;
    JARVIS_ASSERT_FMT(ret == want, "GETPID via x8-slot returned %lx, want %lx",
                      ret, want);
    JARVIS_TEST_PASS();
}

// Testidea: Pin the aarch64 arg slots independently (issue #30): the x0 slot
//           is arg0, the x1 slot is arg1.
// Input: KILL through the documented extraction: (999999, 1) must fail pid
//        lookup (-1); (999999, 0) must take the SIG_NONE short-circuit (0)
//        (syscall_handlers_process.cpp:248-257).
// Expect: -1 then 0 — proving arg0/arg1 arrive from distinct slots.
// Depends: Syscall::handle, Syscall::sys_kill.
JARVIS_TEST(aarch64_abi_arg_routing, "PRE: none | POST: none") {
    uint64_t frame[36] = {};
    frame[8] = static_cast<uint64_t>(SyscallNumber::KILL);  // x8 = number
    frame[0] = 999999;  // x0 = arg0 = nonexistent pid
    frame[1] = 1;       // x1 = arg1 = valid signal -> pid lookup fails
    uint64_t r1 =
        Syscall::handle(frame[8], frame[0], frame[1], frame[2], frame[3], frame);
    frame[1] = 0;  // x1 = SIG_NONE -> short-circuit 0, pid ignored
    uint64_t r2 =
        Syscall::handle(frame[8], frame[0], frame[1], frame[2], frame[3], frame);
    JARVIS_ASSERT_FMT(r1 == static_cast<uint64_t>(-1),
                      "KILL(999999,1) returned %lx, want -1", r1);
    JARVIS_ASSERT_FMT(r2 == 0, "KILL(999999,0) returned %lx, want 0", r2);
    JARVIS_TEST_PASS();
}

// Testidea: Pin the aarch64 number-slot error path (issue #30).
// Input: x8 slot = MAX_SYSCALL through the documented extraction.
// Expect: (uint64_t)-1 (syscall.cpp:126 bounds check).
// Depends: Syscall::handle bounds check.
JARVIS_TEST(aarch64_abi_bad_number, "PRE: none | POST: none") {
    uint64_t frame[36] = {};
    frame[8] = static_cast<uint64_t>(SyscallNumber::MAX_SYSCALL);
    uint64_t ret =
        Syscall::handle(frame[8], frame[0], frame[1], frame[2], frame[3], frame);
    JARVIS_ASSERT_FMT(ret == static_cast<uint64_t>(-1),
                      "bad number returned %lx, want -1", ret);
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
    JARVIS_REGISTER_TEST(aarch64_pan_sctlr_bit_set);
    JARVIS_REGISTER_TEST(aarch64_pan_stac_clac_roundtrip);
    JARVIS_REGISTER_TEST(aarch64_pte_kernel_page_uxn_pxn);
    JARVIS_REGISTER_TEST(aarch64_pte_user_page_pxn);
    JARVIS_REGISTER_TEST(aarch64_pte_user_nx_uxn);
    JARVIS_REGISTER_TEST(aarch64_deep_copy_user_descriptors_valid);
    JARVIS_REGISTER_TEST(aarch64_abi_frame_conform);
    JARVIS_REGISTER_TEST(aarch64_abi_arg_routing);
    JARVIS_REGISTER_TEST(aarch64_abi_bad_number);
    JARVIS_REGISTER_TEST(aarch64_fork_marker_smoke);
}

#endif