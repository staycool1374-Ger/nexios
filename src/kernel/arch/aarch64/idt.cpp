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
/// @brief AArch64 exception vector table (VBAR_EL1) and interrupt dispatch.

#include <kernel/arch/idt.hpp>
#include <kernel/arch/io.hpp>
#include <types.hpp>

namespace arch {

ISRHandler IDT::handlers_[NUM_ENTRIES] = {};

extern "C" void exception_entry(void);

/// @brief Initialise the exception vector table by setting VBAR_EL1.
void IDT::init() {
    uint64_t vbar = reinterpret_cast<uint64_t>(exception_entry);
    asm volatile("msr vbar_el1, %0" : : "r"(vbar));
    asm volatile("isb");
}

/// @brief Ensure the new vector table is visible (instruction barrier).
void IDT::load() {
    asm volatile("isb");
}

/// @brief Register an interrupt handler for a given vector.
/// @param[in] vec Interrupt vector.
/// @param[in] handler Callback function.
void IDT::register_handler(InterruptVector vec, ISRHandler handler) {
    int idx = static_cast<int>(vec);
    if (idx < 0 || static_cast<size_t>(idx) >= NUM_ENTRIES)
        return;
    handlers_[idx] = handler;
}

/// @brief Register an interrupt handler by raw vector number.
/// @param[in] vec Raw interrupt vector index.
/// @param[in] handler Callback function.
void IDT::register_handler_raw(uint8_t vec, ISRHandler handler) {
    if (static_cast<size_t>(vec) >= NUM_ENTRIES)
        return;
    handlers_[vec] = handler;
}

/// @brief Dispatch an interrupt to the registered handler.
/// @param[in] vector Interrupt vector number.
/// @param[in] error_code Architecture-specific error code.
/// @param[in] pc Program counter at the point of interrupt.
void IDT::handle_interrupt(uint64_t vector, uint64_t error_code, uint64_t pc) {
    if (vector >= NUM_ENTRIES)
        return;
    auto handler = handlers_[vector];
    if (handler) {
        handler(vector, error_code, pc);
    }
}

/// @brief Check whether a handler is registered for a vector.
/// @param[in] vec Vector index (bounds-checked; out-of-range is false).
/// @return true when a non-null handler is registered.
bool IDT::has_handler(size_t vec) {
    if (vec >= NUM_ENTRIES)
        return false;
    return handlers_[vec] != nullptr;
}

} // namespace arch
