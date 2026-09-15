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

/// @file rwlock.hpp
/// @brief SMP-safe reader-writer lock with writer preference (issue #61).
///
/// Single 32-bit state word: bit 31 = writer held, bit 30 = writer
/// pending (blocks new readers so a waiting writer cannot starve),
/// bits 29..0 = active reader count.  All transitions are lock-free
/// atomic RMW; waiters spin on arch::pause().
///
/// Contracts (same discipline as SpinLock):
/// - NEVER held across reschedule() or any BLOCKED transition.
/// - Blocking lock() is task-context only; ISR-adjacent paths must use
///   the try_ variants (non-blocking, skip-and-retry).
/// - Read side scales across CPUs; write side is exclusive against
///   readers and writers.

#pragma once

#include <kernel/arch/io.hpp>
#include <lib/atomic.hpp>

namespace kernel {
namespace sync {

class RwLock {
  public:
    RwLock() = default;

    RwLock(const RwLock &) = delete;
    RwLock &operator=(const RwLock &) = delete;
    RwLock(RwLock &&) = delete;
    RwLock &operator=(RwLock &&) = delete;

    /// @brief Acquire shared read access (blocks new readers only while
    ///        a writer holds or waits — writer preference).
    void read_lock() noexcept {
        for (;;) {
            uint32_t seen =
                atomic_load(&state_, __ATOMIC_ACQUIRE);
            if ((seen & (k_writer | k_pending)) != 0) {
                arch::pause();
                continue;
            }
            if (atomic_compare_exchange(&state_, &seen, seen + 1,
                                        __ATOMIC_ACQUIRE)) {
                return;
            }
            arch::pause();
        }
    }

    /// @brief Release shared read access.
    void read_unlock() noexcept {
        atomic_fetch_sub(&state_, 1u, __ATOMIC_RELEASE);
    }

    /// @brief Acquire exclusive write access (writer preference: sets
    ///        the pending bit first so no new reader can overtake).
    void write_lock() noexcept {
        for (;;) {
            uint32_t seen =
                atomic_load(&state_, __ATOMIC_ACQUIRE);
            if ((seen & k_writer) != 0) {
                arch::pause();
                continue;
            }
            atomic_fetch_or(&state_, k_pending, __ATOMIC_ACQ_REL);
            seen = atomic_load(&state_, __ATOMIC_ACQUIRE);
            if (seen == k_pending &&
                atomic_compare_exchange(&state_, &seen, k_writer,
                                        __ATOMIC_ACQUIRE)) {
                return;
            }
            arch::pause();
        }
    }

    /// @brief Release exclusive write access (clears writer + pending
    ///        so a waiting writer or readers can proceed).
    void write_unlock() noexcept {
        atomic_fetch_and(&state_, ~(k_writer | k_pending),
                         __ATOMIC_RELEASE);
    }

    /// @brief Attempt shared access without blocking.
    /// @return true if read access was acquired.
    bool try_read_lock() noexcept {
        uint32_t seen = atomic_load(&state_, __ATOMIC_ACQUIRE);
        if ((seen & (k_writer | k_pending)) != 0)
            return false;
        return atomic_compare_exchange(&state_, &seen, seen + 1,
                                       __ATOMIC_ACQUIRE);
    }

    /// @brief Attempt exclusive access without blocking.
    /// @return true if write access was acquired.
    bool try_write_lock() noexcept {
        uint32_t seen = atomic_load(&state_, __ATOMIC_ACQUIRE);
        if (seen != 0)
            return false;
        return atomic_compare_exchange(&state_, &seen, k_writer,
                                       __ATOMIC_ACQUIRE);
    }

    /// @brief Force-unlock (used to initialise a lock after MemPool
    ///        allocation; mirrors SpinLock::reset).
    void reset() noexcept {
        atomic_store(&state_, 0u, __ATOMIC_RELAXED);
    }

  private:
    static constexpr uint32_t k_writer = 1u << 31;
    static constexpr uint32_t k_pending = 1u << 30;
    uint32_t state_ = 0;
};

} // namespace sync
} // namespace kernel
