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

/// @file serial_input.hpp
/// @brief RX ring / key decoder for polled UART input (issues #355,
///        #407). A static single-producer ring normalizes UART bytes
///        into key events: the shell task pumps the UART, the harness
///        injects bytes directly, consumers pop decoded keys or peek
///        zero-copy slices. Compiled on x86_64/aarch64/riscv64 — the
///        sole UART RX consumer. Zero allocation, zero locks, polling
///        only — no UART RX interrupt exists on these arches and none
///        is enabled here (IER/IMSC stay 0).

#include <types.hpp>

namespace kernel::input::serial {

/// @brief Decoder ring capacity (power of two, SPSC requirement).
constexpr size_t kRingSize = 256;
/// @brief Per-pump UART poll budget (bounds one pump call).
constexpr size_t kPumpBudget = 32;

/// @brief Reset the ring and counters. QUIESCENT-ONLY: never while a
///        producer or consumer is active (mirrors SPSCRing::reset).
void reset();

/// @brief Inject raw serial bytes (harness/test path).
/// @return Bytes accepted (ring-full remainder counts as dropped).
size_t inject(const char *bytes, size_t len);

/// @brief Poll the UART (up to kPumpBudget bytes) into the ring.
/// Inert while loopback is active (the loopback window owns the UART).
/// @return Bytes pushed.
size_t pump();

/// @brief Pop one decoded key (consumer: shell/readline/tests).
/// @return true with the oldest key iff the ring was non-empty.
bool pop(char &out);

/// @brief Peek the next contiguous decoded run without consuming
///        (issue #407: readline fast path). Pumps nothing — call pump()
///        (or shell_peek_input) first. The view is valid only until the
///        next consume/pop/pump; consume strictly last.
/// @param data Receives the run pointer (valid until next ring update).
/// @param len  Receives the run length (0 when empty).
/// @return true iff a non-empty run is exposed.
bool peek_slice(const char *&data, size_t &len);

/// @brief Consume a previously peeked run (issue #407).
///        Consumer-only; clamps to the readable count.
/// @param count Bytes to drop.
/// @return Bytes actually consumed.
size_t consume(size_t count);

/// @brief Ring-full drops since boot (diagnostic, monotonic).
size_t dropped_count();

/// @brief Ring-full overrun events since boot (issue #407: counts each
///        dropped byte that arrived while the ring was full; diagnostic,
///        monotonic, throttled-logged at the push site).
size_t overrun_count();

/// @brief Enter the MCR loopback window (riscv64 debug builds only).
/// Fail-closed: false on other arches, in release, when already
/// active, on a foreign LOOP bit, or on read-back mismatch.
/// While active, pump() is inert and capture is suspended.
/// No logging and no asserts may run inside the window (an abort
/// would strand LOOP and silence the console suite-wide).
bool loopback_begin();

/// @brief Leave the loopback window (unconditional restore + drain).
void loopback_end();

/// @brief Whether the loopback window is active.
bool loopback_active();

/// @brief Whether Terminal capture must suspend (true while loopback
///        owns the UART). Consulted by tests, not production.
bool capture_suspended();

} // namespace kernel::input::serial
