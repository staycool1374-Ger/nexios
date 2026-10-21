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
/// @brief RISC-V64 interrupt dispatch table (stvec-based) — registers
///        per-vector ISR handlers and dispatches via trap_vector.

#include <kernel/arch/idt.hpp>
#include <kernel/arch/io.hpp>
#include <types.hpp>

extern "C" void trap_vector(void);

namespace arch {

/// @brief Static array of registered interrupt handlers.
ISRHandler IDT::handlers_[NUM_ENTRIES] = {};

/// @brief Set stvec to the assembly trap vector entry point.
void IDT::init() {
    asm volatile("csrw stvec, %0" : : "r"((uint64_t)trap_vector) : "memory");
}

/// @brief Ensure stvec is visible (compiler barrier).
void IDT::load() {
    asm volatile("" : : : "memory");
}

/// @brief Register a handler for a named interrupt vector.
/// @param vec     InterruptVector enum value.
/// @param handler Function pointer to invoke on this vector.
void IDT::register_handler(InterruptVector vec, ISRHandler handler) {
    int idx = static_cast<int>(vec);
    if (idx < 0 || static_cast<size_t>(idx) >= NUM_ENTRIES)
        return;
    handlers_[idx] = handler;
}

/// @brief Register a handler for a raw interrupt vector index.
/// @param vec     Vector index (0–NUM_ENTRIES-1).
/// @param handler Function pointer to invoke on this vector.
void IDT::register_handler_raw(uint8_t vec, ISRHandler handler) {
    if (static_cast<size_t>(vec) >= NUM_ENTRIES)
        return;
    handlers_[vec] = handler;
}

/// @brief Dispatch an interrupt to the registered handler.
/// @param vector     Interrupt vector number.
/// @param error_code Error code (zero on RISC-V; included for x86 compat).
/// @param pc         Program counter at time of interrupt.
void IDT::handle_interrupt(uint64_t vector, uint64_t error_code, uint64_t pc) {
    if (vector >= NUM_ENTRIES)
        return;
    auto handler = handlers_[vector];
    if (handler) {
        handler(vector, error_code, pc);
    }
}

/// @brief Check whether a handler is registered for a vector.
/// @param vec Vector index (bounds-checked; out-of-range is false).
/// @return true when a non-null handler is registered.
bool IDT::has_handler(size_t vec) {
    if (vec >= NUM_ENTRIES)
        return false;
    return handlers_[vec] != nullptr;
}

} // namespace arch
