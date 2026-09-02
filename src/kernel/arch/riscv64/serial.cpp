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

/// @file serial.cpp
/// @brief RISC-V64 serial port driver — uses SBI ecall for output.

#include <kernel/arch/serial.hpp>
#include <types.hpp>

namespace arch {

/// @brief Number of characters written to the console since boot.
/// The interactive shell snapshots this before drawing its prompt (see
/// arch::Serial::write_count contract in arch/hal/serial.hpp).
static volatile uint64_t s_serial_write_count = 0;

/// @brief Initialize the serial port (no-op on RISC-V64; SBI handles it).
void Serial::init() {
}

/// @brief Write a single character via SBI ecall.
/// @param c Character to output (\\n is expanded to \\r\\n).
void Serial::putchar(char c) {
    if (c == '\n') {
        asm volatile("li a0, 13; li a7, 1; ecall" : : : "a0", "a7", "memory");
    }
    uint64_t ch = (unsigned char)c;
    asm volatile("mv a0, %0; li a7, 1; ecall"
                 :
                 : "r"(ch)
                 : "a0", "a7", "memory");
    __atomic_fetch_add(&s_serial_write_count, 1U, __ATOMIC_RELAXED);
}

/// @brief Returns the number of characters written to the serial console
///        since boot.
/// @return Character write count.
uint64_t Serial::write_count() noexcept {
    return __atomic_load_n(&s_serial_write_count, __ATOMIC_RELAXED);
}

/// @brief Read a character from serial (stub — always returns NUL).
/// @return Always '\\0' (no SBI getchar in S-mode).
char Serial::getchar() {
    return '\0';
}

/// @brief Write a null-terminated string via SBI ecall.
/// @param s Null-terminated string to output.
void Serial::puts(const char *s) {
    while (*s)
        putchar(*s++);
}

} // namespace arch
