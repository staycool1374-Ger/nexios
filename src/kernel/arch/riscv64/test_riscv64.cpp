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

/// @file test_riscv64.cpp
/// @brief Architecture-specific test suite for RISC-V64 (Sv39 page tables,
///        PLIC, SBI timer, context switching, RTC, PCI ECAM, CSRs).

#if defined(CONFIG_ARCH_RISCV64)

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
#include <kernel/arch/io.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/memory/vmm.hpp>
#include <lib/string.hpp>
#include <constants.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/elf/elf_loader.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/test/test_isolate.hpp>
#include <signal.hpp>

extern "C" {
extern const uint8_t _binary_fork_marker_img_start[];
extern const uint8_t _binary_fork_marker_img_end[];
extern uint64_t riscv64_last_u_scause();
extern uint64_t riscv64_last_u_stval();
extern uint64_t riscv64_last_u_sepc();
}

using namespace kernel;

/// @brief Walk the active Sv39 page table for a known kernel VA, verifying
///        valid entries at all three levels and leaf attributes.
JARVIS_TEST(riscv64_sv39_3level_walk) {
    uint64_t root_pa = arch::read_cr3();
    JARVIS_ASSERT_FMT(root_pa != 0, "SATP PA is 0");
    JARVIS_ASSERT_FMT((root_pa & 0xFFF) == 0, "SATP PA not page-aligned: 0x%lx",
                      root_pa);

    constexpr uint64_t VA = 0xFFFFFFC080207000ULL;
    constexpr uint64_t L0_SHIFT = 30, L1_SHIFT = 21, L2_SHIFT = 12;
    constexpr uint64_t TABLE_MASK = 0x1FF;
    constexpr uint64_t V = 1ULL << 0, R = 1ULL << 1, W = 1ULL << 2,
                       X = 1ULL << 3;
    constexpr uint64_t A = 1ULL << 6;
    constexpr uint64_t LEAF = R | W | X;

    uint64_t *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + root_pa);
    size_t l0_idx = (VA >> L0_SHIFT) & TABLE_MASK;
    uint64_t l0e = l0[l0_idx];
    JARVIS_ASSERT_FMT(l0e & V, "L0 entry %zu invalid: 0x%lx", l0_idx, l0e);
    JARVIS_ASSERT_FMT((l0e & LEAF) == 0, "L0 entry %zu has leaf bits: 0x%lx",
                      l0_idx, l0e);

    uint64_t l1_pa = (l0e & ~0xFFFULL) >> 10 << 12;
    uint64_t *l1 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l1_pa);
    size_t l1_idx = (VA >> L1_SHIFT) & TABLE_MASK;
    uint64_t l1e = l1[l1_idx];
    JARVIS_ASSERT_FMT(l1e & V, "L1 entry %zu invalid: 0x%lx", l1_idx, l1e);

    if ((l1e & LEAF) == 0) {
        uint64_t l2_pa = (l1e & ~0xFFFULL) >> 10 << 12;
        uint64_t *l2 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l2_pa);
        size_t l2_idx = (VA >> L2_SHIFT) & TABLE_MASK;
        uint64_t l2e = l2[l2_idx];
        JARVIS_ASSERT_FMT(l2e & V, "L2 entry %zu invalid: 0x%lx", l2_idx, l2e);
        JARVIS_ASSERT_FMT((l2e & LEAF) != 0, "L2 entry %zu not leaf: 0x%lx",
                          l2_idx, l2e);
        JARVIS_ASSERT_FMT(l2e & A, "L2 entry %zu A bit not set: 0x%lx", l2_idx,
                          l2e);
    } else {
        JARVIS_ASSERT_FMT(l1e & A, "L1 block entry %zu A bit not set: 0x%lx",
                          l1_idx, l1e);
    }

    JARVIS_TEST_PASS();
}

/// @brief Save/restore guard for the L1 block entry a test splits.
///        riscv64 HHDM RAM lives in 2MB block leaves; splitting one for a
///        map test then unmapping would destroy live linear-map entries
///        with no rewind on riscv (x86 relies on snapshot PD restore).
///        Capture values, restore the block leaf + sfence, THEN assert.
///        Usage: BlockGuard g(VA); ...ops...; vals...; g.restore(); asserts.
struct BlockGuard {
    uint64_t *l1 = nullptr;
    size_t idx = 0;
    uint64_t saved = 0;
    bool armed = false;
    explicit BlockGuard(uint64_t va) {
        uint64_t root = arch::read_cr3();
        auto *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + root);
        size_t l0_idx = (va >> 30) & 0x1FF;
        uint64_t l0e = l0[l0_idx];
        if (!(l0e & 1ULL))
            return;
        uint64_t l1_phys = ((l0e >> 10) << 12) & 0xFFFFFFFFFFF000ULL;
        l1 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + l1_phys);
        idx = (va >> 21) & 0x1FF;
        saved = l1[idx];
        armed = true;
    }
    void restore() {
        if (!armed)
            return;
        armed = false;
        l1[idx] = saved;
        asm volatile("sfence.vma" ::: "memory");
    }
};

/// @brief Map a single 4 KB page and verify get_physical matches, then
///        unmap and confirm the mapping is removed.
JARVIS_TEST(riscv64_sv39_map_unmap) {
    uint64_t phys = PMM::alloc_page();
    JARVIS_ASSERT_FMT(phys != 0, "PMM::alloc_page() returned 0");

    constexpr uint64_t VA = 0xFFFFFFC08043F000ULL;
    constexpr uint64_t FLAGS = PageFlags::PRESENT | PageFlags::WRITE;

    // BlockGuard (see block_split): without restoring the split L1 entry,
    // the leaked split-L2 page is freed by snapshot pool-rollback while
    // still referenced — a dangling table pointer serving wild reads to
    // later tests (observed: zeroed s_pages reads → phys-0 leaf → fault).
    BlockGuard guard(VA);
    auto r = arch::ArchPageTable::map_page(VA, phys, FLAGS);
    bool ok_map = r.ok();

    uint64_t retrieved = arch::ArchPageTable::get_physical(VA);

    auto ur = arch::ArchPageTable::unmap_page(VA);
    bool ok_unmap = ur.ok();

    uint64_t after = arch::ArchPageTable::get_physical(VA);
    guard.restore();
    PMM::free_page(phys);
    JARVIS_ASSERT(ok_map);
    JARVIS_ASSERT_EQ(phys, retrieved);
    JARVIS_ASSERT(ok_unmap);
    JARVIS_ASSERT_EQ(0ULL, after);

    JARVIS_TEST_PASS();
}

