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

/// @file spinlock.hpp
/// @brief Busy-wait spinlock using atomic exchange.
///
/// SMP-safe by construction (issue #61 audit): lock() is an
/// atomic_exchange ACQUIRE loop with arch::pause() backoff and unlock()
/// is an atomic_store RELEASE — mutual exclusion holds across CPUs.
/// Task-context paths may block; ISR paths must use try_lock()
/// (non-blocking skip-and-retry).

#pragma once

#include <kernel/arch/io.hpp>
#include <lib/atomic.hpp>

namespace kernel {
namespace sync {

/// @brief Simple busy-wait spinlock using atomic_test_and_set.
class SpinLock {
  public:
    SpinLock() = default;

    SpinLock(const SpinLock &) = delete;
    SpinLock &operator=(const SpinLock &) = delete;
    SpinLock(SpinLock &&) = delete;
    SpinLock &operator=(SpinLock &&) = delete;

    /// @brief Acquire the lock (spin until available).
    void lock() noexcept {
        while (atomic_exchange(&locked_, 1, __ATOMIC_ACQUIRE)) {
            arch::pause();
        }
        __atomic_store_n(&holder_, __builtin_return_address(0),
                         __ATOMIC_RELAXED);
    }

    /// @brief Release the lock.
    void unlock() noexcept {
        atomic_store(&locked_, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&holder_, nullptr, __ATOMIC_RELAXED);
    }

    /// @brief Force-unlock (used to initialise a lock after MemPool
    /// allocation).
    void reset() noexcept {
        locked_ = 0;
        holder_ = nullptr;
    }

    /// @brief Attempt to acquire without blocking.
    /// @return true if the lock was acquired.
    bool try_lock() noexcept {
        bool acquired = !atomic_exchange(&locked_, 1, __ATOMIC_ACQUIRE);
        if (acquired) {
            __atomic_store_n(&holder_, __builtin_return_address(0),
                             __ATOMIC_RELAXED);
        }
        return acquired;
    }

    const void *holder() const noexcept {
        return __atomic_load_n(&holder_, __ATOMIC_RELAXED);
    }

  private:
    int locked_ = 0; ///< 0 = unlocked, 1 = locked.
    const void *holder_ = nullptr;
};

} // namespace sync
} // namespace kernel
