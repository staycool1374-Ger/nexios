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

/// @file notify.cpp
/// @brief One-shot notification implementation — notify, wait, try_wait.

#include <kernel/sync/notify.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/sync/spinlock_guard.hpp>

namespace kernel {
namespace sync {

/// @brief Destructor — wakes any waiter before the object is freed.
Notify::~Notify() {
    SpinLockGuard<SpinLock> guard(lock_);
    if (waiter_) {
        // H-2: a cleaned-up task is REAPED, not TERMINATED.  Never feed a
        // freed TCB to set_task_ready (ready-queue corruption / UAF).  Also
        // require the generation captured at wait time — a recycled TCB that
        // now occupies the slot must not be woken.
        if (waiter_->state != TaskState::TERMINATED &&
            waiter_->state != TaskState::REAPED &&
            waiter_->generation == waiter_gen_) {
            Scheduler::set_task_ready(*waiter_);
        }
        waiter_ = nullptr;
        waiter_gen_ = 0;
    }
    initialized_ = false;
}

/// @brief Initialise the notification object.
void Notify::init() {
    lock_.reset();
    notify_value_ = 0;
    waiter_ = nullptr;
    waiter_gen_ = 0;
    initialized_ = true;
}

/// @brief Initialise the notification object (error-returning overload).
errors::SyncError Notify::init_err() {
    if (initialized_) {
        return errors::SYNC_ERR_ALREADY_INITIALIZED;
    }
    notify_value_ = 0;
    waiter_ = nullptr;
    waiter_gen_ = 0;
    initialized_ = true;
    return errors::SYNC_ERR_OK;
}

/// @brief Signal a waiter with a value, waking it.
void Notify::notify(uint64_t value) {
    SpinLockGuard<SpinLock> guard(lock_);
    notify_value_ = value;
    if (waiter_) {
        if (waiter_->state != TaskState::TERMINATED &&
            waiter_->state != TaskState::REAPED &&
            waiter_->generation == waiter_gen_) {
            Scheduler::set_task_ready(*waiter_);
        }
        waiter_ = nullptr;
        waiter_gen_ = 0;
    }
}

/// @brief Signal a waiter with a value (error-returning overload).
errors::SyncError Notify::notify_err(uint64_t value) {
    SpinLockGuard<SpinLock> guard(lock_);
    notify_value_ = value;
    if (waiter_) {
        if (waiter_->state != TaskState::TERMINATED &&
            waiter_->state != TaskState::REAPED &&
            waiter_->generation == waiter_gen_) {
            Scheduler::set_task_ready(*waiter_);
        }
        waiter_ = nullptr;
        waiter_gen_ = 0;
        return errors::SYNC_ERR_OK;
    }
    return errors::SYNC_ERR_NO_WAITER;
}

/// @brief Block until notified. Returns the notifier's value.
uint64_t Notify::wait() {
    auto *task = Scheduler::current_task();
    if (!task)
        return 0;

    {
        SpinLockGuard<SpinLock> guard(lock_);
        if (waiter_ != nullptr)
            return 0;

        waiter_ = task;
        waiter_gen_ = task->generation;
        task->state = TaskState::BLOCKED;
    } // lock released BEFORE reschedule: never hold a spinlock across a
      // context switch.  reschedule() arms a deferred switch and re-enables
      // IRQs; a timer tick before this frame unwinds would save the task
      // holding lock_, and the ISR-side notify() would spin forever on it
      // (threaded-IRQ keyboard deadlock).

    Scheduler::reschedule();

    return notify_value_;
}

/// @brief Block until notified (error-returning overload).
errors::SyncError Notify::wait_err(uint64_t *out_value) {
    auto *task = Scheduler::current_task();
    if (!task)
        return errors::SYNC_ERR_NO_TASK;

    {
        SpinLockGuard<SpinLock> guard(lock_);
        if (waiter_ != nullptr)
            return errors::SYNC_ERR_ALREADY_WAITING;

        waiter_ = task;
        waiter_gen_ = task->generation;
        task->state = TaskState::BLOCKED;
    } // lock released BEFORE reschedule (see wait())

    Scheduler::reschedule();

    if (out_value)
        *out_value = notify_value_;
    return errors::SYNC_ERR_OK;
}

/// @brief Check if notified without blocking.
bool Notify::try_wait(uint64_t *value) {
    SpinLockGuard<SpinLock> guard(lock_);
    if (waiter_ == nullptr && value && notify_value_ != 0) {
        *value = notify_value_;
        notify_value_ = 0;
        return true;
    }
    return false;
}

/// @brief Check if notified without blocking (error-returning overload).
errors::SyncError Notify::try_wait_err(uint64_t *value) {
    SpinLockGuard<SpinLock> guard(lock_);
    if (waiter_ == nullptr && value && notify_value_ != 0) {
        *value = notify_value_;
        notify_value_ = 0;
        return errors::SYNC_ERR_OK;
    }
    return errors::SYNC_ERR_BUFFER_EMPTY;
}

} // namespace sync
} // namespace kernel
