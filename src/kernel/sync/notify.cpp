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
#include <kernel/arch/io.hpp>

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
///
/// Semaphore::wait() discipline (CODING_STYLE §11.1–§11.3, INV-2): the
/// waiter is registered and marked BLOCKED under the lock, the lock is
/// released before any scheduler call, the task is dequeued from the
/// ready queue, reschedule() arms the deferred switch, and the task
/// spins scheduler-mediated until the notifier wakes it.  A value that
/// arrived before the first wait is consumed without blocking, so an
/// ISR notify can never be lost when the handler has not dispatched
/// yet (issue #148: the old immediate-return path let
/// IrqThread::task_entry busy-spin wait→handler→wait at raised priority
/// and wedge the guest silently).
uint64_t Notify::wait() {
    auto *task = Scheduler::current_task();
    if (!task)
        return 0;

    for (;;) {
        {
            SpinLockGuard<SpinLock> guard(lock_);
            if (waiter_ == nullptr) {
                if (notify_value_ != 0) {
                    uint64_t value = notify_value_;
                    notify_value_ = 0;
                    return value;
                }
                waiter_ = task;
                waiter_gen_ = task->generation;
                task->state = TaskState::BLOCKED;
                break;
            }
            // A waiter is already registered (re-entered wait while still
            // registered, or a foreign waiter): release the lock, yield,
            // and retry — never return without a reschedule (§11.3b).
        }
        Scheduler::reschedule();
        arch::pause();
    }

    // Block OUTSIDE the lock (C-1): never hold a spinlock across the
    // deferred switch — the ISR-side notify() would spin forever on it.
    // Dequeue so a BLOCKED task is never physically queued (INV-2
    // desync — release-build live-lock if the switch applies late).
    Scheduler::dequeue_ready(*task);
    Scheduler::reschedule();

    // reschedule() is deferred (INV-4): keep running with state=BLOCKED
    // until the timer ISR applies the switch and notify() wakes us.
    // Spin-wait (mirrors Semaphore::wait).  If interrupts are off the
    // ISR cannot fire — roll back and leave without a value.
    if (arch::interrupts_enabled()) {
        while (task->state == TaskState::BLOCKED) {
            arch::pause();
        }
        SpinLockGuard<SpinLock> guard(lock_);
        uint64_t value = notify_value_;
        notify_value_ = 0;
        return value;
    }
    {
        SpinLockGuard<SpinLock> guard(lock_);
        if (waiter_ == task && waiter_gen_ == task->generation) {
            waiter_ = nullptr;
            waiter_gen_ = 0;
        }
    }
    task->state = TaskState::RUNNING;
    Scheduler::enqueue_ready(*task);
    return 0;
}

/// @brief Block until notified (error-returning overload).
///
/// Same Semaphore::wait_err() discipline as wait(): consume a pending
/// value without blocking, else register + BLOCKED under the lock and
/// block scheduler-mediated outside it.  A second waiter is rejected
/// with SYNC_ERR_ALREADY_WAITING; an interrupts-disabled caller rolls
/// back with SYNC_ERR_INTERRUPTED.
errors::SyncError Notify::wait_err(uint64_t *out_value) {
    auto *task = Scheduler::current_task();
    if (!task)
        return errors::SYNC_ERR_NO_TASK;

    {
        SpinLockGuard<SpinLock> guard(lock_);
        if (waiter_ != nullptr)
            return errors::SYNC_ERR_ALREADY_WAITING;

        if (notify_value_ != 0) {
            if (out_value)
                *out_value = notify_value_;
            notify_value_ = 0;
            return errors::SYNC_ERR_OK;
        }

        waiter_ = task;
        waiter_gen_ = task->generation;
        task->state = TaskState::BLOCKED;
    } // lock released BEFORE reschedule (see wait())

    // Same lock-scope / dequeue discipline as wait() (C-1/C-2).
    Scheduler::dequeue_ready(*task);
    Scheduler::reschedule();

    if (arch::interrupts_enabled()) {
        while (task->state == TaskState::BLOCKED) {
            arch::pause();
        }
        SpinLockGuard<SpinLock> guard(lock_);
        if (out_value)
            *out_value = notify_value_;
        notify_value_ = 0;
        return errors::SYNC_ERR_OK;
    }
    {
        SpinLockGuard<SpinLock> guard(lock_);
        if (waiter_ == task && waiter_gen_ == task->generation) {
            waiter_ = nullptr;
            waiter_gen_ = 0;
        }
    }
    task->state = TaskState::RUNNING;
    Scheduler::enqueue_ready(*task);
    return errors::SYNC_ERR_INTERRUPTED;
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
