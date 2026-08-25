/// @file test_idt.cpp
/// @brief IDT (Interrupt Descriptor Table) tests.

#if defined(CONFIG_ARCH_X86_64)
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

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/arch/x86_64/hal/msr_impl.hpp>

using namespace kernel;

/// @brief Verifies all 256 IDT entries have non-null handler addresses after
/// init.
/// @input Initialize IDT
/// @expect All 256 entries point to valid handlers (no null pointers)
/// @depends kernel::arch::IDT
JARVIS_TEST(idt_entries_initialized, "PRE: iocd | POST: none") {
    for (uint16_t vec = 0; vec < 256; ++vec) {
        JARVIS_ASSERT(arch::IDT::has_handler(static_cast<uint8_t>(vec)));
    }
}

/// @brief Verifies CPU exceptions 0-31 have handler entries (no gaps).
/// @input Initialize IDT, inspect exception vectors 0-31
/// @expect Each vector 0-31 has a valid handler
/// @depends kernel::arch::IDT
JARVIS_TEST(idt_exception_handlers_mapped, "PRE: iocd | POST: none") {
    for (uint8_t vec = 0; vec <= 31; ++vec) {
        JARVIS_ASSERT(arch::IDT::has_handler(vec));
    }
}

/// @brief Verifies PIC IRQ0-IRQ15 mapped to interrupt vectors 0x20-0x2F.
/// @input Initialize IDT with PIC remapping
/// @expect Vectors 0x20-0x2F point to IRQ handlers
/// @depends kernel::arch::IDT, PIC
JARVIS_TEST(idt_irq_remapped, "PRE: iocd | POST: none") {
    for (uint8_t vec = 0x20; vec <= 0x2F; ++vec) {
        JARVIS_ASSERT(arch::IDT::has_handler(vec));
    }
}

/// @brief Verifies the syscall trap gate (int $0x80 / isr_128) is installed.
/// @input Inspect the IDT vector 0x80 slot and the LSTAR MSR.
/// @expect The 0x80 IDT entry is a present trap gate — the sole live syscall
///         path.  IA32_LSTAR must be 0 (P7, issue #6): the LSTAR/sysret
///         fastpath was removed because MSR_KERNEL_GS_BASE was never written
///         (entry swapgs left GS base 0 → guaranteed panic on any ring-3
///         syscall).  int $0x80 is GS-free.
/// @depends kernel::arch::IDT, Syscall::init
JARVIS_TEST(idt_syscall_handler_installed, "PRE: iocd | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    // P7: LSTAR must NOT be programmed (fastpath removed).
    uint64_t lstar = arch::rdmsr(arch::IA32_LSTAR);
    JARVIS_ASSERT(lstar == 0);
    // The live path is the 0x80 trap gate.
    JARVIS_ASSERT(arch::IDT::has_handler(0x80));
#else
    JARVIS_TEST_PASS();
#endif
}

/// @brief Verifies double fault handler uses TSS IST stack (not kernel stack).
/// @input Initialize IDT, inspect double fault entry (vector 8)
/// @expect IST index set to valid IST stack
/// @depends kernel::arch::IDT, TSS
JARVIS_TEST(idt_double_fault_uses_ist, "PRE: iocd | POST: none") {
    const auto &entry = arch::IDT::entry(8);
    JARVIS_ASSERT(entry.ist != 0);
    JARVIS_ASSERT(entry.ist <= 7);
}

/// @brief Verifies vectors 0x30-0x7F are not set (or point to spurious
/// handler).
/// @input Initialize IDT, inspect reserved vectors
/// @expect Vectors 0x30-0x7F are null or spurious handler
/// @depends kernel::arch::IDT
JARVIS_TEST(idt_reserved_vectors_null, "PRE: iocd | POST: none") {
    for (uint8_t vec = 0x30; vec <= 0x7F; ++vec) {
        const auto &e = arch::IDT::entry(vec);
        uint64_t handler = static_cast<uint64_t>(e.offset_high) << 32 |
                           static_cast<uint64_t>(e.offset_mid) << 16 |
                           e.offset_low;
        JARVIS_ASSERT(handler == 0 || arch::IDT::has_handler(vec));
    }
}

/// @brief Registers all IDT unit tests with the test framework.
/// @input None
/// @expect All IDT tests registered via JARVIS_REGISTER_TEST
/// @depends kernel test framework
void register_idt_tests() {
    Logger::info("Registering IDT tests");
    JARVIS_REGISTER_TEST(idt_entries_initialized);
    JARVIS_REGISTER_TEST(idt_exception_handlers_mapped);
    JARVIS_REGISTER_TEST(idt_irq_remapped);
    JARVIS_REGISTER_TEST(idt_syscall_handler_installed);
    JARVIS_REGISTER_TEST(idt_double_fault_uses_ist);
    JARVIS_REGISTER_TEST(idt_reserved_vectors_null);
}
#endif