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

#pragma once

namespace kernel {

// GCC 16+ needs indirect type traits for template-dependent contexts
template<typename T> constexpr inline bool _is_integral_v = false;
template<> constexpr inline bool _is_integral_v<bool> = true;
template<> constexpr inline bool _is_integral_v<char> = true;
template<> constexpr inline bool _is_integral_v<signed char> = true;
template<> constexpr inline bool _is_integral_v<unsigned char> = true;
template<> constexpr inline bool _is_integral_v<short> = true;
template<> constexpr inline bool _is_integral_v<unsigned short> = true;
template<> constexpr inline bool _is_integral_v<int> = true;
template<> constexpr inline bool _is_integral_v<unsigned int> = true;
template<> constexpr inline bool _is_integral_v<long> = true;
template<> constexpr inline bool _is_integral_v<unsigned long> = true;
template<> constexpr inline bool _is_integral_v<long long> = true;
template<> constexpr inline bool _is_integral_v<unsigned long long> = true;

template<typename T> constexpr inline bool _is_reference_v = false;
template<typename T> constexpr inline bool _is_reference_v<T&> = true;
template<typename T> constexpr inline bool _is_reference_v<T&&> = true;

template<typename T>
concept Integral = _is_integral_v<T>;

template<typename T>
concept TriviallyCopiable = __is_trivially_copyable(T);

template<typename T>
concept ValueType = !_is_reference_v<T>;

template<typename T>
concept ErrorEnum = __is_enum(T);

/// @brief Any scoped/unscoped enumeration (issue #348-D: constrains
///        kernel::to_underlying without dragging error-enum semantics).
template<typename T>
concept Enum = __is_enum(T);

template<typename T>
concept Lockable = requires(T t) {
    { t.lock() };
    { t.unlock() };
};

} // namespace kernel
