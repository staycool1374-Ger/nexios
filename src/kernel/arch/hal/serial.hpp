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

/// @file serial.hpp
/// @brief Serial port (UART) driver — architecture-agnostic interface.

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace arch {

/// @brief Serial port (UART) driver — architecture-agnostic interface.
class Serial {
  public:
    /// @brief Initialise the serial port hardware.
    static void init();
    /// @brief Transmit a single character.
    /// @param c Character to transmit.
    static void putchar(char c);
    /// @brief Receive a single character (blocking).
    /// @return The received character.
    static char getchar();
    /// @brief Non-blocking receive poll (issue #245: the interactive shell
    ///        has no input source on architectures without PS/2 — the x86
    ///        COM1 inline poll and the Keyboard stub both miss the UART).
    ///        True with the oldest pending byte iff RX data is ready;
    ///        false immediately otherwise (never waits, never spins).
    ///        riscv64 polls the virt 16550A LSR (issue #247).
    /// @param[out] c Received character (untouched when returning false).
    /// @return True iff a byte was available and stored in c.
    static bool poll_getchar(char &c);
    /// @brief Transmit a null-terminated string.
    /// @param str Null-terminated string to transmit.
    static void puts(const char *str);

    /// @brief Returns the number of characters written to the serial console
    ///        since boot.  The interactive shell snapshots this before
    ///        drawing its prompt; if it advanced while the shell waits for
    ///        input, a background task wrote to the console and the shell
    ///        redraws its prompt on a fresh line (prevents async output such
    ///        as the ELF loader's "completed" event from gluing onto the
    ///        prompt).
    static uint64_t write_count() noexcept;

#if defined(CONFIG_ARCH_AARCH64)
    /// @brief PL011 register snapshot for init-state assertions (issue
    ///        #355): read-only observers so tests never duplicate the
    ///        UART base. No behavior change, no side effects.
    struct Pl011Snapshot {
        uint32_t fr;
        uint32_t cr;
        uint32_t lcr_h;
        uint32_t ibrd;
        uint32_t fbrd;
        uint32_t imsc;
    };
    static Pl011Snapshot pl011_snapshot();
#endif

#if defined(CONFIG_ARCH_RISCV64)
    /// @brief 16550A register snapshot (issue #355): read-only observers
    ///        for init-state assertions (IER must stay 0 — polling only).
    struct Uart16550Snapshot {
        uint8_t ier;
        uint8_t lcr;
        uint8_t mcr;
        uint8_t lsr;
    };
    static Uart16550Snapshot uart_snapshot();
    /// @brief MCR LOOP bit (bit 4): TX→RX internal loopback for the
    ///        test-only round-trip (production never sets it).
    static constexpr uint8_t UART_MCR_LOOP = 0x10;
    /// @brief Raw MCR read (loopback setup observes the pre-test value).
    static uint8_t uart_mcr();
    /// @brief Raw MCR write (test-only loopback control; production
    ///        never calls it — TX stays on the SBI path).
    static void uart_mcr_set(uint8_t value);
    /// @brief Drain the RX FIFO (bounded 16 reads — the 16550 FIFO depth).
    static void uart_drain_rx();
#endif
};

} // namespace arch
