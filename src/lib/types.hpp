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

/// @file types.hpp
/// @brief Fundamental type definitions for the kernel (no standard library).

#pragma once

using uint8_t   = unsigned char;
using uint16_t  = unsigned short;
using uint32_t  = unsigned int;
using uint64_t  = unsigned long long;

using int8_t    = signed char;
using int16_t   = signed short;
using int32_t   = signed int;
using int64_t   = signed long long;

using size_t    = uint64_t;
using uintptr_t = uint64_t;
using intptr_t  = int64_t;

using nullptr_t = decltype(nullptr);

/// @brief User-defined literal for kibibytes (multiplies by 1024).
static constexpr size_t operator""_KiB(unsigned long long v) { return v * 1024; }
/// @brief User-defined literal for mebibytes (multiplies by 1024^2).
static constexpr size_t operator""_MiB(unsigned long long v) { return v * 1024 * 1024; }

// Placement new — required for `new (ptr) Type(...)` syntax.
// These do NOT allocate; they return the pointer unchanged.
void* operator new(unsigned long size, void* ptr) noexcept;
void* operator new[](unsigned long size, void* ptr) noexcept;

/// @brief Pull in the central configuration header.
#include <kernel/nexios_config.h>
#include <concepts.hpp>

/// @brief Convert a scoped-enum value to its underlying integer type
///        (issue #348-D: std::to_underlying is C++23/`<utility>`, absent
///        under -nostdinc — this is the hand-rolled equivalent).
namespace kernel {
template <Enum E>
[[nodiscard]] constexpr auto to_underlying(E value) noexcept {
    return static_cast<__underlying_type(E)>(value);
}
} // namespace kernel
