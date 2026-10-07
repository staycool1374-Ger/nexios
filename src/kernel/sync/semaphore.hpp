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

/// @file semaphore.hpp
/// @brief Counting semaphore with blocking wait/post and priority-sorted
/// waiters.

#include <types.hpp>
#include <kernel/task/task.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/sync/sync_errors.hpp>

namespace kernel {
namespace sync {

class Semaphore {
  public:
    static constexpr size_t MAX_WAITERS = CONFIG_SYNC_MAX_WAITERS;

    Semaphore()
        : count_(0), max_count_(1), waiter_count_(0), owner_(nullptr),
          holder_priority_(0) {
    }
    /// @brief Initialize semaphore with a count and optional maximum.
    /// @param initial Starting count.
    /// @param max Maximum count (default unlimited).
    void init(uint64_t initial, uint64_t max = 0xFFFFFFFF);
    /// @brief Initialize semaphore with a count and optional maximum
    /// (error-returning overload).
    /// @return SYNC_ERR_OK on success, SYNC_ERR_ALREADY_INITIALIZED if already
    /// initialized.
    errors::SyncError init_err(uint64_t initial, uint64_t max = 0xFFFFFFFF);

    /// @brief Decrement count, blocking if zero.
    void wait();
    /// @brief Decrement count, blocking if zero (error-returning overload).
    /// @return SYNC_ERR_OK on success, SYNC_ERR_NO_TASK if no current task,
    /// SYNC_ERR_MAX_WAITERS if waiter limit reached.
    errors::SyncError wait_err();

    /// @brief Decrement count without blocking.
    /// @return true if the count was decremented.
    bool try_wait();
    /// @brief Decrement count without blocking (error-returning overload).
    /// @return SYNC_ERR_OK on success, SYNC_ERR_QUEUE_EMPTY if count is zero.
    errors::SyncError try_wait_err();

    /// @brief Increment count, waking a waiter if any.
    void post();
    /// @brief Increment count, waking a waiter if any (error-returning
    /// overload).
    /// @return SYNC_ERR_OK on success, SYNC_ERR_BUFFER_FULL if max count
    /// reached.
    errors::SyncError post_err();

    uint64_t value() const {
        return count_;
    }

    /// @brief Number of tasks currently blocked on this semaphore.
    size_t waiter_count() {
        SpinLockGuard<SpinLock> guard(lock_);
        return waiter_count_;
    }

    /// @brief Detach a task from this semaphore's waiter list (lock-safe).
    ///        Used by TaskControlBlock::cleanup() to unlink a reaped task
    ///        before its TCB is freed.  Verifies pointer AND generation
    ///        match (guards against a recycled TCB occupying the slot).
    /// @return true if the task was found and removed.
    bool remove_waiter(TaskControlBlock &task);

  private:
    SpinLock lock_;      ///< Protects all semaphore state.
    uint64_t count_;     ///< Current semaphore count.
    uint64_t max_count_; ///< Maximum count (capped on post).
    TaskControlBlock *waiters_[MAX_WAITERS]; ///< Array of waiting tasks.
    uint32_t waiter_gens_[MAX_WAITERS];      ///< Generations at insertion time.
    size_t waiter_count_; ///< Number of tasks waiting on this semaphore.
    TaskControlBlock
        *owner_; ///< Owner task for PIP (non-null when count < initial).
    uint64_t holder_priority_; ///< Saved original priority for PI restore.

    bool add_waiter(TaskControlBlock &task);
    void wake_one();
    void inherit_priority(TaskControlBlock &waiter);
    void restore_priority();
};

} // namespace sync
} // namespace kernel
