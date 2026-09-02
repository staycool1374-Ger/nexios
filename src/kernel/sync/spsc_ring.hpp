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

/// @file spsc_ring.hpp
/// @file spsc_ring.hpp
/// @brief Lock-free single-producer single-consumer ring buffer template.

#pragma once

#include <types.hpp>
#include <lib/atomic.hpp>

/// @brief Lock-free single-producer single-consumer ring buffer.
/// @tparam T  Element type.
/// @tparam N  Capacity (must be a power of two).
template <typename T, size_t N> class SPSCRing {
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be power of 2");

    static constexpr size_t MASK = N - 1;

  public:
    /// @brief Push an item (discards if full).
    /// @return true on success.
    bool try_push(const T &item) {
        size_t h = kernel::atomic_load(&head_, __ATOMIC_RELAXED);
        size_t t = kernel::atomic_load(&tail_, __ATOMIC_ACQUIRE);
        size_t next = (h + 1) & MASK;
        if (next == t)
            return false;
        data_[h] = item;
        kernel::atomic_store(&head_, next, __ATOMIC_RELEASE);
        return true;
    }

    /// @brief Pop an item (no-op if empty).
    /// @return true on success.
    bool try_pop(T &item) {
        size_t t = kernel::atomic_load(&tail_, __ATOMIC_RELAXED);
        size_t h = kernel::atomic_load(&head_, __ATOMIC_ACQUIRE);
        if (t == h)
            return false;
        item = data_[t];
        kernel::atomic_store(&tail_, (t + 1) & MASK, __ATOMIC_RELEASE);
        return true;
    }

    /// @brief Check whether the buffer is empty.
    bool empty() const {
        return kernel::atomic_load(&head_, __ATOMIC_ACQUIRE) ==
               kernel::atomic_load(&tail_, __ATOMIC_ACQUIRE);
    }

    /// @brief Reset to empty state.  M-5 (audit-task-sync-v0.4.2): QUIESCENT-
    ///        ONLY — must not be called while either side is active (the
    ///        head/tail pair is not published atomically together).
    void reset() {
        kernel::atomic_store(&head_, size_t{0}, __ATOMIC_RELEASE);
        kernel::atomic_store(&tail_, size_t{0}, __ATOMIC_RELAXED);
    }

  private:
    alignas(64) size_t head_ =
        0; ///< Producer index (written by producer only).
    alignas(64) size_t tail_ =
        0;           ///< Consumer index (written by consumer only).
    T data_[N] = {}; ///< Element storage.
};
