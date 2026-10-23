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

/// @file serial_input.cpp
/// @brief Serial-injected key decoder (issues #355, #407). Ring core is
///        compiled on all arches (x86_64 included since #407: the shell
///        consumes UART RX through the ring everywhere); only the
///        arch::Keyboard facade stays non-x86 (x86_64 owns the i8042
///        Keyboard implementation).

#include <kernel/input/serial_input.hpp>
#include <kernel/arch/hal/serial.hpp>
#include <kernel/nexios_config.h>
#include <kernel/sync/spsc_ring.hpp>
#include <logger.hpp>

#if !defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/hal/keyboard.hpp>
#endif

namespace kernel::input::serial {

namespace {

// Strict SPSC: producer = shell pump / harness inject; consumer =
// shell/readline/tests. Static storage, no allocation, no locks.
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
static SPSCRing<char, kRingSize> s_ring;
static size_t s_dropped = 0;
static size_t s_overrun = 0;
static bool s_loopback_active = false;
#if defined(CONFIG_DEBUG) && defined(CONFIG_ARCH_RISCV64)
static uint8_t s_mcr_saved = 0;
#endif

/// @brief Normalize one raw UART byte into the ring.
/// NUL is dropped (never a key); DEL becomes backspace; CR/LF both
/// become newline; control/high bytes pass through verbatim (a UART
/// has no modifier channel — Ctrl arrives as 0x01-0x1A, never sticky).
void push_decoded(uint8_t byte) {
    char out = 0;
    bool emit = true;
    if (byte == 0x00) {
        emit = false; // NUL: dropped, counts below
    } else if (byte == 0x0D || byte == 0x0A) {
        out = '\n';
    } else if (byte == 0x08 || byte == 0x7F) {
        out = '\b';
    } else if (byte == 0x09) {
        out = '\t';
    } else {
        out = static_cast<char>(byte);
    }
    if (!emit) {
        ++s_dropped;
        return;
    }
    if (!s_ring.try_push(out)) {
        // Reachable exhaustion (paste burst over 255 while the shell naps):
        // fail-closed drop + count, never block, never ENSURE. Throttled
        // log: first drop, then every 256th (task context only — pump()
        // never runs in an ISR, and Logger::warn is ring-buffered).
        ++s_dropped;
        ++s_overrun;
        static constexpr size_t kOverrunLogEvery = 256;
        if ((s_overrun & (kOverrunLogEvery - 1)) == 1)
            kernel::Logger::warn("serial rx overrun: dropped=%zu",
                                 s_overrun);
    }
}

} // namespace

void reset() {
    s_ring.reset();
    s_dropped = 0;
    s_overrun = 0;
    s_loopback_active = false;
}

size_t inject(const char *bytes, size_t len) {
    if (!bytes)
        return 0;
    size_t accepted = 0;
    for (size_t idx = 0; idx < len; ++idx) {
        size_t before = s_dropped;
        push_decoded(static_cast<uint8_t>(bytes[idx]));
        if (s_dropped == before)
            ++accepted;
    }
    return accepted;
}

size_t pump() {
    if (s_loopback_active)
        return 0;
    size_t pushed = 0;
    for (size_t idx = 0; idx < kPumpBudget; ++idx) {
        char byte = 0;
        if (!arch::Serial::poll_getchar(byte))
            break;
        size_t before = s_dropped;
        push_decoded(static_cast<uint8_t>(byte));
        if (s_dropped == before)
            ++pushed;
    }
    return pushed;
}

bool pop(char &out) {
    return s_ring.try_pop(out);
}

size_t dropped_count() {
    return s_dropped;
}

size_t overrun_count() {
    return s_overrun;
}

bool peek_slice(const char *&data, size_t &len) {
    SPSCRing<char, kRingSize>::Slice slice =
        s_ring.peek_slice(kRingSize);
    data = slice.data;
    len = slice.len;
    return len != 0;
}

size_t consume(size_t count) {
    return s_ring.consume(count);
}

bool loopback_begin() {
#if !defined(CONFIG_DEBUG)
    return false;
#elif !defined(CONFIG_ARCH_RISCV64)
    return false;
#else
    if (s_loopback_active)
        return false;
    uint8_t current = arch::Serial::uart_mcr();
    if ((current & arch::Serial::UART_MCR_LOOP) != 0)
        return false; // foreign LOOP bit — not ours to own
    arch::Serial::uart_mcr_set(
        static_cast<uint8_t>(current | arch::Serial::UART_MCR_LOOP));
    if (arch::Serial::uart_mcr() !=
        static_cast<uint8_t>(current | arch::Serial::UART_MCR_LOOP)) {
        arch::Serial::uart_mcr_set(current);
        return false;
    }
    s_mcr_saved = current;
    arch::Serial::uart_drain_rx();
    s_loopback_active = true;
    return true;
#endif
}

void loopback_end() {
#if defined(CONFIG_DEBUG) && defined(CONFIG_ARCH_RISCV64)
    if (!s_loopback_active)
        return;
    arch::Serial::uart_mcr_set(s_mcr_saved);
    s_loopback_active = false;
    arch::Serial::uart_drain_rx();
#endif
}

bool loopback_active() {
    return s_loopback_active;
}

bool capture_suspended() {
    return s_loopback_active;
}

} // namespace kernel::input::serial

#if !defined(CONFIG_ARCH_X86_64)
// Non-x86 arch::Keyboard: one-line delegates onto the decoder (issue
// #355). handle_irq is a contract-parity no-op — no UART IRQ exists on
// these arches. Modifiers are always false: a UART has no modifier
// channel.
namespace arch {

void Keyboard::init() {
    kernel::input::serial::reset();
}

void Keyboard::handle_irq() {
}

bool Keyboard::getchar(char &c) {
    return kernel::input::serial::pop(c);
}

size_t Keyboard::read(char *buf, size_t len) {
    if (!buf)
        return 0;
    size_t count = 0;
    while (count < len) {
        char c = 0;
        if (!kernel::input::serial::pop(c))
            break;
        buf[count++] = c;
    }
    return count;
}

void Keyboard::flush() {
    kernel::input::serial::reset();
}

size_t Keyboard::pump_serial() {
    return kernel::input::serial::pump();
}

size_t Keyboard::inject_serial(const char *bytes, size_t len) {
    return kernel::input::serial::inject(bytes, len);
}

} // namespace arch

#endif // !CONFIG_ARCH_X86_64
