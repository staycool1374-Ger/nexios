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

/// @file bit.hpp
/// @brief Bit manipulation primitives (issue #348-D2: `<bit>` is absent
///        under -nostdinc and std::byteswap is C++23 anyway — hand-rolled).
/// @note #347 transparency: pure value functions (constexpr/noexcept,
///       no static state, no branches that matter) — safe to call inside
///       traced paths without modeling.

#pragma once

#include <types.hpp>
#include <concepts.hpp>

namespace kernel {

/// @brief Reinterpret the object representation of @p src as To (issue
///        #348-D2: std::bit_cast needs `<bit>`).
/// @tparam To Destination type (must be trivially copyable, same size).
/// @note Endian-sensitive by nature (same bytes, different value
///       meaning across endianness) — all supported arches are
///       little-endian; do NOT use it to parse multi-byte wire formats
///       (assemble those with explicit shifts, see validate_header).
template <typename To, TriviallyCopiable From>
[[nodiscard]] constexpr To bit_cast(const From &src) noexcept {
    static_assert(sizeof(To) == sizeof(From),
                  "bit_cast requires equal-sized types");
    static_assert(__is_trivially_copyable(To),
                  "bit_cast destination must be trivially copyable");
    // __builtin_memcpy is constant-evaluated; alignment-safe by construction
    // (bytewise copy, no actual misaligned access).
    To dst{};
    __builtin_memcpy(&dst, &src, sizeof(To));
    return dst;
}

/// @brief Byte order tags (std::endian equivalent).
enum class endian : uint32_t {
    little = 0,
    big = 1,
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    native = 1,
#else
    native = 0,
#endif
};

/// @brief 16-bit byte swap (constexpr).
[[nodiscard]] constexpr uint16_t byteswap16(uint16_t value) noexcept {
    return static_cast<uint16_t>((value << 8) | (value >> 8));
}

/// @brief 32-bit byte swap (constexpr).
[[nodiscard]] constexpr uint32_t byteswap32(uint32_t value) noexcept {
    return ((value & 0x000000FFULL) << 24) | ((value & 0x0000FF00ULL) << 8) |
           ((value & 0x00FF0000ULL) >> 8) | ((value & 0xFF000000ULL) >> 24);
}

/// @brief 64-bit byte swap (constexpr).
[[nodiscard]] constexpr uint64_t byteswap64(uint64_t value) noexcept {
    return ((value & 0x00000000000000FFULL) << 56) |
           ((value & 0x000000000000FF00ULL) << 40) |
           ((value & 0x0000000000FF0000ULL) << 24) |
           ((value & 0x00000000FF000000ULL) << 8) |
           ((value & 0x000000FF00000000ULL) >> 8) |
           ((value & 0x0000FF0000000000ULL) >> 24) |
           ((value & 0x00FF000000000000ULL) >> 40) |
           ((value & 0xFF00000000000000ULL) >> 56);
}

// Compile-time contract (issue #348-D2).
static_assert(byteswap16(0x1234) == 0x3412, "byteswap16");
static_assert(byteswap32(0x12345678) == 0x78563412, "byteswap32");
static_assert(byteswap64(0x0102030405060708ULL) == 0x0807060504030201ULL,
              "byteswap64");
static_assert(byteswap32(byteswap32(0xDEADBEEFULL)) == 0xDEADBEEFULL,
              "byteswap32 involution");
static_assert(bit_cast<uint32_t>(0x12345678U) == 0x12345678U,
              "bit_cast identity on same representation");
static_assert(endian::native == endian::little,
              "all supported arches are little-endian");

} // namespace kernel
