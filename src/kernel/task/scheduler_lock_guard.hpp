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

/// @file scheduler_lock_guard.hpp
/// @brief Checked guard + per-CPU take-depth for scheduler_lock_ (#417).

#include <types.hpp>
#include <kernel/nexios_config.h>
#include <kernel/arch/cpu_context.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>

namespace kernel {

/// @brief Per-CPU scheduler_lock_ take depth (#417; defined in
///        core/global_state.cpp, zero-initialised).  Slot [cpu] counts
///        takes of scheduler_lock_ by that CPU not yet released.
///        Single-CPU invariant: 0 outside a take, 1 inside (the lock is
///        non-recursive).  SMP: a CPU only ever touches its own slot.
///        NOT part of SchedPerCpuPod snapshots: the depth is transient
///        by construction — test boundaries and quiesce run with the
///        lock released, the same assumption the old global holder_
///        slot already relied on.
extern "C" {
extern uint64_t scheduler_lock_depth[CONFIG_MAX_CPUS];
} // extern "C"

/// @brief This CPU's take depth — the per-CPU equivalent of the old
///        global holder() check.  Returns 0 iff this CPU holds nothing.
inline uint64_t scheduler_lock_depth_own() noexcept {
    return scheduler_lock_depth[arch::cpu_index()];
}

/// @brief Checked RAII guard for scheduler_lock_ ONLY (#417).
///        Drop-in replacement for SpinLockGuard<sync::SpinLock> on
///        scheduler_lock_: the blocking constructor takes the lock,
///        the adopt constructor takes ownership of a lock acquired
///        via try_lock(); both mark this CPU's depth slot, the
///        destructor (or early unlock()) clears it.  Mark AFTER a
///        successful take and unmark BEFORE release, so an observer
///        never sees a taken-but-unmarked slot (a same-CPU re-take
///        then trips the entry ENSUREs deterministically); another
///        CPU's slot is never touched, so legal cross-CPU contention
///        spins instead of tripping (the #417 fix).  Free functions
///        must not outlive the scope (same rule as SpinLockGuard).
class SchedulerLockGuard {
  public:
    /// @brief Acquire the lock, then mark this CPU's depth slot.
    explicit SchedulerLockGuard(sync::SpinLock &lock) noexcept
        : lock_(lock), owns_(true) {
        lock_.lock();
        ++scheduler_lock_depth[arch::cpu_index()];
    }
    /// @brief Adopt an already-acquired lock (caller must have taken
    ///        it via lock() or a successful try_lock()).
    explicit SchedulerLockGuard(sync::SpinLock &lock,
                                adopt_lock_t) noexcept
        : lock_(lock), owns_(true) {
        // Adopt: the caller holds the lock (a racing take by another
        // CPU blocks or fails its try; only this CPU's paths index
        // this CPU's slot), so marking here is race-free.
        ++scheduler_lock_depth[arch::cpu_index()];
    }
    ~SchedulerLockGuard() noexcept {
        if (owns_) {
            // Unmark BEFORE release: with the slot clear first, a racing
            // same-CPU observer sees depth 0 only when the lock is
            // actually free-or-freeing (a try_lock in between still
            // fails — the lock itself is still held here).
            --scheduler_lock_depth[arch::cpu_index()];
            lock_.unlock();
        }
    }
    /// @brief True while this guard holds the lock and will release it.
    bool owns_lock() const noexcept { return owns_; }
    /// @brief Release early (the destructor becomes a no-op).
    void unlock() noexcept {
        if (owns_) {
            owns_ = false;
            --scheduler_lock_depth[arch::cpu_index()];
            lock_.unlock();
        }
    }
    SchedulerLockGuard(const SchedulerLockGuard &) = delete;
    SchedulerLockGuard &operator=(const SchedulerLockGuard &) = delete;
    SchedulerLockGuard(SchedulerLockGuard &&) = delete;
    SchedulerLockGuard &operator=(SchedulerLockGuard &&) = delete;

  private:
    sync::SpinLock &lock_;
    bool owns_ = false;
};

} // namespace kernel
