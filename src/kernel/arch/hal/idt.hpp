#pragma once

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

/// @file idt.hpp
/// @brief Interrupt Descriptor Table (x86_64) / interrupt vector dispatch for
/// all architectures.

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace arch {

/// @cond
#if defined(CONFIG_ARCH_X86_64)
/// @endcond

/// @brief x86-64 IDT entry (16-byte packed interrupt gate descriptor).
struct IDTEntry {
    uint16_t offset_low;  ///< Bits 0–15 of handler address.
    uint16_t selector;    ///< Code segment selector.
    uint8_t ist;          ///< Interrupt Stack Table index.
    uint8_t type_attr;    ///< Gate type and attributes.
    uint16_t offset_mid;  ///< Bits 16–31 of handler address.
    uint32_t offset_high; ///< Bits 32–63 of handler address.
    uint32_t zero;        ///< Reserved (must be zero).
    IDTEntry() = default;
} __attribute__((packed));

/// @brief x86-64 IDT descriptor (10-byte packed, used with LIDT).
struct IDTDescriptor {
    uint16_t limit; ///< Size of the IDT minus one.
    uint64_t base;  ///< Linear address of the IDT.
} __attribute__((packed));

/// @brief Named interrupt vectors for x86-64.
enum class InterruptVector : uint8_t {
    DIV_ZERO = 0,         ///< Divide-by-zero fault (#DE).
    DEBUG = 1,            ///< Debug trap (#DB).
    NMI = 2,              ///< Non-maskable interrupt.
    BREAKPOINT = 3,       ///< Breakpoint trap (#BP).
    OVERFLOW = 4,         ///< Overflow trap (#OF).
    BOUND_RANGE = 5,      ///< Bound-range exceeded (#BR).
    INVALID_OPCODE = 6,   ///< Invalid opcode (#UD).
    DEVICE_NA = 7,        ///< Device not available (#NM).
    DOUBLE_FAULT = 8,     ///< Double fault (#DF).
    INVALID_TSS = 10,     ///< Invalid TSS (#TS).
    SEGMENT_NP = 11,      ///< Segment not present (#NP).
    STACK_SEG_FAULT = 12, ///< Stack-segment fault (#SS).
    GP_FAULT = 13,        ///< General-protection fault (#GP).
    PAGE_FAULT = 14,      ///< Page fault (#PF).
    FPU_ERROR = 16,       ///< x87 FPU error (#MF).
    ALIGN_CHECK = 17,     ///< Alignment check (#AC).
    MACHINE_CHECK = 18,   ///< Machine check (#MC).
    SIMD_ERROR = 19,      ///< SIMD floating-point error (#XM).
    VIRT_ERROR = 20,      ///< Virtualisation error (#VE).
    TIMER = 32,           ///< PIT/HPET timer interrupt.
                             ///< §1 OWNER (ABI v1 frozen): scheduler tick;
                             ///< never claimable (irq_delivery.cpp rejects).
    KEYBOARD = 33,        ///< PS/2 keyboard interrupt.
    SYSCALL = 0x80,       ///< System-call software interrupt.
                             ///< §1 sole live gate (ABI v1 frozen);
                             ///< claim rejects (irq_delivery.cpp).
};

/// @brief Interrupt Service Routine function signature.
/// @param vector Interrupt vector number.
/// @param error_code CPU-pushed error code (0 if none).
/// @param rip Faulting or return instruction pointer.
using ISRHandler = void (*)(uint64_t vector, uint64_t error_code, uint64_t rip);

/// @brief x86-64 Interrupt Descriptor Table manager.
class IDT {
  public:
    /// @brief Initialise the IDT with default gate descriptors.
    static void init();
    /// @brief Load the IDT via the LIDT instruction.
    static void load();
    /// @brief Register a handler for a named interrupt vector.
    /// @param vec Named vector.
    /// @param handler Handler function.
    static void register_handler(InterruptVector vec, ISRHandler handler);
    /// @brief Register a handler for a raw vector number.
    /// @param vec Raw vector (0–255).
    /// @param handler Handler function.
    static void register_handler_raw(uint8_t vec, ISRHandler handler);
    /// @brief Dispatch an interrupt to the registered handler.
    /// @param vector Interrupt vector.
    /// @param error_code CPU-pushed error code.
    /// @param rip Faulting instruction address.
    static void handle_interrupt(uint64_t vector, uint64_t error_code,
                                 uint64_t rip);
    /// @brief Get a const reference to an IDT entry.
    /// @param vec Vector index.
    /// @return Const reference to the IDT entry.
    static const IDTEntry &entry(uint8_t vec);
    /// @brief Check whether a handler is registered for a vector.
    /// @param vec Vector index.
    /// @return true if a handler exists.
    static bool has_handler(size_t vec);

  private:
    static constexpr size_t NUM_ENTRIES = 256;
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static IDTEntry entries_[NUM_ENTRIES];
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static IDTDescriptor desc_;
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static ISRHandler handlers_[NUM_ENTRIES];
};

/// @brief Assembly-level interrupt entry point.
extern "C" void isr_entry();

/// @cond
#elif defined(CONFIG_ARCH_AARCH64) || defined(CONFIG_ARCH_RISCV64)

/// @brief Named interrupt vectors for AArch64/RISC-V.
/// §1 abstract enum frozen under ABI v1; any new assignment
/// bumps the ABI minor (spec §10: versioning table).
enum class InterruptVector : uint8_t {
    TIMER = 0,    ///< Timer interrupt.
    KEYBOARD = 1, ///< Keyboard interrupt.
    SYSCALL = 2,  ///< System-call exception.
};

/// @brief Interrupt Service Routine function signature.
using ISRHandler = void (*)(uint64_t vector, uint64_t error_code, uint64_t rip);

/// @brief Generic interrupt dispatch manager for AArch64/RISC-V.
class IDT {
  public:
    static void init();
    static void load();
    static void register_handler(InterruptVector vec, ISRHandler handler);
    static void register_handler_raw(uint8_t vec, ISRHandler handler);
    static void handle_interrupt(uint64_t vector, uint64_t error_code,
                                 uint64_t rip);

  private:
    static constexpr size_t NUM_ENTRIES = 64;
    static ISRHandler handlers_[NUM_ENTRIES];
};

#else
#error "HAL: no idt implementation for this architecture"
#endif
/// @endcond

} // namespace arch
