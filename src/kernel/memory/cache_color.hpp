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

/// @file cache_color.hpp
/// @brief Cache coloring contract (issue #62).
///
/// A page's color is the cache-set index bits above the page offset:
/// color_of(phys) = (phys >> SHIFT) & MASK.  The geometry below is a
/// QEMU-contracted convention (no observable silicon cache under
/// virtualization), retunable via these three constexprs — no silicon
/// truth is claimed.  The allocator (PMM colored path) honors colors;
/// this header owns the formula so tests and PMM cannot disagree.

#pragma once

#include <types.hpp>

namespace kernel::cache {

// 16 colors of 4 KiB-interleaved pages (bits 15..12 of the phys addr).
static constexpr uint64_t NUM_COLORS = 16;
static constexpr uint64_t COLOR_SHIFT = 12;
static constexpr uint64_t COLOR_MASK = NUM_COLORS - 1;

static_assert((NUM_COLORS & (NUM_COLORS - 1)) == 0,
              "cache colors must be a power of two (mask formula)");

/// @brief Page color of a physical address (pure, lock-free, any ctx).
/// @param phys_addr Byte physical address (need not be page-aligned).
/// @return Color in [0, NUM_COLORS).
constexpr uint64_t color_of(uint64_t phys_addr) noexcept {
    return (phys_addr >> COLOR_SHIFT) & COLOR_MASK;
}

} // namespace kernel::cache