/// @brief Fill a 2 MB-aligned region with 512 page mappings, then overlay
///        a single page to trigger an L2 block-split scenario.
JARVIS_TEST(riscv64_sv39_block_split) {
    // Issue #206: scratch VA in an UNMAPPED L1 range (L1[128], beyond the
    // 256MB RAM window).  Mapping over live HHDM RAM (e.g. 0x80600000)
    // remaps the test's own .bss/PMM metadata underneath itself: L2[i]
    // installs redirect s_pages/bitmap reads to test pages (observed:
    // pages[492] reading 0 → phys-0 leaf → access fault).  L1[128] is
    // invalid at entry, so no split occurs (fresh L2 via get_table) and
    // the guard below restores invalid (not a block leaf).
    constexpr uint64_t BLOCK_VA = 0xFFFFFFC090000000ULL;
    constexpr uint64_t PAGE_VA = BLOCK_VA + 0x1000;
    constexpr uint64_t FLAGS = PageFlags::PRESENT | PageFlags::WRITE;

    // Static scratch (fully rewritten every run): 512-entry arrays would
    // consume 8KB of harness stack.  NOTE: these live in kernel .bss —
    // never map test pages over the kernel image ranges they occupy
    // (observed: self-remap zeroes your own reads).
    static uint64_t s_pages[512];
    static uint64_t s_check[512];
    uint64_t *pages = s_pages;
    uint64_t *check = s_check;
    bool setup_ok = true;
    for (size_t i = 0; i < 512; ++i) {
        pages[i] = PMM::alloc_page();
        if (pages[i] == 0)
            setup_ok = false;
    }

    BlockGuard guard(BLOCK_VA);
    bool map_ok = true;
    if (setup_ok) {
        for (size_t i = 0; i < 512; ++i) {
            auto r = arch::ArchPageTable::map_page(BLOCK_VA + i * 0x1000,
                                                   pages[i], FLAGS);
            if (!r.ok())
                map_ok = false;
        }
    }

    uint64_t new_p = PMM::alloc_page();
    bool overlay_ok = false;
    uint64_t retrieved = 0;
    if (new_p != 0 && map_ok) {
        auto r = arch::ArchPageTable::map_page(PAGE_VA, new_p, FLAGS);
        overlay_ok = r.ok();
        retrieved = arch::ArchPageTable::get_physical(PAGE_VA);
    }

    for (size_t i = 0; i < 512; ++i)
        check[i] = arch::ArchPageTable::get_physical(BLOCK_VA + i * 0x1000);

    for (size_t i = 0; i < 512; ++i) {
        arch::ArchPageTable::unmap_page(BLOCK_VA + i * 0x1000);
        PMM::free_page(pages[i]);
    }
    guard.restore();
    if (new_p != 0)
        PMM::free_page(new_p);

    JARVIS_ASSERT(setup_ok);
    JARVIS_ASSERT(map_ok);
    JARVIS_ASSERT(overlay_ok);
    JARVIS_ASSERT_EQ(new_p, retrieved);
    for (size_t i = 0; i < 512; ++i) {
        if (i == 1) {
            JARVIS_ASSERT_EQ(new_p, check[i]);
        } else {
            JARVIS_ASSERT(check[i] != 0);
        }
    }

    JARVIS_TEST_PASS();
}

/// @brief Save two contexts, then switch between them and verify sp values.
JARVIS_TEST(riscv64_context_save_restore) {
    arch::ArchContext ctx_a{};
    arch::ArchContext ctx_b{};

    uint64_t sp_a = 0xFFFFFFC010000000ULL;
    uint64_t sp_b = 0xFFFFFFC020000000ULL;

    arch::ArchContextManager::save(ctx_a, sp_a);
    arch::ArchContextManager::save(ctx_b, sp_b);

    JARVIS_ASSERT_EQ(ctx_a.sp, sp_a);
    JARVIS_ASSERT_EQ(ctx_b.sp, sp_b);

    uint64_t current_sp = sp_a;
    arch::ArchContextManager::switch_to(ctx_a, ctx_b, current_sp);
    JARVIS_ASSERT_EQ(current_sp, sp_b);
    JARVIS_ASSERT_EQ(ctx_a.sp, sp_a);

    arch::ArchContextManager::switch_to(ctx_b, ctx_a, current_sp);
    JARVIS_ASSERT_EQ(current_sp, sp_a);

    JARVIS_TEST_PASS();
}

/// @brief Verify that init_stack builds the expected sret frame layout
///        (sepc, user_sp, sstatus, and aligned padding).
JARVIS_TEST(riscv64_context_sret_frame) {
    uint64_t stack[1024];
    uint64_t *stack_top = stack + 1024;

    auto entry = []() {
        while (1) {
            arch::pause();
        }
    };
    constexpr uint64_t SSTATUS_SPIE = 1ULL << 5;
    uint64_t psr = SSTATUS_SPIE;
    uint64_t user_sp = 0xFFFFFFC030000000ULL;

    arch::ArchContextManager::init_stack(stack_top, entry, 0, 0, psr, user_sp);

    JARVIS_ASSERT(stack_top < stack + 1024);
    JARVIS_ASSERT(stack_top > stack);

    uint64_t *frame = stack_top;
    uint64_t sepc_val = reinterpret_cast<uint64_t>(+entry);
    JARVIS_ASSERT_EQ(frame[19], 0ULL);
    JARVIS_ASSERT_EQ(frame[18], sepc_val);
    JARVIS_ASSERT_EQ(frame[17], user_sp);
    JARVIS_ASSERT_EQ(frame[16], psr);
    JARVIS_ASSERT_EQ(frame[15], 0ULL);

    JARVIS_TEST_PASS();
}

