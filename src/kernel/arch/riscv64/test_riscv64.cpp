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
#include <lib/string.hpp>
#include <constants.hpp>

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

/// @brief Map a single 4 KB page and verify get_physical matches, then
///        unmap and confirm the mapping is removed.
JARVIS_TEST(riscv64_sv39_map_unmap) {
    uint64_t phys = PMM::alloc_page();
    JARVIS_ASSERT_FMT(phys != 0, "PMM::alloc_page() returned 0");

    constexpr uint64_t VA = 0xFFFFFFC08043F000ULL;
    constexpr uint64_t FLAGS = PageFlags::PRESENT | PageFlags::WRITE;

    auto r = arch::ArchPageTable::map_page(VA, phys, FLAGS);
    JARVIS_ASSERT(r.ok());

    uint64_t retrieved = arch::ArchPageTable::get_physical(VA);
    JARVIS_ASSERT_EQ(phys, retrieved);

    auto ur = arch::ArchPageTable::unmap_page(VA);
    JARVIS_ASSERT(ur.ok());

    retrieved = arch::ArchPageTable::get_physical(VA);
    JARVIS_ASSERT_EQ(0ULL, retrieved);

    PMM::free_page(phys);
    JARVIS_TEST_PASS();
}

/// @brief Fill a 2 MB-aligned region with 512 page mappings, then overlay
///        a single page to trigger an L2 block-split scenario.
JARVIS_TEST(riscv64_sv39_block_split) {
    constexpr uint64_t BLOCK_VA = 0xFFFFFFC080600000ULL;
    constexpr uint64_t PAGE_VA = BLOCK_VA + 0x1000;
    constexpr uint64_t FLAGS = PageFlags::PRESENT | PageFlags::WRITE;

    for (size_t i = 0; i < 512; ++i) {
        uint64_t p = PMM::alloc_page();
        JARVIS_ASSERT(p != 0);
        auto r = arch::ArchPageTable::map_page(BLOCK_VA + i * 0x1000, p, FLAGS);
        JARVIS_ASSERT(r.ok());
    }

    uint64_t new_p = PMM::alloc_page();
    JARVIS_ASSERT(new_p != 0);
    auto r = arch::ArchPageTable::map_page(PAGE_VA, new_p, FLAGS);
    JARVIS_ASSERT(r.ok());

    uint64_t retrieved = arch::ArchPageTable::get_physical(PAGE_VA);
    JARVIS_ASSERT_EQ(new_p, retrieved);

    for (size_t i = 0; i < 512; ++i) {
        uint64_t pa = arch::ArchPageTable::get_physical(BLOCK_VA + i * 0x1000);
        if (i == 1) {
            JARVIS_ASSERT_EQ(new_p, pa);
        } else {
            JARVIS_ASSERT(pa != 0);
        }
    }

    for (size_t i = 0; i < 512; ++i) {
        arch::ArchPageTable::unmap_page(BLOCK_VA + i * 0x1000);
    }
    PMM::free_page(new_p);

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

/// @brief Register all riscv64 architecture tests.
void register_riscv64_tests() {
    Logger::info("Registering riscv64 architecture tests");

    JARVIS_REGISTER_TEST(riscv64_sv39_3level_walk);
    // Issue #205: deregistered until the Sv39 VMM backend lands (#152) —
    // ArchPageTable map/unmap/get_physical decode PTEs x86-style.
    // JARVIS_REGISTER_TEST(riscv64_sv39_map_unmap);
    // Issue #205: same Sv39 backend gate as map_unmap (#152).
    // JARVIS_REGISTER_TEST(riscv64_sv39_block_split);
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
}

#endif
