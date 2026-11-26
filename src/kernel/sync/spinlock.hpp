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

#include <kernel/arch/io.hpp>
#include <lib/atomic.hpp>
#include <kernel/sync/spinlock_order.hpp>
#include <kernel/sync/spinlock_hook.hpp> // issue #546-B hook seam (weak default)
#include <kernel/task/tick_watchdog.hpp> // contended-spin checkpoint (atomics only)

namespace kernel {
namespace sync {

/// @brief Simple busy-wait spinlock using atomic_test_and_set.
class SpinLock {
  public:
    SpinLock() = default;

    /// @brief Ranked construction (M2, #546 §6.2): the rank feeds the
    ///        lock-order check.  Unranked by default (invisible to it).
    constexpr explicit SpinLock(LockRank rank) noexcept
        : rank_(static_cast<uint8_t>(rank)) {};

    SpinLock(const SpinLock &) = delete;
    SpinLock &operator=(const SpinLock &) = delete;
    SpinLock(SpinLock &&) = delete;
    SpinLock &operator=(SpinLock &&) = delete;

    /// @brief Acquire the lock (bounded contended spin + hook decision).
    ///        Fast path takes the lock with no hook call. On contention,
    ///        spins at most CONFIG_LOCK_CONTEND_ROUNDS pause rounds (with
    ///        a tick-watchdog checkpoint every 256) and then invokes
    ///        spin_contended_hook() exactly once per exhaustion; Retry
    ///        re-enters the bounded loop, Acquired returns with the take
    ///        fully published by the hook. try_lock() never calls the hook.
    void lock() noexcept {
        if (!atomic_exchange(&locked_, 1, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&holder_, __builtin_return_address(0),
                             __ATOMIC_RELAXED);
            spinlock_note_acquire(this, rank_);
            return;
        }
        for (;;) {
            for (uint64_t round = 0;
                 round < static_cast<uint64_t>(CONFIG_LOCK_CONTEND_ROUNDS);
                 ++round) {
                arch::pause();
                if ((round & 255U) == 255U) {
                    kernel::tick_watchdog::checkpoint("lock_contend");
                }
                if (!atomic_exchange(&locked_, 1, __ATOMIC_ACQUIRE)) {
                    __atomic_store_n(&holder_, __builtin_return_address(0),
                                     __ATOMIC_RELAXED);
                    spinlock_note_acquire(this, rank_);
                    return;
                }
            }
            if (spin_contended_hook(*this) ==
                SpinContendAction::Acquired) {
                return;
            }
        }
    }

    /// @brief Acquire without ever invoking the contended hook (IRQ-combined
    ///        paths that must not park: the hook could sti inside a bare-cli
    ///        atomic region). Same bounded/checkpointed shape, unbounded
    ///        re-entry like the status-quo spin it replaces.
    void lock_irq_atomic() noexcept {
        if (!atomic_exchange(&locked_, 1, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&holder_, __builtin_return_address(0),
                             __ATOMIC_RELAXED);
            spinlock_note_acquire(this, rank_);
            return;
        }
        for (;;) {
            for (uint64_t round = 0;
                 round < static_cast<uint64_t>(CONFIG_LOCK_CONTEND_ROUNDS);
                 ++round) {
                arch::pause();
                if ((round & 255U) == 255U) {
                    kernel::tick_watchdog::checkpoint("lock_contend");
                }
                if (!atomic_exchange(&locked_, 1, __ATOMIC_ACQUIRE)) {
                    __atomic_store_n(&holder_, __builtin_return_address(0),
                                     __ATOMIC_RELAXED);
                    spinlock_note_acquire(this, rank_);
                    return;
                }
            }
        }
    }

    /// @brief Release the lock.
    void unlock() noexcept {
        spinlock_note_release(this);
        atomic_store(&locked_, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&holder_, nullptr, __ATOMIC_RELAXED);
    }

    /// @brief Force-unlock (used to initialise a lock after MemPool
    /// allocation).
    void reset() noexcept {
        locked_ = 0;
        holder_ = nullptr;
        // NOTE: rank_ survives reset (set once at declaration/init).
        // NOTE: no order-bookkeeping update — reset() runs on locks
        // that are not held (fresh or recycled blocks, never a live
        // critical section); an init-time unlock() of a never-locked
        // lock is an unpaired no-op by design (see spinlock_order).
    }

    /// @brief Attempt to acquire without blocking.
    /// @return true if the lock was acquired.
    bool try_lock() noexcept {
        bool acquired = !atomic_exchange(&locked_, 1, __ATOMIC_ACQUIRE);
        if (acquired) {
            __atomic_store_n(&holder_, __builtin_return_address(0),
                             __ATOMIC_RELAXED);
            spinlock_note_acquire(this, rank_);
        }
        return acquired;
    }

    const void *holder() const noexcept {
        return __atomic_load_n(&holder_, __ATOMIC_RELAXED);
    }

    /// @brief Diagnostic reads of the lock word (wedge hunt, #545).
    ///        Lock-free atomic loads; no state change.  Lets the park
    ///        exhaustion dump distinguish a poisoned/freed word (0xDD…),
    ///        an unpublished acquire (locked + holder NULL), and a
    ///        normally held lock.
    int is_locked_for_debug() const noexcept {
        return __atomic_load_n(&locked_, __ATOMIC_RELAXED);
    }
    const void *holder_for_debug() const noexcept {
        return __atomic_load_n(&holder_, __ATOMIC_RELAXED);
    }

    /// @brief Assign the lock-order rank (M2, #546).  Set once at
    ///        declaration/init; never mutated while contended.
    void set_rank(LockRank rank) noexcept {
        rank_ = static_cast<uint8_t>(rank);
    }

    /// @brief The lock-order rank (Unranked = invisible to the check).
    LockRank rank() const noexcept {
        return static_cast<LockRank>(rank_);
    }

  private:
    int locked_ = 0; ///< 0 = unlocked, 1 = locked.
    // M2 (issue #546): rank byte lives in the existing padding —
    // sizeof(SpinLock) stays 16 (static_assert below).
    uint8_t rank_ = static_cast<uint8_t>(LockRank::Unranked);
    const void *holder_ = nullptr;
};

static_assert(sizeof(sync::SpinLock) == 16,
              "SpinLock layout: rank_ must fit the existing padding (#546)");

} // namespace sync
} // namespace kernel