/// @brief Initialize PLIC and verify threshold is 0 and at least one source
///        is enabled.
JARVIS_TEST(riscv64_plic_init) {
    arch::ArchInterruptController::init();

    volatile uint32_t *threshold =
        reinterpret_cast<volatile uint32_t *>(0x0C200000ULL);
    uint32_t thresh_val = *threshold;
    JARVIS_ASSERT_FMT(thresh_val == 0, "PLIC threshold not 0: 0x%x",
                      thresh_val);

    // Issue #205: init() intentionally enables nothing (stays disabled
    // comment in interrupt_controller.cpp); enable IRQ 10 in-test so the
    // enable-path assertion below tests the mask/unmask registers.
    arch::ArchInterruptController::unmask(10);
    volatile uint32_t *enable =
        reinterpret_cast<volatile uint32_t *>(0x0C002000ULL);
    uint32_t enable_val = enable[0];
    JARVIS_ASSERT_FMT(enable_val != 0, "No PLIC interrupt sources enabled");

    JARVIS_TEST_PASS();
}

/// @brief Mask and unmask IRQ 10, verifying the enable register state.
JARVIS_TEST(riscv64_plic_mask_unmask) {
    arch::ArchInterruptController::mask(10);

    volatile uint32_t *enable =
        reinterpret_cast<volatile uint32_t *>(0x0C002000ULL);
    uint32_t en = enable[0];
    JARVIS_ASSERT_FMT((en & (1U << 10)) == 0, "IRQ 10 not masked: enable=0x%x",
                      en);

    arch::ArchInterruptController::unmask(10);
    en = enable[0];
    JARVIS_ASSERT_FMT((en & (1U << 10)) != 0,
                      "IRQ 10 not unmasked: enable=0x%x", en);

    JARVIS_TEST_PASS();
}

/// @brief Write EOI for IRQ 10 and IRQ 1, then verify claim register is 0.
JARVIS_TEST(riscv64_plic_claim_complete) {
    arch::ArchInterruptController::eoi(10);

    volatile uint32_t *claim =
        reinterpret_cast<volatile uint32_t *>(0x0C200004ULL);
    uint32_t pending = *claim;
    JARVIS_ASSERT_FMT(pending == 0, "PLIC claim non-zero after EOI: %u",
                      pending);

    arch::ArchInterruptController::eoi(1);
    pending = *claim;
    JARVIS_ASSERT_FMT(pending == 0, "PLIC claim non-zero after EOI (IRQ1): %u",
                      pending);

    JARVIS_TEST_PASS();
}

/// @brief Set a future timer via SBI ecall and verify mtime is monotonic.
JARVIS_TEST(riscv64_sbi_timer_set_stime) {
    uint64_t mtime{};
    asm volatile("csrr %0, time" : "=r"(mtime));

    uint64_t future = mtime + 100000;
    uint64_t ret{};
    asm volatile("mv a0, %1; li a7, 0; ecall; mv %0, a0"
                 : "=r"(ret)
                 : "r"(future)
                 : "a0", "a7", "memory");
    JARVIS_ASSERT_EQ(ret, 0ULL);

    uint64_t mtime2{};
    asm volatile("csrr %0, time" : "=r"(mtime2));
    JARVIS_ASSERT_FMT(mtime2 >= mtime, "mtime not monotonic: %lu -> %lu", mtime,
                      mtime2);

    JARVIS_TEST_PASS();
}

/// @brief Read timer ticks in a loop and verify they are monotonic.
JARVIS_TEST(riscv64_timer_ticks_monotonic) {
    uint64_t prev = arch::Timer::ticks();
    for (int i = 0; i < 10; ++i) {
        uint64_t curr = arch::Timer::ticks();
        JARVIS_ASSERT_FMT(curr >= prev,
                          "ticks not monotonic: prev=%lu, curr=%lu", prev,
                          curr);
        prev = curr;
        for (int j = 0; j < 1000; ++j) {
            asm volatile("");
        }
    }

    JARVIS_TEST_PASS();
}

/// @brief Read ns-precision time, spin, and verify delta is positive and
///        within reasonable bounds.
JARVIS_TEST(riscv64_timer_ns_conversion) {
    uint64_t t0 = arch::Timer::ns();
    for (int i = 0; i < 1000000; ++i) {
        asm volatile("");
    }
    uint64_t t1 = arch::Timer::ns();

    uint64_t delta = t1 - t0;
    JARVIS_ASSERT_FMT(delta > 0, "Timer delta <= 0: %lu", delta);
    JARVIS_ASSERT_FMT(delta < 10000000000ULL, "Timer delta too large: %lu ns",
                      delta);

    JARVIS_TEST_PASS();
}

/// @brief Check MISA for F and D extensions and verify arch::has_fpu().
JARVIS_TEST(riscv64_fpu_extension_detection) {
    uint64_t misa{};
    asm volatile("csrr %0, misa" : "=r"(misa));

    bool has_f = (misa & (1ULL << ('F' - 'A'))) != 0;
    bool has_d = (misa & (1ULL << ('D' - 'A'))) != 0;
    JARVIS_ASSERT_FMT(has_f, "MISA F extension (bit %u) not set: 0x%lx",
                      (unsigned)('F' - 'A'), misa);
    JARVIS_ASSERT_FMT(has_d, "MISA D extension (bit %u) not set: 0x%lx",
                      (unsigned)('D' - 'A'), misa);

    arch::CpuIdResult cpuid_result = arch::cpuid(0);
    (void)cpuid_result;

    bool fpu_detected = arch::has_fpu();
    JARVIS_ASSERT_FMT(fpu_detected, "arch::has_fpu() returned false");

    JARVIS_TEST_PASS();
}

/// @brief Write characters via SBI serial and check LSR TEMT status.
JARVIS_TEST(riscv64_sbi_console_putchar) {
    arch::Serial::putchar('R');

    volatile uint8_t *lsr = reinterpret_cast<volatile uint8_t *>(0x10000005ULL);
    uint8_t lsr_val = *lsr;
    JARVIS_ASSERT_FMT(lsr_val & (1 << 6),
                      "TEMT not set after putchar: LSR=0x%x", lsr_val);

    for (char c = 0x20; c <= 0x7E; ++c) {
        arch::Serial::putchar(c);
    }

    lsr_val = *lsr;
    JARVIS_ASSERT_FMT(lsr_val & (1 << 6),
                      "TEMT not set after bulk puts: LSR=0x%x", lsr_val);

    JARVIS_TEST_PASS();
}

/// @brief Read vendor and device ID from PCI bus 0 device 0 function 0 via
/// ECAM.
JARVIS_TEST(riscv64_pci_ecam_read) {
    arch::PciBdf bdf{0, 0, 0};
    uint16_t vendor = arch::pci_read_vendor(bdf);
    uint16_t device = arch::pci_read_device(bdf);

    JARVIS_ASSERT_FMT(vendor != 0xFFFF,
                      "PCI Vendor ID 0xFFFF (no device at 0:0:0)");
    JARVIS_ASSERT_FMT(device != 0x0000, "PCI Device ID 0x0000 at 0:0:0");

    JARVIS_TEST_PASS();
}

