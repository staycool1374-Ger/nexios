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

/// @file eventgroup.hpp
/// @brief Event group (bitmask) — wait on any/set/clear with per-waiter
/// clear-on-exit.

#include <types.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/sync/sync_errors.hpp>

namespace kernel {

// Forward declaration — EventGroup only stores pointers to TCB.
struct TaskControlBlock;

namespace sync {

class EventGroup {
  public:
    static constexpr size_t MAX_WAITERS = CONFIG_SYNC_MAX_WAITERS;

    EventGroup() : bits_(0), wait_count_(0) {
    }
    /// @brief Destructor — wakes any waiters before the object is freed.
    ~EventGroup();
    /// @brief Initialize the event group to zero bits.
    void init();
    /// @brief Initialize the event group to zero bits (error-returning
    /// overload).
    /// @return SYNC_ERR_OK on success, SYNC_ERR_ALREADY_INITIALIZED if already
    /// initialized.
    errors::SyncError init_err();

    /// @brief Atomically set bits, waking satisfied waiters.
    void set_bits(uint64_t bits);
    /// @brief Atomically set bits, waking satisfied waiters (error-returning
    /// overload).
    /// @return SYNC_ERR_OK on success.
    errors::SyncError set_bits_err(uint64_t bits);

    /// @brief Atomically clear bits.
    void clear_bits(uint64_t bits);
    /// @brief Atomically clear bits (error-returning overload).
    /// @return SYNC_ERR_OK on success.
    errors::SyncError clear_bits_err(uint64_t bits);

    uint64_t get_bits() const {
        return bits_;
    }

    /// @brief Number of tasks currently blocked on this event group.
    size_t waiter_count() {
        SpinLockGuard<SpinLock> guard(lock_);
        return wait_count_;
    }

    /// @brief Detach a task from this event group's waiter list (lock-safe).
    ///        Used by TaskControlBlock::cleanup() to unlink a reaped task
    ///        before its TCB is freed.  Verifies pointer AND generation
    ///        match (guards against a recycled TCB occupying the slot).
    /// @return true if the task was found and removed.
    bool remove_waiter(TaskControlBlock &task);

    /// @brief Block until any of the requested bits are set.
    /// @param wanted Bitmask of bits to wait for.
    /// @param clear_on_exit If true, clear matched bits before returning.
    /// @return The bits that were set when the wait completed.
    uint64_t wait_bits(uint64_t wanted, bool clear_on_exit = false);
    /// @brief Block until any of the requested bits are set (error-returning
    /// overload).
    /// @param wanted Bitmask of bits to wait for.
    /// @param clear_on_exit If true, clear matched bits before returning.
    /// @param[out] out_bits The bits that were set when the wait completed.
    /// @return SYNC_ERR_OK on success, SYNC_ERR_NO_TASK if no current task,
    /// SYNC_ERR_MAX_WAITERS if waiter limit reached.
    errors::SyncError wait_bits_err(uint64_t wanted, bool clear_on_exit,
                                    uint64_t *out_bits);

    /// @brief Check if bits are set without blocking.
    /// @return true if any of the requested bits are currently set.
    bool try_wait_bits(uint64_t wanted);
    /// @brief Check if bits are set without blocking (error-returning
    /// overload).
    /// @param[out] out_result true if bits were set, false otherwise.
    /// @return SYNC_ERR_OK on success.
    errors::SyncError try_wait_bits_err(uint64_t wanted, bool *out_result);

  private:
    SpinLock lock_; ///< Protects all event-group state.
    uint64_t bits_; ///< Current event bits.

    /// @brief A task waiting for specific bits to become set.
    struct EventWaiter {
        TaskControlBlock *task; ///< Waiting task.
        uint64_t wanted_bits;   ///< Bitmask this task is waiting for.
        bool clear_on_exit;     ///< Clear matched bits on wake.
        uint32_t generation;    ///< task->generation at insertion time.
    };
    EventWaiter waiters_[MAX_WAITERS]; ///< Array of waiting tasks.
    size_t wait_count_;                ///< Number of waiting tasks.

    bool add_waiter(TaskControlBlock &task, uint64_t wanted, bool clear);
    void wake_matching();
};

} // namespace sync
} // namespace kernel
