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
/// @brief Lock-free single-producer single-consumer ring buffer template.

#include <types.hpp>
#include <concepts.hpp>
#include <lib/atomic.hpp>

/// @brief Lock-free single-producer single-consumer ring buffer.
/// @tparam T  Element type (must be trivially copyable: elements cross
///         the producer/consumer boundary by value, issue #348-E).
/// @tparam N  Capacity (must be a power of two).
template <kernel::TriviallyCopiable T, size_t N> class SPSCRing {
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be power of 2");

    static constexpr size_t MASK = N - 1;

  public:
    /// @brief Zero-copy view into the readable region (issue #407).
    /// @field data Pointer to the first readable element (contiguous run
    ///         only — a wrapped region needs two peeks with a consume
    ///         between them).
    /// @field len  Number of readable elements from data (0 when empty).
    struct Slice {
        const T *data = nullptr;
        size_t len = 0;
    };

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

    /// @brief Peek the next contiguous readable run without consuming
    ///        (issue #407: readline fast path — scan + bulk copy, no
    ///        per-byte pop).
    ///        Consumer-only. The returned region stays valid only until
    ///        the next consume/try_pop/push-side wrap: the producer
    ///        never writes inside [tail, head), so the view is stable
    ///        while the consumer holds it without advancing.
    /// @param max_len Upper bound on the reported run (bounds the scan).
    /// @return Slice with len 0 and data nullptr when empty.
    Slice peek_slice(size_t max_len) const {
        size_t t = kernel::atomic_load(&tail_, __ATOMIC_RELAXED);
        size_t h = kernel::atomic_load(&head_, __ATOMIC_ACQUIRE);
        size_t used = (h + N - t) & MASK;
        size_t contiguous = N - (t & MASK);
        size_t len = used;
        if (len > contiguous)
            len = contiguous;
        if (len > max_len)
            len = max_len;
        Slice out{};
        if (len != 0) {
            out.data = &data_[t & MASK];
            out.len = len;
        }
        return out;
    }

    /// @brief Advance the consumer past count elements (issue #407).
    ///        Consumer-only. Clamps to the readable count — never
    ///        asserts on caller error (reachable over-consumption is a
    ///        drop, not an invariant violation).
    /// @param count Elements to drop (e.g. a scanned slice run).
    /// @return Elements actually consumed.
    size_t consume(size_t count) {
        size_t t = kernel::atomic_load(&tail_, __ATOMIC_RELAXED);
        size_t h = kernel::atomic_load(&head_, __ATOMIC_ACQUIRE);
        size_t used = (h + N - t) & MASK;
        if (count > used)
            count = used;
        if (count != 0)
            kernel::atomic_store(&tail_, (t + count) & MASK,
                                 __ATOMIC_RELEASE);
        return count;
    }

    /// @brief Reset to empty state.  QUIESCENT-ONLY — must not be called
    ///        while either side is active (the head/tail pair is not
    ///        published atomically together).
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