/// @brief Read RTC time twice with a small delay and verify the clock
///        advances monotonically.
JARVIS_TEST(riscv64_rtc_mtime_read) {
    arch::RTC::read_seconds();

    arch::tm t1{};
    arch::RTC::read_time(&t1);

    uint16_t year1 = t1.tm_year + 1900;
    // Issue #205: the riscv64 RTC is an mtime uptime clock (epoch 1970),
    // not a wall clock — pin the uptime-epoch contract + monotonicity.
    // A goldfish-rtc wall-clock driver is follow-up work.
    JARVIS_ASSERT_FMT(year1 == 1970, "RTC epoch not 1970: %u", year1);

    for (int i = 0; i < 100000; ++i) {
        asm volatile("");
    }

    arch::tm t2{};
    arch::RTC::read_time(&t2);
    uint64_t secs1 = t1.tm_hour * 3600 + t1.tm_min * 60 + t1.tm_sec;
    uint64_t secs2 = t2.tm_hour * 3600 + t2.tm_min * 60 + t2.tm_sec;
    JARVIS_ASSERT_FMT(secs2 >= secs1, "RTC time regressed: %lu -> %lu", secs1,
                      secs2);

    JARVIS_TEST_PASS();
}

/// @brief Verify SATP CSR mode is Sv39 (8), and round-trip write_cr3/read_cr3.
JARVIS_TEST(riscv64_satp_csr) {
    uint64_t satp_pa = arch::read_cr3();
    JARVIS_ASSERT_FMT(satp_pa != 0, "SATP PA is 0");
    JARVIS_ASSERT_FMT((satp_pa & 0xFFF) == 0, "SATP PA not page-aligned: 0x%lx",
                      satp_pa);

    uint64_t satp{};
    asm volatile("csrr %0, satp" : "=r"(satp));
    uint64_t mode = (satp >> 60) & 0xF;
    JARVIS_ASSERT_FMT(mode == 8, "SATP MODE not Sv39: %lu (expected 8)", mode);

    arch::write_cr3(satp_pa);
    uint64_t reread = arch::read_cr3();
    JARVIS_ASSERT_EQ(satp_pa, reread);

    arch::ArchPageTable::tlb_flush_all();

    JARVIS_TEST_PASS();
}

/// @brief Read vendor ID, arch ID, and implementation ID CSRs — verify
///        marchid is non-zero.
JARVIS_TEST(riscv64_boot_mvendorid) {
    uint64_t mvendorid{};
    asm volatile("csrr %0, mvendorid" : "=r"(mvendorid));

    uint64_t marchid{};
    asm volatile("csrr %0, marchid" : "=r"(marchid));
    JARVIS_ASSERT_FMT(marchid != 0, "marchid is 0");

    uint64_t mimpid{};
    asm volatile("csrr %0, mimpid" : "=r"(mimpid));

    (void)mvendorid;
    (void)mimpid;

    JARVIS_TEST_PASS();
}

/// @brief Verify that U-mode ecall (bit 8) and timer interrupts (bit 5)
///        are delegated to S-mode via medeleg/mideleg.
JARVIS_TEST(riscv64_medeleg_selected) {
    uint64_t medeleg{};
    asm volatile("csrr %0, medeleg" : "=r"(medeleg));
    JARVIS_ASSERT_FMT(medeleg & (1ULL << 8),
                      "medeleg[8] (ecall U-mode) not set: 0x%lx", medeleg);

    uint64_t mideleg{};
    asm volatile("csrr %0, mideleg" : "=r"(mideleg));
    JARVIS_ASSERT_FMT(mideleg & (1ULL << 5),
                      "mideleg[5] (timer) not set: 0x%lx", mideleg);

    JARVIS_TEST_PASS();
}

/// @brief Pin the riscv64 ecall register convention, dispatch half (issue #30):
///        the saved-a7 slot carries the number, saved a0-a3 carry args, and
///        the handler return is the a0 value (syscall_entry.S:164-171; the
///        OFF_A0-slot store is asm-only and needs a U-mode round-trip — the
///        #29 runtime test — so this pins extraction+mapping+dispatch here).
///        Slot indices are qword offsets of the OFF_* values
///        (syscall_entry.S:32-39: A0=72, A1=80, A2=88, A3=96, A7=128).
// Testidea: Build a synthetic trap frame with GETPID in the a7 slot,
//           extract number/args exactly per the asm rules, dispatch through
//           the real Syscall::handle, assert the return equals GETPID's.
// Input: frame[16]=GETPID, frame[9..10]=sentinels.
// Expect: return == current task id (0 when the harness has no task).
// Depends: Syscall::handle, Syscall::sys_getpid.
JARVIS_TEST(riscv64_abi_frame_conform, "PRE: none | POST: none") {
    uint64_t frame[36] = {};
    frame[16] = static_cast<uint64_t>(SyscallNumber::GETPID);  // a7 = number
    frame[9] = 0xDEAD;   // a0 = arg0 sentinel (GETPID ignores args)
    frame[10] = 0xBEEF;  // a1 = arg1 sentinel
    // Extraction replicates syscall_entry.S:164-168 exactly.
    uint64_t num = frame[16];
    uint64_t ret =
        Syscall::handle(num, frame[9], frame[10], frame[11], frame[12], frame);
    auto *cur = Scheduler::current_task();
    uint64_t want = (cur != nullptr) ? cur->id : 0;
    JARVIS_ASSERT_FMT(ret == want, "GETPID via a7-slot returned %lx, want %lx",
                      ret, want);
    JARVIS_TEST_PASS();
}

