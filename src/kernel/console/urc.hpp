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

/// @file urc.hpp
/// @brief Bounded prefix-registered async console queue (issue #407 item 2).
///        Unsolicited result codes (URCs): background producers enqueue
///        short text lines; only the shell task pops and prints them at
///        prompt redraw, so async output never interleaves a typed line.
///        FIFO, drop-newest on overflow with a counted notice, ISR-safe
///        enqueue, static storage only (no ResourceTracker impact).

#pragma once

#include <types.hpp>

namespace kernel::console::urc {

// Payload line capacity (content chars, excluding the NUL).  Every loop
// in this module is bounded by one of the constants below.
static constexpr size_t kLineMax = 128;
// Queued lines.  Full => drop-newest + dropped_count, never overwrite.
static constexpr size_t kQueueDepth = 16;
// Max lines the shell drains per prompt pass (keeps redraw bounded).
static constexpr size_t kDrainMaxPerPass = 8;
// Prefix registry slots (compile-time tags + late producers).
static constexpr size_t kMaxPrefixes = 8;
// Prefix text capacity (chars, excluding the NUL).
static constexpr size_t kPrefixMax = 8;
// register_prefix failure sentinel (no free slot, or sealed).
static constexpr uint8_t kNoPrefix = 0xFF;

/// @brief Producer tags.  Order matches the compile-time prefix table.
enum class Tag : uint8_t {
    Elf = 0, ///< ELF loader progress lines ("[ELF] ...").
    Kill = 1, ///< Task-death notices ("[KILL] ...").
    Test = 2, ///< Test seam lines ("[TST] ...").
    Count = 3, ///< Tag count (not a producer).
};

/// @brief Enqueue one async line (ISR-safe, bounded, never blocks).
///        The line is frozen at enqueue ("[PREFIX] " + payload,
///        truncated with "..." past kLineMax-1).  Full queue drops the
///        new line and counts it (drop-newest: the queued prefix of
///        history survives).  Unknown tag or unregistered prefix is a
///        counted reject, never ENSURE.
/// @param tag Producer tag (< Tag::Count, prefix registered).
/// @param line NUL-terminated payload (nullptr => counted reject).
/// @return true iff the line was queued.
[[nodiscard]] bool emit(Tag tag, const char *line);

/// @brief Pop the oldest queued line (shell task only).
/// @param out Receives the line (always NUL-terminated).
/// @param cap Capacity of out in bytes (0/nullptr => false).
/// @return true iff a line was available.
bool pop(char *out, size_t cap);

/// @brief True iff at least one line is queued.
[[nodiscard]] bool pending();

/// @brief Drops since the last call (consumes the delta).
/// @return Number of lines dropped since the previous take_dropped().
[[nodiscard]] size_t take_dropped();

/// @brief Lifetime drops (monotonic, never reset except reset()).
[[nodiscard]] size_t dropped_count();
/// @brief Lifetime payload truncations (monotonic).
[[nodiscard]] size_t truncated_count();
/// @brief Lifetime rejects (null line, unknown tag, sealed/full table).
[[nodiscard]] size_t rejected_count();

/// @brief Register a late producer prefix (boot only, before seal()).
/// @param prefix Short tag text (<= kPrefixMax-1 chars, clamped).
/// @return Prefix id for emit, or kNoPrefix when sealed/full (counted).
[[nodiscard]] uint8_t register_prefix(const char *prefix);

/// @brief Freeze the prefix table (idempotent, shell boot path).
void seal();
/// @brief True once seal() has run.
[[nodiscard]] bool sealed();

/// @brief Quiescent-only reset (tests): drains the queue, zeroes
///        indices/counters/seal, keeps the compile-time prefixes.
///        NOT ISR-safe against concurrent emit — call only when no
///        producer can fire.
void reset();

} // namespace kernel::console::urc
