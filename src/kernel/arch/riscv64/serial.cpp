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
#include <kernel/arch/riscv64/hal/uart.hpp>
#include <types.hpp>

namespace arch {

/// @brief UART register literals live in hal/uart.hpp (issue #387: single
///        arch-owned source shared with the arch tests).  Aliases below
///        keep this TU's call sites untouched.
static constexpr uint64_t RISCV_UART_BASE = UART0_BASE;
static constexpr uint64_t RISCV_UART_RBR_OFF = UART_RBR_OFF;
static constexpr uint64_t RISCV_UART_IER_OFF = UART_IER_OFF;
static constexpr uint64_t RISCV_UART_LCR_OFF = UART_LCR_OFF;
static constexpr uint64_t RISCV_UART_MCR_OFF = UART_MCR_OFF;
static constexpr uint64_t RISCV_UART_LSR_OFF = UART_LSR_OFF;
static constexpr uint8_t RISCV_UART_LSR_DATA_READY = UART_LSR_DATA_READY;
static constexpr int SERIAL_RX_WAIT_ITERS = 1000000;

/// @brief Number of characters written to the console since boot.
/// The interactive shell snapshots this before drawing its prompt (see
/// arch::Serial::write_count contract in arch/hal/serial.hpp).
static volatile uint64_t s_serial_write_count = 0;

/// @brief Initialize the serial port (no-op on RISC-V64).
/// The virt 16550A divisor is pre-initialized by OpenSBI/QEMU;
/// reprogramming it risks breaking the S-mode SBI TX path, so RX-only
/// polling attaches to the live configuration (issue #247). The kernel
/// never writes IER — RX is polled, never interrupt-driven.
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

/// @brief 16550A register snapshot for init-state assertions (issue #355).
/// Read-only: four MMIO byte reads, no writes, no side effects.
Serial::Uart16550Snapshot Serial::uart_snapshot() {
    Uart16550Snapshot snap{};
    const volatile void *ier_addr = reinterpret_cast<const volatile void *>(
        RISCV_UART_BASE + RISCV_UART_IER_OFF);
    const volatile void *lcr_addr = reinterpret_cast<const volatile void *>(
        RISCV_UART_BASE + RISCV_UART_LCR_OFF);
    const volatile void *mcr_addr = reinterpret_cast<const volatile void *>(
        RISCV_UART_BASE + RISCV_UART_MCR_OFF);
    const volatile void *lsr_addr = reinterpret_cast<const volatile void *>(
        RISCV_UART_BASE + RISCV_UART_LSR_OFF);
    snap.ier = arch::mmio_read8(ier_addr);
    snap.lcr = arch::mmio_read8(lcr_addr);
    snap.mcr = arch::mmio_read8(mcr_addr);
    snap.lsr = arch::mmio_read8(lsr_addr);
    return snap;
}

/// @brief Raw MCR read (loopback setup observes the pre-test value).
uint8_t Serial::uart_mcr() {
    const volatile void *mcr_addr = reinterpret_cast<const volatile void *>(
        RISCV_UART_BASE + RISCV_UART_MCR_OFF);
    return arch::mmio_read8(mcr_addr);
}

/// @brief Raw MCR write (test-only loopback control; production never
///        calls it — TX stays on the SBI path).
void Serial::uart_mcr_set(uint8_t value) {
    volatile void *mcr_addr = reinterpret_cast<volatile void *>(
        RISCV_UART_BASE + RISCV_UART_MCR_OFF);
    arch::mmio_write8(mcr_addr, value);
}

/// @brief Drain the RX FIFO (bounded 16 reads — the 16550 FIFO depth).
void Serial::uart_drain_rx() {
    const volatile void *lsr_addr = reinterpret_cast<const volatile void *>(
        RISCV_UART_BASE + RISCV_UART_LSR_OFF);
    const volatile void *rbr_addr = reinterpret_cast<const volatile void *>(
        RISCV_UART_BASE + RISCV_UART_RBR_OFF);
    for (int idx = 0; idx < 16; ++idx) {
        if ((arch::mmio_read8(lsr_addr) & RISCV_UART_LSR_DATA_READY) == 0)
            break;
        (void)arch::mmio_read8(rbr_addr);
        arch::pause();
    }
}

} // namespace arch