// Testidea: Pin the riscv64 arg slots independently (issue #30): the a0 slot
//           is arg0, the a1 slot is arg1.
// Input: KILL through the documented extraction: (999999, 1) must fail pid
//        lookup (-1); (999999, 0) must take the SIG_NONE short-circuit (0)
//        (syscall_handlers_process.cpp:248-257).
// Expect: -1 then 0 — proving arg0/arg1 arrive from distinct slots.
// Depends: Syscall::handle, Syscall::sys_kill.
JARVIS_TEST(riscv64_abi_arg_routing, "PRE: none | POST: none") {
    uint64_t frame[36] = {};
    frame[16] = static_cast<uint64_t>(SyscallNumber::KILL);  // a7 = number
    frame[9] = 999999;   // a0 = arg0 = nonexistent pid
    frame[10] = 1;       // a1 = arg1 = valid signal -> pid lookup fails
    uint64_t r1 = Syscall::handle(frame[16], frame[9], frame[10], frame[11],
                                  frame[12], frame);
    frame[10] = 0;  // a1 = SIG_NONE -> short-circuit 0, pid ignored
    uint64_t r2 = Syscall::handle(frame[16], frame[9], frame[10], frame[11],
                                  frame[12], frame);
    JARVIS_ASSERT_FMT(r1 == static_cast<uint64_t>(-1),
                      "KILL(999999,1) returned %lx, want -1", r1);
    JARVIS_ASSERT_FMT(r2 == 0, "KILL(999999,0) returned %lx, want 0", r2);
    JARVIS_TEST_PASS();
}

// Testidea: Pin the riscv64 number-slot error path (issue #30).
// Input: a7 slot = MAX_SYSCALL through the documented extraction.
// Expect: (uint64_t)-1 (syscall.cpp:126 bounds check).
// Depends: Syscall::handle bounds check.
JARVIS_TEST(riscv64_abi_bad_number, "PRE: none | POST: none") {
    uint64_t frame[36] = {};
    frame[16] = static_cast<uint64_t>(SyscallNumber::MAX_SYSCALL);
    uint64_t ret = Syscall::handle(frame[16], frame[9], frame[10], frame[11],
                                   frame[12], frame);
    JARVIS_ASSERT_FMT(ret == static_cast<uint64_t>(-1),
                      "bad number returned %lx, want -1", ret);
    JARVIS_TEST_PASS();
}

// Issue #206 (M1): synthetic U-mode ECALL round-trip without any ELF.
// A 48-byte raw stub (post-ecall x2 regression sample for issue #220).
// Store cookie / GETPID ecall / store cookie /
// spin runs in U-mode from private user mappings.
// runs in U-mode from private user mappings; the harness observes both
// cookies through the HHDM alias of the data page.  Cookie 1 proves the
// sret entry (fetch + store in U-mode); cookie 2 proves the ECALL trap
// AND the sret return (without the Phase-1 trap fixes this faults or
// wedges: no kstack switch, no x2 restore).
namespace {
// RV64LE.  M1 U-mode ECALL probe: tp carries the data-page base
// (x2 untouched after dispatch; tp is callee-saved and restored
// unconditionally).  CAREFUL: tp=x4 (rd/rs1=4), NOT t0=x5!
// All multi-byte encodings below were verified with the python RV decoder
// (lesson: never hand-commit without decoding).
//   lui tp,0x41001        -> tp = 0x41001000 (data page)
//   addi t1,zero,0x111    -> cookie 1
//   sw t1,0(tp)
//   mv t1,x2              -> snapshot live user-x2 pre-ecall (dispatch proof)
//   sw t1,8(tp)           -> cookie 3 (expect 0x70009000)
//   addi a7,zero,23       -> GETPID
//   ecall                 -> at +0x18
//   mv t1,x2              -> snapshot live user-x2 POST-ecall (issue #220:
//                            trap-round-trip x2 preservation proof)
//   sw t1,12(tp)          -> cookie 4 (expect 0x70009000)
//   addi t1,zero,0x222    -> cookie 2
//   sw t1,4(tp)
//   jal x0,0              -> spin at +0x2C
constexpr uint8_t kUmodeProbeStub[] = {
    0x37, 0x12, 0x00, 0x41, 0x13, 0x03, 0x10, 0x11, 0x23, 0x20, 0x62, 0x00,
    0x13, 0x03, 0x01, 0x00, 0x23, 0x24, 0x62, 0x00, 0x93, 0x08, 0x70, 0x01,
    0x73, 0x00, 0x00, 0x00, 0x13, 0x03, 0x01, 0x00, 0x23, 0x26, 0x62, 0x00,
    0x13, 0x03, 0x20, 0x22, 0x23, 0x22, 0x62, 0x00, 0x6F, 0x00, 0x00, 0x00,
};
constexpr uint64_t kUmodeStubVa = 0x41000000ULL;
constexpr uint64_t kUmodeDataVa = 0x41001000ULL;
constexpr uint32_t kUmodeCookie1 = 0x111;
constexpr uint32_t kUmodeCookie2 = 0x222;
}  // namespace

