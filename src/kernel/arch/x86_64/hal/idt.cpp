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

/// @file idt.cpp
/// @brief Interrupt Descriptor Table — populates IDT entries and dispatches
/// interrupts to registered handlers.

#include <kernel/arch/idt.hpp>
#include <kernel/arch/gdt.hpp>
#include <types.hpp>
#include <assert.hpp>

namespace arch {

/// @brief External ISR vector table defined in the linker or assembly entry
/// point.
// NOLINTNEXTLINE(bugprone-reserved-identifier)
extern "C" uint64_t __isr_vector[];

/// @brief IDT entry table.
IDTEntry IDT::entries_[NUM_ENTRIES] = {};
/// @brief IDT pseudo-descriptor (base + limit) loaded by LIDT.
IDTDescriptor IDT::desc_ = {};
/// @brief Registered C++ interrupt handler callbacks.
ISRHandler IDT::handlers_[NUM_ENTRIES] = {};

/// @brief Construct an IDT entry from its raw fields.
/// @param handler 64-bit address of the interrupt service routine.
/// @param selector Code segment selector.
/// @param type_attr Gate type and attributes (e.g. 0x8E for interrupt gate).
/// @param ist IST stack index (0–6, 0 means no IST switch).
/// @return A populated IDTEntry structure.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static IDTEntry make_entry(uint64_t handler, uint16_t selector,
                           uint8_t type_attr, uint8_t ist) {
    IDTEntry e = {};
    e.offset_low = handler & 0xFFFF;
    e.selector = selector;
    e.ist = ist & 0x7;
    e.type_attr = type_attr;
    e.offset_mid = (handler >> 16) & 0xFFFF;
    e.offset_high = (handler >> 32) & 0xFFFFFFFF;
    e.zero = 0;
    return e;
}

/// @brief Initialise the IDT with default ISR entries and special handling for
/// SYSCALL (trap gate, ring-3 accessible) and DOUBLE_FAULT (IST1).
void IDT::init() {
    for (size_t i = 0; i < NUM_ENTRIES; ++i) {
        entries_[i] = make_entry(__isr_vector[i], GDT_CODE, 0x8E, 0);
    }

    entries_[static_cast<int>(InterruptVector::SYSCALL)] =
        make_entry(__isr_vector[static_cast<int>(InterruptVector::SYSCALL)],
                   GDT_CODE, 0xEE, 0);

    // Issue #226: the breakpoint gate (#BP, vector 3) must be ring-3
    // accessible (DPL=3, 0xEE) like SYSCALL. With DPL=0 a user `int3`
    // raises #GP (error 0x1A = IDT gate 3) instead of #BP, so software
    // breakpoints can never fire (spec §4: int3 is the x86_64 mechanism).
    entries_[static_cast<int>(InterruptVector::BREAKPOINT)] = make_entry(
        __isr_vector[static_cast<int>(InterruptVector::BREAKPOINT)], GDT_CODE,
        0xEE, 0);

    // Double-fault handler (vector 8) uses IST1 to switch to a dedicated stack
    entries_[static_cast<int>(InterruptVector::DOUBLE_FAULT)] = make_entry(
        __isr_vector[static_cast<int>(InterruptVector::DOUBLE_FAULT)], GDT_CODE,
        0x8E, 1);

    desc_.limit = sizeof(entries_) - 1;
    desc_.base = reinterpret_cast<uint64_t>(entries_);
}

/// @brief Load the IDT into the CPU via the LIDT instruction.
void IDT::load() {
    asm volatile("lidt %0" : : "m"(desc_));
}

/// @brief Register a C++ interrupt handler for a specific interrupt vector.
/// @param vec The interrupt vector (from the InterruptVector enum).
/// @param handler Callback to invoke when the interrupt fires.
void IDT::register_handler(InterruptVector vec, ISRHandler handler) {
    int idx = static_cast<int>(vec);
    if (idx < 0 || static_cast<size_t>(idx) >= NUM_ENTRIES)
        return;
    handlers_[idx] = handler;
}

/// @brief Register a handler using a raw vector number (bypasses the enum).
/// @param vec Interrupt vector number (0–255).
/// @param handler Callback to invoke when the interrupt fires.
void IDT::register_handler_raw(uint8_t vec, ISRHandler handler) {
    if (static_cast<size_t>(vec) >= NUM_ENTRIES)
        return;
    handlers_[vec] = handler;
}

/// @brief Dispatch an interrupt to its registered handler.
/// @param vector Interrupt vector number.
/// @param error_code CPU-pushed error code (0 for vectors that do not push
/// one).
/// @param rip Instruction pointer at the time of the interrupt.
void IDT::handle_interrupt(uint64_t vector, uint64_t error_code, uint64_t rip) {
    if (vector >= NUM_ENTRIES)
        return;
    auto handler = handlers_[vector];
    if (handler) {
        handler(vector, error_code, rip);
    }
}

/// @brief Get a const reference to an IDT entry.
/// @param vec Interrupt vector number.
/// @return Const reference to the IDTEntry.
const IDTEntry &IDT::entry(uint8_t vec) {
    return entries_[vec];
}

/// @brief Check whether an IDT entry has a non-zero handler address.
/// @param vec Interrupt vector number.
/// @return true if the entry contains a valid handler address.
bool IDT::has_handler(size_t vec) {
    if (vec >= NUM_ENTRIES)
        return false;
    const auto &e = entries_[vec];
    uint64_t handler = static_cast<uint64_t>(e.offset_high) << 32 |
                       static_cast<uint64_t>(e.offset_mid) << 16 | e.offset_low;
    return handler != 0;
}

} // namespace arch
