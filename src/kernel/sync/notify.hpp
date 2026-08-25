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

/// @file notify.hpp
/// @brief One-shot notification (signalling) primitive — single waiter, value
/// delivery.

#pragma once

#include <types.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/sync_errors.hpp>

namespace kernel {

// Forward declaration — Notify only stores a pointer to TCB.
struct TaskControlBlock;

namespace sync {

static constexpr uint64_t NOTIFY_INVALID = 0;

class Notify {
  public:
    Notify()
        : notify_value_(0), waiter_(nullptr), waiter_gen_(0),
          initialized_(false) {
    }
    /// @brief Destructor — wakes any waiter before the object is freed.
    ~Notify();
    /// @brief Initialize the notification object.
    void init();
    /// @brief Initialize the notification object (error-returning overload).
    /// @return SYNC_ERR_OK on success, SYNC_ERR_ALREADY_INITIALIZED if already
    /// initialized.
    errors::SyncError init_err();

    /// @brief Signal a waiter with a value, waking it.
    /// @param value The value to deliver to the waiter.
    void notify(uint64_t value);
    /// @brief Signal a waiter with a value, waking it (error-returning
    /// overload).
    /// @param value The value to deliver to the waiter.
    /// @return SYNC_ERR_OK on success, SYNC_ERR_NO_WAITER if no waiter.
    errors::SyncError notify_err(uint64_t value);

    /// @brief Block until notified.
    /// @return The value passed by the notifier.
    uint64_t wait();
    /// @brief Block until notified (error-returning overload).
    /// @param[out] out_value Receives the notification value.
    /// @return SYNC_ERR_OK on success, SYNC_ERR_NO_TASK if no current task,
    /// SYNC_ERR_ALREADY_WAITING if already has a waiter.
    errors::SyncError wait_err(uint64_t *out_value);

    /// @brief Check if notified without blocking.
    /// @param[out] value Receives the notification value if available.
    /// @return true if a notification was pending.
    bool try_wait(uint64_t *value);
    /// @brief Check if notified without blocking (error-returning overload).
    /// @param[out] value Receives the notification value if available.
    /// @return SYNC_ERR_OK on success, SYNC_ERR_BUFFER_EMPTY if no notification
    /// pending.
    errors::SyncError try_wait_err(uint64_t *value);

    uint64_t value() const {
        return notify_value_;
    }

  private:
    SpinLock lock_;          ///< Protects all notify state.
    uint64_t notify_value_;  ///< Value delivered to the waiter.
    TaskControlBlock *waiter_; ///< Currently waiting task (nullptr = none).
    uint64_t waiter_gen_;    ///< TCB generation captured at wait registration
                             ///< (H-2: defeats ABA if the TCB is recycled).
    bool initialized_;       ///< Whether init() has been called.
};

} // namespace sync
} // namespace kernel