JARVIS_TEST(riscv64_umode_ecall_smoke, "PRE: none | POST: none") {
    using namespace kernel;
    const uint64_t free_before = PMM::free_pages_ref();

    // Private stub (R+X) + data (R+W) pages, USER-owned so cleanup
    // reclaims them via free_user_pages.
    const uint64_t stub_phys = PMM::alloc_user_page();
    const uint64_t data_phys = PMM::alloc_user_page();
    JARVIS_ASSERT_FMT(stub_phys != 0 && data_phys != 0, "user alloc failed");

    auto *stub = reinterpret_cast<volatile uint8_t *>(arch::HHDM_OFFSET +
                                                      stub_phys);
    for (size_t i = 0; i < sizeof(kUmodeProbeStub); ++i)
        stub[i] = kUmodeProbeStub[i];
    auto *data = reinterpret_cast<volatile uint32_t *>(arch::HHDM_OFFSET +
                                                        data_phys);
    data[0] = 0;
    data[1] = 0;
    data[2] = 0;
    data[3] = 0;  // cookie 4 slot (issue #220)

    auto *t = TaskControlBlock::create_user(
        reinterpret_cast<void (*)()>(kUmodeStubVa), 11, 10, 32_KiB);
    JARVIS_ASSERT_FMT(t != nullptr, "create_user failed");
    JARVIS_ASSERT(Scheduler::set_sched_policy(*t, SchedPolicy::FIXED));

    // Map our pages into the task's tables (create_user maps its own
    // stack + yield stub elsewhere; our VAs don't collide).
    VMM::map_page_in_pml4(kUmodeStubVa, stub_phys, true, true,
                          t->page_table_);
    VMM::map_page_in_pml4(kUmodeDataVa, data_phys, true, false,
                          t->page_table_);
    // Point the initial frame past create_user's yield stub.
    auto *frame = reinterpret_cast<uint64_t *>(t->context.sp);
    frame[31] = kUmodeStubVa;  // SEPC slot (OFF_SEPC=248, idx 31)

    // Fail fast if the map didn't land (else the U-fetch fault below
    // misattributes a mapping bug to the trap path).
    JARVIS_ASSERT_FMT(
        VMM::virt_to_phys_in_pml4(kUmodeStubVa, t->page_table_) == stub_phys,
        "stub VA not mapped in task tables");
    JARVIS_ASSERT_FMT(
        VMM::virt_to_phys_in_pml4(kUmodeDataVa, t->page_table_) == data_phys,
        "data VA not mapped in task tables");

    {
        arch::IrqGuard ig{};
        Scheduler::add_task(*t);
    }
    Scheduler::reschedule();

    // ecall sits at stub+0x18, post-advance +0x1C, spin at +0x2C.
    const uint64_t child_id = t->id;
    const uint64_t start = arch::Timer::ticks();
    uint64_t sepc_now = 0;
    uint64_t a0_now = 0;
    uint64_t x2_live = 0;
    uint64_t x2_post = 0;  // post-ecall x2 (issue #220)
    bool cookies_ok = false;
    while (arch::Timer::ticks() - start < 200) {
        // Handler-side progress (proves trap+return even before cookies).
        // Captured pre-teardown: frame dangles after delete t (Rule 5).
        sepc_now = frame[31];
        a0_now = frame[9];
        // Live x2 sampled by the stub itself (mv t1,x2 pre-ecall),
        // race-free: the data page is written once by U-mode and never
        // touched by the kernel, unlike frame[1] which every tick entry
        // rewrites (return-path SIE window, issue #206 follow-up).
        x2_live = data[2];
        x2_post = data[3];
        // Cookie round trip in the data page (proves tp/stores, no x2).
        if (data[0] == kUmodeCookie1 && data[1] == kUmodeCookie2) {
            cookies_ok = true;
            break;
        }
        arch::hlt();
    }

    // Teardown BEFORE asserting (cookbook Rule 5): terminate (spinning
    // in U-mode), drain, release user resources, free the TCB.
    if (TaskControlBlock::is_valid(t) &&
        t->state != TaskState::TERMINATED)
        (void)Scheduler::terminate_err(*t, 0);
    Scheduler::drain_zombie_list();
    if (TaskControlBlock::is_valid(t)) {
        t->cleanup();
        delete t;
    }
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_FMT(sepc_now >= kUmodeStubVa + 0x1C &&
                          sepc_now <= kUmodeStubVa + 0x2C,
                      "sepc not past ecall: 0x%lx", sepc_now);
    JARVIS_ASSERT_FMT(cookies_ok, "cookie round trip failed");
    JARVIS_ASSERT_FMT(a0_now == child_id, "a0 != task id: 0x%lx", a0_now);
    // Dispatch-x2 proof via the stub's own sample (frame[1] is not a
    // stable dispatch record: tick entries rewrite the live frame; see
    // the post-ecall-x2 follow-up issue for the remaining question).
    JARVIS_ASSERT_FMT(x2_live == 0x70009000ULL, "live x2 != user_rsp: 0x%lx",
                      x2_live);
    // Trap-round-trip x2 proof (issue #220): entry preserved x2, E2 restored it.
    JARVIS_ASSERT_FMT(x2_post == 0x70009000ULL, "post-trap x2 != user_rsp: 0x%lx",
                      x2_post);
    JARVIS_ASSERT_FMT(PMM::free_pages_ref() == free_before,
                      "PMM delta %ld pages after umode smoke",
                      (long)(PMM::free_pages_ref() - free_before));
    JARVIS_TEST_PASS();
}

// Issue #206 M2: e_machine gate — EM_RISCV accepted, x86_64/AArch64 rejected.
// Runmode: kernel
// Testidea: Synthetic headers differing only in machine field.
// Input: ET_EXEC headers with machine 0xF3 / 0x3E / 0xB7.
// Expect: validate true / false / false.
// Depends: elf::validate_header.
JARVIS_TEST(riscv64_elf_bad_machine, "PRE: none | POST: none") {
    auto make_hdr = [](uint16_t machine) {
        elf::ELF64Header hdr{};
        hdr.ident[0] = 0x7F;
        hdr.ident[1] = 'E';
        hdr.ident[2] = 'L';
        hdr.ident[3] = 'F';
        hdr.ident[4] = 2;
        hdr.ident[5] = 1;
        hdr.ident[6] = 1;
        hdr.type = elf::ET_EXEC;
        hdr.machine = machine;
        hdr.version = 1;
        hdr.entry = 0x41000000ULL;
        hdr.phoff = 0;
        hdr.ehsize = sizeof(elf::ELF64Header);
        hdr.phentsize = sizeof(elf::ELF64ProgramHeader);
        hdr.phnum = 0;
        return hdr;
    };
    elf::ELF64Header riscv = make_hdr(0xF3);
    elf::ELF64Header x86 = make_hdr(0x3E);
    elf::ELF64Header arm = make_hdr(0xB7);
    JARVIS_ASSERT_FMT(elf::validate_header(&riscv), "EM_RISCV rejected");
    JARVIS_ASSERT_FMT(!elf::validate_header(&x86), "x86_64 accepted");
    JARVIS_ASSERT_FMT(!elf::validate_header(&arm), "AArch64 accepted");
    JARVIS_TEST_PASS();
}

