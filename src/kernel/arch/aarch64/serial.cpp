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
/// @brief AArch64 PL011 UART serial driver.

#include <kernel/arch/serial.hpp>
#include <kernel/arch/hal/io.hpp>
#include <constants.hpp>

namespace arch {

#include <kernel/memory/address.hpp>
#define UART_BASE ((volatile uint32_t *)(arch::HHDM_OFFSET + 0x9000000ULL))

#define UART_DR 0x000
#define UART_FR 0x018
#define UART_IBRD 0x024
#define UART_FBRD 0x028
#define UART_LCR_H 0x02C
#define UART_CR 0x030
#define UART_IMSC 0x038
#define UART_ICR 0x044

/// @brief Number of characters written to the console since boot.
/// The interactive shell snapshots this before drawing its prompt (see
/// arch::Serial::write_count contract in arch/hal/serial.hpp).
static volatile uint64_t s_serial_write_count = 0;

/// @brief Initialise the PL011 UART at 115200 baud (default QEMU virt config).
/// Disables UART, clears pending interrupts, sets baud rate, enables TX/RX.
void Serial::init() {
    mmio_write32(UART_BASE + UART_CR / 4, 0);
    for (int i = 0; i < 100; ++i)
        asm volatile("nop");
    mmio_write32(UART_BASE + UART_ICR / 4, 0x7FF);
    mmio_write32(UART_BASE + UART_IBRD / 4, 26);
    mmio_write32(UART_BASE + UART_FBRD / 4, 3);
    mmio_write32(UART_BASE + UART_LCR_H / 4, 0x70);
    mmio_write32(UART_BASE + UART_IMSC / 4, 0);
    mmio_write32(UART_BASE + UART_CR / 4, 0x301);
}

/// @brief Write a single character to the serial port.
/// Translates LF to CR+LF. Waits for TX FIFO to have space before writing.
/// @param[in] c Character to transmit.
void Serial::putchar(char c) {
    if (c == '\n') {
        while (mmio_read32(UART_BASE + UART_FR / 4) & (1 << 5))
            ;
        mmio_write32(UART_BASE + UART_DR / 4, '\r');
    }
    while (mmio_read32(UART_BASE + UART_FR / 4) & (1 << 5))
        ;
    mmio_write32(UART_BASE + UART_DR / 4, c);
    __atomic_fetch_add(&s_serial_write_count, 1U, __ATOMIC_RELAXED);
}

/// @brief Returns the number of characters written to the serial console
///        since boot.
/// @return Character write count.
uint64_t Serial::write_count() noexcept {
    return __atomic_load_n(&s_serial_write_count, __ATOMIC_RELAXED);
}

/// @brief Read a single character from the serial port (blocking).
/// Waits for RX FIFO to contain data before reading.
/// @return The received character (lower 8 bits of data register).
char Serial::getchar() {
    while (mmio_read32(UART_BASE + UART_FR / 4) & (1 << 4))
        ;
    return mmio_read32(UART_BASE + UART_DR / 4) & 0xFF;
}

/// @brief Write a null-terminated string to the serial port.
/// @param[in] s Null-terminated string to transmit.
void Serial::puts(const char *s) {
    while (*s)
        putchar(*s++);
}

} // namespace arch
