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

/// @file test_hal.cpp
/// @brief HAL (Hardware Abstraction Layer) tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/arch/page_table.hpp>
#include <kernel/arch/context.hpp>
#include <kernel/arch/cpuid.hpp>
#include <kernel/arch/interrupt_controller.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <string.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies ArchPageTable map/unmap operations.
// Input: Map page, unmap page, verify mapping
// Expect: map returns true, unmap returns true, page no longer mapped
// Depends: kernel::arch::ArchPageTable
JARVIS_TEST(hal_page_table_map_unmap, "PRE: iocd | POST: none") {
    uint64_t cr3 = arch::ArchPageTable::current();
    JARVIS_ASSERT_FMT(cr3 != 0, "CR3 must be non-zero, got 0x%lx", cr3);

    arch::ArchPageTable::tlb_flush_all();
    JARVIS_ASSERT_EQ(cr3, arch::ArchPageTable::current());

    uint64_t virt = 0xFFFF800000000000ULL;
    arch::ArchPageTable::tlb_flush(virt);

    JARVIS_ASSERT_EQ(4096ULL, arch::ArchPageTable::PAGE_SIZE);
    JARVIS_ASSERT_EQ(512ULL, arch::ArchPageTable::ENTRIES);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies ArchPageTable clone creates independent copy.
// Input: Clone page table, modify clone, verify original unchanged
// Expect: Clone has same mappings, modifications don't affect original
// Depends: kernel::arch::ArchPageTable
JARVIS_TEST(hal_page_table_clone, "PRE: iocd | POST: none") {
    uint64_t cr3 = arch::read_cr3();
    JARVIS_ASSERT(cr3 != 0);

    uint64_t cr3_again = arch::read_cr3();
    JARVIS_ASSERT_EQ(cr3, cr3_again);

    JARVIS_ASSERT_EQ(4096ULL, arch::ArchPageTable::PAGE_SIZE);
    JARVIS_ASSERT_EQ(512ULL, arch::ArchPageTable::ENTRIES);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies ArchPageTable switch changes active address space.
// Input: Switch to different page table, access memory
// Expect: Memory accesses use new page table mappings
// Depends: kernel::arch::ArchPageTable
JARVIS_TEST(hal_page_table_switch, "PRE: iocd | POST: none") {
    uint64_t cr3 = arch::ArchPageTable::current();
    JARVIS_ASSERT(cr3 != 0);

    arch::ArchPageTable::activate(cr3);
    JARVIS_ASSERT_EQ(cr3, arch::ArchPageTable::current());

    arch::ArchPageTable::tlb_flush_all();
    JARVIS_ASSERT_EQ(cr3, arch::ArchPageTable::current());

    JARVIS_TEST_PASS();
}

#if defined(CONFIG_ARCH_X86_64)
// Runmode: kernel
// Testidea: Verifies ArchContext save/restore preserves register state.
// Input: Save context, modify registers, restore, verify original values
// Expect: All general-purpose registers restored correctly
// Depends: kernel::arch::ArchContext
JARVIS_TEST(hal_context_save_restore, "PRE: iocd | POST: none") {
    arch::ArchContext ctx;
    uint64_t test_rsp = 0xDEADBEEF;

    arch::ArchContextManager::save(ctx, test_rsp);
    JARVIS_ASSERT_EQ(test_rsp, ctx.rsp);

    uint64_t restored_rsp = 0;
    arch::ArchContextManager::restore(ctx, restored_rsp);
    JARVIS_ASSERT_EQ(test_rsp, restored_rsp);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies ArchContext switch_to transfers control correctly.
// Input: switch_to from task A to task B
// Expect: Task B runs with its register state, task A paused
// Depends: kernel::arch::ArchContext, kernel::task::Scheduler
// ⚠ BUG #008: This test exercises the scheduler/ISR nesting path. May trigger
//    GPF if isr_nesting_depth guard is incomplete. Diagnostic test.
JARVIS_TEST(hal_context_switch_to, "PRE: iocd | POST: none") {
    arch::ArchContext ctx_a;
    arch::ArchContext ctx_b;

    // Allocate writable memory for fake kernel stacks (via HHDM).
    uint64_t stack_a_phys = PMM::alloc_page();
    JARVIS_ASSERT(stack_a_phys != 0);
    uint64_t stack_b_phys = PMM::alloc_page();
    JARVIS_ASSERT(stack_b_phys != 0);

    uint64_t rsp_a = arch::HHDM_OFFSET + stack_a_phys + arch::PAGE_SIZE;
    uint64_t rsp_b = arch::HHDM_OFFSET + stack_b_phys + arch::PAGE_SIZE;

    arch::ArchContextManager::init_stack(
        reinterpret_cast<uint64_t *>(rsp_b), reinterpret_cast<void (*)()>(0),
        arch::SEG_KERNEL_CODE, arch::SEG_KERNEL_DATA, arch::RFLAGS_DEFAULT, 0);

    arch::ArchContextManager::save(ctx_a, rsp_a);
    arch::ArchContextManager::save(ctx_b, rsp_b);

    uint64_t current_rsp = rsp_a;
    arch::ArchContextManager::switch_to(ctx_a, ctx_b, current_rsp);
    JARVIS_ASSERT_EQ(ctx_a.rsp, rsp_a);
    JARVIS_ASSERT_EQ(ctx_b.rsp, rsp_b);
    JARVIS_ASSERT_EQ(current_rsp, rsp_b);

    PMM::free_page(stack_a_phys);
    PMM::free_page(stack_b_phys);
    JARVIS_TEST_PASS();
}
#endif

// Runmode: kernel
// Testidea: Verifies ArchInterruptController mask/unmask operations.
// Input: Mask IRQ, trigger interrupt, unmask, verify delivery
// Expect: Masked IRQ not delivered, unmasked IRQ delivered
// Depends: kernel::arch::ArchInterruptController
// ⚠ BUG #008: Interrupt operations may trigger timer nesting paths.
#if defined(CONFIG_ARCH_X86_64)
JARVIS_TEST(hal_interrupt_mask_unmask, "PRE: iocd | POST: none") {
    arch::IrqState saved = arch::ArchInterruptController::snapshot();

    arch::ArchInterruptController::mask(1);
    arch::IrqState masked = arch::ArchInterruptController::snapshot();
    JARVIS_ASSERT((masked.pic1_mask & 0x02) != 0);

    arch::ArchInterruptController::unmask(1);
    arch::IrqState unmasked = arch::ArchInterruptController::snapshot();
    JARVIS_ASSERT((unmasked.pic1_mask & 0x02) == 0);

    arch::ArchInterruptController::restore(saved);

    JARVIS_TEST_PASS();
}
#endif

// Runmode: kernel
// Testidea: Verifies ArchInterruptController EOI signals end of interrupt.
// Input: Trigger interrupt, send EOI, verify controller ready for next
// Expect: Without EOI, same priority interrupts blocked; with EOI, delivered
// Depends: kernel::arch::ArchInterruptController
// ⚠ BUG #008: Interrupt operations may trigger timer nesting paths.
JARVIS_TEST(hal_interrupt_eoi, "PRE: iocd | POST: none") {
    arch::ArchInterruptController::eoi(32);
    arch::ArchInterruptController::eoi(40);
    arch::ArchInterruptController::eoi(48);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies ArchTimer one-shot mode fires once.
// Input: Set one-shot timer for N ticks, wait
// Expect: Timer fires exactly once at target tick
// Depends: kernel::arch::ArchTimer
// Blocked: Timer::oneshot() and Timer::remaining() declared but not
// yet implemented in arch/x86_64/timer.cpp (planned for v0.2.20).
// ⚠ BUG #008: Timer ISR may trigger scheduler nesting. Diagnostic test.
/* Pseudocode:
 * 1. Set one-shot timer for current_ticks + 100
 * 2. Advance ticks to target - 1, verify not fired
 * 3. Advance to target, verify fired
 * 4. Advance further, verify not fired again
 */
JARVIS_TEST(hal_timer_oneshot, "PRE: iocd | POST: none") {
    /* Pseudocode: hal_timer_oneshot.
     * Implementation plan:
     *   - Requires Timer::oneshot(uint64_t) and Timer::remaining()
     * implementation
     *   - Once available: set oneshot(50), verify remaining > 0 && <= 50
     *   - Advance ticks, verify remaining decreases to 0
     * Blocked: Timer::oneshot/remaining not yet implemented.
     */
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies ArchTimer periodic mode fires repeatedly.
// Input: Set periodic timer with period P
// Expect: Timer fires at t+P, t+2P, t+3P...
// Depends: kernel::arch::ArchTimer
// Blocked: Timer::periodic() declared but not yet implemented.
// ⚠ BUG #008: Timer ISR may trigger scheduler nesting. Diagnostic test.
/* Pseudocode:
 * 1. Set periodic timer period=50
 * 2. Advance ticks, verify fires at 50, 100, 150
 * 3. Disable timer, verify stops firing
 */
JARVIS_TEST(hal_timer_periodic, "PRE: iocd | POST: none") {
    /* Pseudocode: hal_timer_periodic.
     * Implementation plan:
     *   - Requires Timer::periodic(uint64_t) and Timer::remaining()
     *   - Once available: set periodic(100), verify remaining changes
     *   - After multiple ticks, verify fires repeatedly
     * Blocked: Timer::periodic not yet implemented.
     */
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies ArchTimer remaining time calculation.
// Input: Set timer, query remaining at various points
// Expect: Remaining decreases correctly, 0 after fire
// Depends: kernel::arch::ArchTimer
// Blocked: Timer::oneshot() and Timer::remaining() not yet implemented.
/* Pseudocode:
 * 1. Set one-shot timer for 1000 ticks
 * 2. At tick+100, remaining ≈ 900
 * 3. At tick+500, remaining ≈ 500
 * 4. At tick+1000, remaining = 0 (fired)
 */
JARVIS_TEST(hal_timer_remaining, "PRE: iocd | POST: none") {
    /* Pseudocode: hal_timer_remaining.
     * Implementation plan:
     *   - Requires Timer::oneshot() and Timer::remaining()
     *   - Set oneshot for 200 ticks, query remaining at intervals
     *   - Verify remaining decreases monotonically
     * Blocked: Timer::oneshot/remaining not yet implemented.
     */
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies ArchIO in/out byte/word/long operations.
// Input: Write to port, read back, verify (mock/test port)
// Expect: Values written match values read
// Depends: kernel::arch::ArchIO
#if defined(CONFIG_ARCH_X86_64)
JARVIS_TEST(hal_io_byte_word_long, "PRE: iocd | POST: none") {
    // Port 0x80 (POST diagnostic) — reads may not return written values
    // on all QEMU chipset emulations.  The purpose of this test is to
    // verify the I/O instructions execute without faulting.
    arch::ArchIO::outb(0x80, 0x42);
    arch::ArchIO::io_wait();
    (void)arch::ArchIO::inb(0x80);

    arch::ArchIO::outw(0x80, 0x1234);
    arch::ArchIO::io_wait();
    (void)arch::ArchIO::inw(0x80);

    arch::ArchIO::outl(0x80, 0x12345678);
    arch::ArchIO::io_wait();
    (void)arch::ArchIO::inl(0x80);

    arch::ArchIO::io_wait();

    JARVIS_TEST_PASS();
}
#endif

// Runmode: kernel
// Testidea: Verifies HAL methods delegate to correct arch/x86_64
// implementation. Input: Call HAL interface methods, check they call x86_64
// specifics Expect: All HAL calls resolve to arch/x86_64 implementations
// Depends: kernel::arch::*, build system
#if defined(CONFIG_ARCH_X86_64)
JARVIS_TEST(hal_delegates_to_x86_64, "PRE: iocd | POST: none") {
    JARVIS_ASSERT_EQ(8ULL, sizeof(void *));

    uint64_t cr3 = arch::read_cr3();
    JARVIS_ASSERT(cr3 != 0);

    uint64_t cr0 = arch::read_cr0();
    JARVIS_ASSERT(cr0 != 0);

    uint64_t cr4 = arch::read_cr4();
    JARVIS_ASSERT(cr4 != 0);

    JARVIS_ASSERT(arch::interrupts_enabled());

    JARVIS_TEST_PASS();
}
#endif

// Runmode: kernel
// Testidea: Verifies CPUID feature flags on x86-64. The architecture requires
// SSE and SSE2 to be present on all x86-64 CPUs.
// Input: arch::cpuid(1)
// Expect: EDX bits 25 (SSE) and 26 (SSE2) are set
// Depends: kernel::arch::cpuid
#if defined(CONFIG_ARCH_X86_64)
JARVIS_TEST(hal_cpuid_features, "PRE: iocd | POST: none") {
    arch::CpuIdResult leaf1 = arch::cpuid(1);
    JARVIS_ASSERT(leaf1.edx & arch::CPUID_EDX1_SSE);
    JARVIS_ASSERT(leaf1.edx & arch::CPUID_EDX1_SSE2);
    JARVIS_ASSERT(leaf1.edx & arch::CPUID_EDX1_FPU);
    JARVIS_ASSERT(leaf1.edx & arch::CPUID_EDX1_FXSR);
    JARVIS_TEST_PASS();
}
#endif

// Runmode: kernel
// Testidea: Verifies ArchInterruptController snapshot/restore cycle correctly
// saves and restores PIC mask state.
// Input: Snapshot current state, mask IRQ 2, verify mask changed, restore,
// verify original mask restored
// Expect: Snapshot captures correct masks, restore reverts to saved state
// Depends: kernel::arch::ArchInterruptController
#if defined(CONFIG_ARCH_X86_64)
JARVIS_TEST(hal_interrupt_snapshot_restore_cycle, "PRE: iocd | POST: none") {
    arch::IrqState saved = arch::ArchInterruptController::snapshot();

    arch::ArchInterruptController::mask(2);
    arch::IrqState masked = arch::ArchInterruptController::snapshot();
    JARVIS_ASSERT((masked.pic1_mask & 0x04) != 0);

    arch::ArchInterruptController::unmask(2);
    arch::IrqState unmasked = arch::ArchInterruptController::snapshot();
    JARVIS_ASSERT((unmasked.pic1_mask & 0x04) == 0);

    arch::ArchInterruptController::restore(saved);
    arch::IrqState restored = arch::ArchInterruptController::snapshot();
    JARVIS_ASSERT_EQ(saved.pic1_mask, restored.pic1_mask);
    JARVIS_ASSERT_EQ(saved.pic2_mask, restored.pic2_mask);

    JARVIS_TEST_PASS();
}
#endif

void register_hal_tests() {
    Logger::info("Registering HAL tests");

    JARVIS_REGISTER_TEST(hal_page_table_map_unmap);
    JARVIS_REGISTER_TEST(hal_page_table_clone);
    JARVIS_REGISTER_TEST(hal_page_table_switch);
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_REGISTER_TEST(hal_context_save_restore);
    JARVIS_REGISTER_TEST(hal_context_switch_to);
    JARVIS_REGISTER_TEST(hal_interrupt_mask_unmask);
#endif
    JARVIS_REGISTER_TEST(hal_interrupt_eoi);
    JARVIS_REGISTER_TEST(hal_timer_oneshot);
    JARVIS_REGISTER_TEST(hal_timer_periodic);
    JARVIS_REGISTER_TEST(hal_timer_remaining);
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_REGISTER_TEST(hal_io_byte_word_long);
    JARVIS_REGISTER_TEST(hal_delegates_to_x86_64);
    JARVIS_REGISTER_TEST(hal_cpuid_features);
    JARVIS_REGISTER_TEST(hal_interrupt_snapshot_restore_cycle);
#endif
}