// Issue #206 M2: 37-slot fork convention (OFF_SP=idx1, SEPC=idx31,
// SSTATUS=idx32=0x20) + A0-zero child return.
// Runmode: kernel
// Testidea: Helper-built frame cloned from the harness task.
// Input: make_synthetic_clone_frame(entry, stack) with A0 sentinel.
// Expect: Child frame echoes SEPC/SSTATUS/SP, A0 == 0.
// Depends: make_synthetic_clone_frame, TaskControlBlock::clone.
JARVIS_TEST(riscv64_clone_frame_readback, "PRE: none | POST: none") {
    using namespace kernel;
    auto *parent = Scheduler::current_task();
    JARVIS_ASSERT(parent != nullptr);
    static uint64_t regs[37];
    test::make_synthetic_clone_frame(regs, 0x41000000ULL, 0x70009000ULL);
    regs[9] = 0xDEADULL;
    auto *c = TaskControlBlock::clone(regs);
    JARVIS_ASSERT(c != nullptr);
    const auto *f = reinterpret_cast<const uint64_t *>(c->context.sp);
    JARVIS_ASSERT_FMT(f[31] == 0x41000000ULL, "SEPC not echoed: 0x%lx", f[31]);
    JARVIS_ASSERT_FMT(f[32] == (1ULL << 5), "SSTATUS not U-mode: 0x%lx",
                      f[32]);
    JARVIS_ASSERT_FMT(f[1] == 0x70009000ULL, "OFF_SP not echoed: 0x%lx", f[1]);
    JARVIS_ASSERT_FMT(f[9] == 0ULL, "child A0 not zero: 0x%lx", f[9]);
    kernel::test::terminate_and_drain(*c);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Issue #206 M2: exec slot convention — SEPC/SP/SSTATUS/A0 land on the live
// trap frame (OFF_* indices shared with the M1 ABI tests).
// Runmode: kernel
// Testidea: exec_into_current on a create_user task with a minimal
//           phnum=0 RISC-V image; read back the live frame slots.
// Input: Synthetic EM_RISCV header (entry 0x41000000), empty file data.
// Expect: regs[31]==entry, regs[32]==0x20, regs[1]==user_rsp, regs[9]==0.
// Depends: elf::exec_into_current, ScopedCurrentTask.
JARVIS_TEST(riscv64_exec_slots, "PRE: none | POST: none") {
    using namespace kernel;
    elf::ELF64Header hdr{};
    hdr.ident[0] = 0x7F;
    hdr.ident[1] = 'E';
    hdr.ident[2] = 'L';
    hdr.ident[3] = 'F';
    hdr.ident[4] = 2;
    hdr.ident[5] = 1;
    hdr.ident[6] = 1;
    hdr.type = elf::ET_EXEC;
    hdr.machine = 0xF3;
    hdr.version = 1;
    hdr.entry = 0x41000000ULL;
    hdr.phoff = 0;
    hdr.ehsize = sizeof(elf::ELF64Header);
    hdr.phentsize = sizeof(elf::ELF64ProgramHeader);
    hdr.phnum = 0;
    static uint8_t file_data[64] = {};
    __builtin_memcpy(file_data, &hdr, sizeof(hdr));
    auto *t = TaskControlBlock::create_user(
        reinterpret_cast<void (*)()>(0x41000000ULL), 11, 10, 32_KiB);
    JARVIS_ASSERT(t != nullptr);
    {
        test::ScopedCurrentTask guard(*t);
        static uint64_t regs[37] = {};
        for (int i = 0; i < 37; ++i)
            regs[i] = 0xAAAA0000ULL + static_cast<uint64_t>(i);
        const bool ok =
            elf::exec_into_current(&hdr, file_data, nullptr, nullptr, regs,
                                   sizeof(file_data));
        JARVIS_ASSERT_FMT(ok, "exec_into_current failed");
        JARVIS_ASSERT_FMT(regs[31] == 0x41000000ULL, "SEPC slot: 0x%lx",
                          regs[31]);
        JARVIS_ASSERT_FMT(regs[32] == (1ULL << 5), "SSTATUS slot: 0x%lx",
                          regs[32]);
        JARVIS_ASSERT_FMT(regs[9] == 0ULL, "A0 slot not zero: 0x%lx", regs[9]);
        // SP slot: setup_user_stack returns post-argc SP (not page-aligned
        // by construction) — verify it was overwritten into the new user
        // stack window instead.
        JARVIS_ASSERT_FMT(
            regs[1] != 0xAAAA0001ULL && regs[1] >= mem::STACK_VADDR &&
                regs[1] < mem::STACK_VADDR + 2 * mem::STACK_SIZE,
            "SP slot not in user stack window: 0x%lx", regs[1]);
    }
    if (TaskControlBlock::is_valid(t)) {
        t->cleanup();
        delete t;
    }
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Issue #206 M2: U-mode fault terminates (no panic/park). Poison the loader-
// valid SEPC with an unmapped user VA; fetch faults at U-mode → dispatch →
// riscv64_u_fault_handler marks TERMINATED/-SIGSEGV and switches away.
// Runmode: kernel
// Testidea: create_user task, poison frame[31], dispatch at prio 11.
// Input: Unmapped user VA 0x5000000000 in SEPC slot.
// Expect: TERMINATED, exit -SIGSEGV, latched scause != 0.
// Depends: trap dispatch, riscv64_u_fault_handler, waitpid wake path.
JARVIS_TEST(riscv64_u_fault_terminates, "PRE: none | POST: none") {
    using namespace kernel;
    auto *t = TaskControlBlock::create_user(
        reinterpret_cast<void (*)()>(0x41000000ULL), 11, 10, 32_KiB);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*t, SchedPolicy::FIXED));
    auto *frame = reinterpret_cast<uint64_t *>(t->context.sp);
    JARVIS_ASSERT(frame != nullptr);
    frame[31] = 0x5000000000ULL;
    {
        arch::IrqGuard ig{};
        Scheduler::add_task(*t);
    }
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT_FMT(t->state == TaskState::TERMINATED,
                      "U-fault did not terminate the task");
    JARVIS_ASSERT_FMT(
        t->exit_code ==
            static_cast<uint64_t>(-static_cast<int64_t>(Signal::SIGSEGV)),
        "exit code is not -SIGSEGV: 0x%lx", t->exit_code);
    JARVIS_ASSERT_FMT(riscv64_last_u_scause() != 0, "fault scause not latched");
    if (TaskControlBlock::is_valid(t) &&
        t != Scheduler::current_task())
        (void)Scheduler::terminate_err(*t, t->exit_code);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

namespace {
constexpr const char *kM2ImagePath = "/tmp/riscv64_m2_probe.elf";

uint64_t m2_write_file(const char *path, const uint8_t *data, size_t size) {
    kernel::test::mark_vfs_touched();
    if (size == 0)
        return 0;
    if (kernel::vfs::create(path, 0) != 0)
        return 0;
    kernel::vfs::Vnode *file = kernel::vfs::resolve(path);
    if (file == nullptr || file->ops == nullptr ||
        file->ops->write == nullptr) {
        kernel::test::mark_vfs_touched();
        kernel::vfs::unlink(path);
        return 0;
    }
    size_t off = 0;
    while (off < size) {
        size_t chunk = size - off;
        if (chunk > 4096)
            chunk = 4096;
        int64_t written = file->ops->write(*file, data + off, chunk, off);
        if (written <= 0) {
            kernel::test::mark_vfs_touched();
            kernel::vfs::unlink(path);
            return 0;
        }
        off += static_cast<uint64_t>(written);
    }
    return (off == size) ? size : 0ULL;
}
}  // namespace

// Issue #206 M2 acceptance: a real riscv64 user ELF (fork-marker) loads via
// ElfLoader (EM_RISCV + 37-qword finalize) and runs to ECALL in U-mode —
// fork/waitpid/exit/write all trap through the M1 path and exit clean.
// Runmode: kernel
// Testidea: Stage fork-marker to tmpfs, ElfLoader-load, dispatch at prio 11.
// Input: Embedded _binary_fork_marker_img (riscv64 build).
// Expect: TERMINATED with exit 0 (positive U-mode execution marker).
// Depends: ElfLoader, tmpfs, finalize, trap ECALL, scheduler dispatch.
JARVIS_TEST(riscv64_elf_ecall_smoke, "PRE: none | POST: none") {
    using namespace kernel;
    size_t img_size = static_cast<size_t>(_binary_fork_marker_img_end -
                                          _binary_fork_marker_img_start);
    JARVIS_ASSERT_FMT(img_size != 0, "fork-marker image missing");
    elf::ElfLoader::reset();
    JARVIS_ASSERT_FMT(
        m2_write_file(kM2ImagePath, _binary_fork_marker_img_start, img_size) !=
            0,
        "tmpfs stage failed");
    JARVIS_ASSERT_FMT(
        elf::ElfLoader::request_load(kM2ImagePath) == elf::LoadResult::OK,
        "request_load failed");
    elf::ElfLoader::wait_loader_idle();
    TaskControlBlock *t = elf::ElfLoader::take_completed();
    JARVIS_ASSERT_FMT(t != nullptr && t->page_table_ != 0 && t->is_user_,
                      "take_completed failed");
    t->priority = 11;
    t->base_priority = 11;
    {
        arch::IrqGuard ig{};
        Scheduler::add_task(*t);
    }
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    const bool is_term = (t->state == TaskState::TERMINATED);
    const uint64_t exit_code = t->exit_code;
    const uint64_t fault_scause = riscv64_last_u_scause();
    const uint64_t fault_stval = riscv64_last_u_stval();
    const uint64_t fault_sepc = riscv64_last_u_sepc();
    if (TaskControlBlock::is_valid(t) &&
        t != Scheduler::current_task())
        (void)Scheduler::terminate_err(*t, t->exit_code);
    Scheduler::drain_zombie_list();
    kernel::test::mark_vfs_touched();
    kernel::vfs::unlink(kM2ImagePath);
    JARVIS_ASSERT_FMT(is_term, "riscv64 ELF task not TERMINATED");
    JARVIS_ASSERT_FMT(exit_code == 0,
                      "riscv64 ELF exit=0x%lx scause=0x%lx stval=0x%lx "
                      "sepc=0x%lx",
                      exit_code, fault_scause, fault_stval, fault_sepc);
    JARVIS_TEST_PASS();
}

/// @brief Register all riscv64 architecture tests.
void register_riscv64_tests() {
    Logger::info("Registering riscv64 architecture tests");

    JARVIS_REGISTER_TEST(riscv64_sv39_3level_walk);
    // Issue #206: re-enabled — Sv39 backend fixed (shift codec); these two
    // are the proof (were #205-deferred to #152).
    JARVIS_REGISTER_TEST(riscv64_sv39_map_unmap);
    JARVIS_REGISTER_TEST(riscv64_sv39_block_split);
    JARVIS_REGISTER_TEST(riscv64_context_save_restore);
    JARVIS_REGISTER_TEST(riscv64_context_sret_frame);
    JARVIS_REGISTER_TEST(riscv64_plic_init);
    JARVIS_REGISTER_TEST(riscv64_plic_mask_unmask);
    JARVIS_REGISTER_TEST(riscv64_plic_claim_complete);
    JARVIS_REGISTER_TEST(riscv64_sbi_timer_set_stime);
    JARVIS_REGISTER_TEST(riscv64_timer_ticks_monotonic);
    JARVIS_REGISTER_TEST(riscv64_timer_ns_conversion);
    // Issue #205: deregistered — misa is M-mode-only (csrr faults
    // illegal-insn in S-mode); S-mode FPU detection needs DTB isa-string
    // parsing or tentative FS-enable follow-up work.
    // JARVIS_REGISTER_TEST(riscv64_fpu_extension_detection);
    JARVIS_REGISTER_TEST(riscv64_sbi_console_putchar);
    JARVIS_REGISTER_TEST(riscv64_pci_ecam_read);
    JARVIS_REGISTER_TEST(riscv64_rtc_mtime_read);
    JARVIS_REGISTER_TEST(riscv64_satp_csr);
    // Issue #205: deregistered — mvendorid/marchid/mimpid are M-mode-only
    // CSRs; reading them in S-mode raises illegal-insn (no S-gate proof
    // possible; QEMU banner already shows the values).
    // JARVIS_REGISTER_TEST(riscv64_boot_mvendorid);
    // Issue #205: deregistered — medeleg/mideleg are M-mode-only CSRs
    // (same illegal-insn class as mvendorid; U-ecall delegation proven
    // indirectly when #206 runs U-mode).
    // JARVIS_REGISTER_TEST(riscv64_medeleg_selected);
    JARVIS_REGISTER_TEST(riscv64_abi_frame_conform);
    JARVIS_REGISTER_TEST(riscv64_abi_arg_routing);
    JARVIS_REGISTER_TEST(riscv64_abi_bad_number);
    JARVIS_REGISTER_TEST(riscv64_umode_ecall_smoke);  // issue #206 M1
    JARVIS_REGISTER_TEST(riscv64_elf_bad_machine);    // issue #206 M2
    JARVIS_REGISTER_TEST(riscv64_clone_frame_readback);  // issue #206 M2
    JARVIS_REGISTER_TEST(riscv64_exec_slots);            // issue #206 M2
    JARVIS_REGISTER_TEST(riscv64_u_fault_terminates);    // issue #206 M2
    JARVIS_REGISTER_TEST(riscv64_elf_ecall_smoke);       // issue #206 M2
}

#endif
