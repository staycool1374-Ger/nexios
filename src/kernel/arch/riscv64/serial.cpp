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
#include <kernel/arch/hal/io.hpp>
#include <types.hpp>

namespace arch {

/// @brief QEMU virt 16550A UART base (issue #247): raw physical address,
///        directly dereferenceable in S-mode (riscv64 low half is
///        identity-mapped — same precedent as the PLIC accessors and the
///        LSR probe in test_riscv64.cpp).
static constexpr uint64_t RISCV_UART_BASE = 0x10000000ULL;
static constexpr uint64_t RISCV_UART_RBR_OFF = 0;
static constexpr uint64_t RISCV_UART_LSR_OFF = 5;
static constexpr uint8_t RISCV_UART_LSR_DATA_READY = 0x01;
static constexpr int SERIAL_RX_WAIT_ITERS = 1000000;

/// @brief Number of characters written to the console since boot.
/// The interactive shell snapshots this before drawing its prompt (see
/// arch::Serial::write_count contract in arch/hal/serial.hpp).
static volatile uint64_t s_serial_write_count = 0;

/// @brief Initialize the serial port (no-op on RISC-V64).
/// The virt 16550A divisor is pre-initialized by OpenSBI/QEMU;
/// reprogramming it risks breaking the S-mode SBI TX path, so RX-only
/// polling attaches to the live configuration (issue #247).
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

/// @brief Read a character from the virt 16550A RX FIFO (issue #247).
/// Waits (bounded) for LSR data-ready; returns '\0' when nothing arrives
/// (FLAW-08 — never hangs). TX stays on the SBI ecall path.
/// @return The received character, or '\0' on bounded-wait expiry.
char Serial::getchar() {
    const volatile void *lsr_addr = reinterpret_cast<const volatile void *>(
        RISCV_UART_BASE + RISCV_UART_LSR_OFF);
    int wait_idx = 0;
    for (wait_idx = 0; wait_idx < SERIAL_RX_WAIT_ITERS; ++wait_idx) {
        if ((arch::mmio_read8(lsr_addr) & RISCV_UART_LSR_DATA_READY) !=
            0) {
            break;
        }
        arch::pause();
    }
    if (wait_idx >= SERIAL_RX_WAIT_ITERS) {
        return '\0'; // no data (not a valid console character)
    }
    const volatile void *rbr_addr = reinterpret_cast<const volatile void *>(
        RISCV_UART_BASE + RISCV_UART_RBR_OFF);
    return static_cast<char>(arch::mmio_read8(rbr_addr));
}

/// @brief Non-blocking receive poll (issue #247): the interactive shell's
///        only input source on riscv64 (no PS/2, Keyboard is a stub).
///        Zero-wait LSR check — never spins, so the shell nap loop keeps
///        its 20ms latency bound even when the FIFO is empty.
/// @param[out] c Oldest pending RX byte when data is ready (untouched
///        otherwise).
/// @return True iff the LSR data-ready bit was set and c holds a byte.
bool Serial::poll_getchar(char &c) {
    const volatile void *lsr_addr = reinterpret_cast<const volatile void *>(
        RISCV_UART_BASE + RISCV_UART_LSR_OFF);
    if ((arch::mmio_read8(lsr_addr) & RISCV_UART_LSR_DATA_READY) == 0) {
        return false;
    }
    const volatile void *rbr_addr = reinterpret_cast<const volatile void *>(
        RISCV_UART_BASE + RISCV_UART_RBR_OFF);
    c = static_cast<char>(arch::mmio_read8(rbr_addr));
    return true;
}

/// @brief Write a null-terminated string via SBI ecall.
/// @param s Null-terminated string to output.
void Serial::puts(const char *s) {
    while (*s)
        putchar(*s++);
}

} // namespace arch
