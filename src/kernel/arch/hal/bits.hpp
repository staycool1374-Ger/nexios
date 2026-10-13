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

/// @file bits.hpp
/// @brief Bit manipulation utilities — find highest/lowest set bit.

#include <types.hpp>

/// @brief HAL-level bit manipulation utilities.
namespace hal::bits {

/// @brief Find the highest (most significant) set bit in a 64-bit mask.
/// @param mask The 64-bit value to scan.
/// @return Bit position of the highest set bit (0–63), or 0 if mask is zero.
static inline uint64_t find_highest_bit(uint64_t mask) noexcept {
    if (mask == 0)
        return 0;
#if defined(CONFIG_ARCH_X86_64) || defined(CONFIG_ARCH_AARCH64) ||             \
    defined(__GNUC__)
    return 63ULL - static_cast<uint64_t>(__builtin_clzll(mask));
#else
    uint64_t n = 0;
    if (mask & 0xFFFFFFFF00000000ULL) {
        n |= 32;
        mask >>= 32;
    }
    if (mask & 0xFFFF0000ULL) {
        n |= 16;
        mask >>= 16;
    }
    if (mask & 0xFF00ULL) {
        n |= 8;
        mask >>= 8;
    }
    if (mask & 0xF0ULL) {
        n |= 4;
        mask >>= 4;
    }
    if (mask & 0xCULL) {
        n |= 2;
        mask >>= 2;
    }
    if (mask & 0x2ULL) {
        n |= 1;
    }
    return n;
#endif
}

/// @brief Find the lowest (least significant) set bit in a 64-bit mask.
/// @param mask The 64-bit value to scan.
/// @return Bit position of the lowest set bit (0–63), or 0 if mask is zero.
static inline uint64_t find_lowest_bit(uint64_t mask) noexcept {
    if (mask == 0)
        return 0;
#if defined(CONFIG_ARCH_X86_64) || defined(CONFIG_ARCH_AARCH64) ||             \
    defined(__GNUC__)
    return static_cast<uint64_t>(__builtin_ctzll(mask));
#else
    uint64_t n = 0;
    if (!(mask & 0xFFFFFFFF)) {
        n |= 32;
        mask >>= 32;
    }
    if (!(mask & 0xFFFF)) {
        n |= 16;
        mask >>= 16;
    }
    if (!(mask & 0xFF)) {
        n |= 8;
        mask >>= 8;
    }
    if (!(mask & 0xF)) {
        n |= 4;
        mask >>= 4;
    }
    if (!(mask & 0x3)) {
        n |= 2;
        mask >>= 2;
    }
    if (!(mask & 0x1)) {
        n |= 1;
    }
    return n;
#endif
}

} // namespace hal::bits
