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

/// @file span.hpp
/// @brief Non-owning contiguous view (issue #348-D2: std::span needs
///        `<span>`, absent under -nostdinc — this is the hand-rolled
///        equivalent, minimal on purpose).
/// @note #347 transparency: a Span is a transparent view, not a path
///       step — constexpr/noexcept, zero hidden control flow, no static
///       state.  Hot-path tracing never needs to model inside it.

#pragma once

#include <types.hpp>

namespace kernel {

/// @brief Non-owning view over count contiguous elements.
/// @tparam T Element type (use `const T` for read-only views).
template <typename T>
class Span {
  public:
    /// @brief Empty view.
    constexpr Span() noexcept : data_(nullptr), size_(0) {
    }
    /// @brief View over [data, data + count).  Null with nonzero count
    ///        is a caller bug (same contract as std::span).
    constexpr Span(T *data, size_t count) noexcept
        : data_(data), size_(count) {
    }
    /// @brief View over a C array (size deduced).
    template <size_t kCount>
    constexpr Span(T (&arr)[kCount]) noexcept : data_(arr), size_(kCount) {
    }

    /// @brief Element count.
    [[nodiscard]] constexpr size_t size() const noexcept {
        return size_;
    }
    /// @brief True when the view holds no elements.
    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0;
    }
    /// @brief Pointer to the first element (may be null when empty).
    [[nodiscard]] constexpr T *data() const noexcept {
        return data_;
    }
    /// @brief Unchecked element access (std::span parity: the caller
    ///        owns bounds; use size() to guard).
    [[nodiscard]] constexpr T &operator[](size_t idx) const noexcept {
        return data_[idx];
    }
    /// @brief Range-for support.
    [[nodiscard]] constexpr T *begin() const noexcept {
        return data_;
    }
    /// @brief Range-for support.
    [[nodiscard]] constexpr T *end() const noexcept {
        return data_ + size_;
    }

  private:
    T *data_;
    size_t size_;
};

// Compile-time contract (issue #348-D2).
static_assert(Span<uint8_t>().empty(), "default span is empty");
static_assert(Span<uint8_t>().size() == 0, "default span has size 0");

} // namespace kernel
