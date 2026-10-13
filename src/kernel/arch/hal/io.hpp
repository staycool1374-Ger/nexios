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

/// @file io.hpp
/// @brief Port I/O (x86_64) and memory-mapped I/O (AArch64, RISC-V) primitives.

#include <types.hpp>

/// @cond
#if defined(CONFIG_ARCH_X86_64)
/// @endcond
/// @brief Assembly-level I/O stubs for x86-64 (provided by arch linker).
extern "C" {
/// @brief Write a byte to an I/O port.
void arch_outb(uint16_t port, uint8_t val);
/// @brief Read a byte from an I/O port.
/// @return The byte read.
uint8_t arch_inb(uint16_t port);
/// @brief Write a 16-bit word to an I/O port.
void arch_outw(uint16_t port, uint16_t val);
/// @brief Read a 16-bit word from an I/O port.
/// @return The word read.
uint16_t arch_inw(uint16_t port);
/// @brief Write a 32-bit dword to an I/O port.
void arch_outl(uint16_t port, uint32_t val);
/// @brief Read a 32-bit dword from an I/O port.
/// @return The dword read.
uint32_t arch_inl(uint16_t port);
/// @brief Halt the CPU until the next interrupt.
void arch_hlt();
/// @brief CPU pause/yield hint (e.g. PAUSE on x86).
void arch_pause();
/// @brief Disable interrupts (CLI).
void arch_cli();
/// @brief Enable interrupts (STI).
void arch_sti();
}
/// @cond
#endif
/// @endcond

/// @cond
// Include arch-specific inline implementations
#if defined(CONFIG_ARCH_X86_64)
/// @endcond
#include <kernel/arch/x86_64/hal/io_impl.hpp>
#elif defined(CONFIG_ARCH_AARCH64)
#include <kernel/arch/aarch64/hal/io_impl.hpp>
#elif defined(CONFIG_ARCH_RISCV64)
#include <kernel/arch/riscv64/hal/io_impl.hpp>
/// @cond
#else
#error "HAL: no io implementation for this architecture"
#endif
/// @endcond
