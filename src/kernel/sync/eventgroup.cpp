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

/// @file eventgroup.cpp
/// @brief EventGroup implementation — set/clear bits, wait bits, try-wait.

#include <kernel/sync/eventgroup.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/arch/io.hpp>
#include <assert.hpp>

namespace kernel {
namespace sync {

/// @brief Destructor — wakes all waiters before the object is freed.
EventGroup::~EventGroup() {
    SpinLockGuard<SpinLock> guard(lock_);
    for (size_t i = 0; i < wait_count_; ++i) {
        if (waiters_[i].task &&
            waiters_[i].task->state != TaskState::TERMINATED &&
            waiters_[i].task->state != TaskState::REAPED) {
            waiters_[i].task->waiting_on_eventgroup = nullptr;
            Scheduler::set_task_ready(*waiters_[i].task);
        }
    }
    wait_count_ = 0;
    bits_ = 0;
}

/// @brief Initialise the event group to zero bits.
void EventGroup::init() {
    lock_.reset();
    bits_ = 0;
    wait_count_ = 0;
}

/// @brief Initialise the event group (error-returning overload).
errors::SyncError EventGroup::init_err() {
    if (wait_count_ != 0 || bits_ != 0) {
        return errors::SYNC_ERR_ALREADY_INITIALIZED;
    }
    bits_ = 0;
    wait_count_ = 0;
    return errors::SYNC_ERR_OK;
}

/// @brief Atomically set bits and wake waiters whose conditions are now met.
void EventGroup::set_bits(uint64_t bits) {
    SpinLockGuard<SpinLock> guard(lock_);
    bits_ |= bits;
    wake_matching();
}

/// @brief Atomically set bits (error-returning overload).
errors::SyncError EventGroup::set_bits_err(uint64_t bits) {
    SpinLockGuard<SpinLock> guard(lock_);
    bits_ |= bits;
    wake_matching();
    return errors::SYNC_ERR_OK;
}

/// @brief Atomically clear bits.
void EventGroup::clear_bits(uint64_t bits) {
    SpinLockGuard<SpinLock> guard(lock_);
    bits_ &= ~bits;
}

/// @brief Atomically clear bits (error-returning overload).
errors::SyncError EventGroup::clear_bits_err(uint64_t bits) {
    SpinLockGuard<SpinLock> guard(lock_);
    bits_ &= ~bits;
    return errors::SYNC_ERR_OK;
}

/// @brief Add a task to the waiter array (caller must hold lock_).
bool EventGroup::add_waiter(TaskControlBlock &task, uint64_t wanted,
                            bool clear) {
    // Caller must hold lock_ (wait_bits already holds it)
    if (wait_count_ >= MAX_WAITERS)
        return false;
    waiters_[wait_count_].task = &task;
    waiters_[wait_count_].wanted_bits = wanted;
    waiters_[wait_count_].clear_on_exit = clear;
    waiters_[wait_count_].generation = task.generation;
    task.waiting_on_eventgroup = this;
    ++wait_count_;
    return true;
}

/// @brief Detach a task from the waiter array (lock-safe).  Verifies pointer
///        AND generation match so a recycled TCB that now occupies the slot is
///        never removed.
/// @return true if the task was found and removed.
bool EventGroup::remove_waiter(TaskControlBlock &task) {
    SpinLockGuard<SpinLock> guard(lock_);
    for (size_t i = 0; i < wait_count_;) {
        if (waiters_[i].task == &task &&
            waiters_[i].generation == task.generation) {
            waiters_[i] = waiters_[--wait_count_];
            task.waiting_on_eventgroup = nullptr;
            return true;
        }
        ++i;
    }
    return false;
}

/// @brief Wake all waiters whose conditions are satisfied (caller must hold
/// lock_).
void EventGroup::wake_matching() {
    for (size_t i = 0; i < wait_count_;) {
        if (waiters_[i].task->generation != waiters_[i].generation) {
            waiters_[i] = waiters_[--wait_count_];
            continue;
        }
        if ((bits_ & waiters_[i].wanted_bits) == waiters_[i].wanted_bits) {
            if (waiters_[i].clear_on_exit) {
                bits_ &= ~waiters_[i].wanted_bits;
            }
            // Harden: a cleaned-up task is REAPED, not TERMINATED.  Never
            // feed a freed TCB to set_task_ready (ready-queue corruption /
            // UAF).  Note this drops the entry even for dead waiters (the
            // swap-remove below), keeping the array consistent.
            if (waiters_[i].task->state != TaskState::TERMINATED &&
                waiters_[i].task->state != TaskState::REAPED) {
                waiters_[i].task->waiting_on_eventgroup = nullptr;
                Scheduler::set_task_ready(*waiters_[i].task);
            }
            waiters_[i] = waiters_[--wait_count_];
        } else {
            ++i;
        }
    }
}

/// @brief Block until any of the requested bits are set.
///
/// H-1 fix (audit-task-sync-v0.4.2): the spinlock must NOT be held across
/// Scheduler::reschedule() — identical C-1 deadlock exposure as the semaphore.
/// The waiter-table-full path must never ENSURE(added): reachable resource
/// exhaustion is a retry condition, not an invariant violation.
uint64_t EventGroup::wait_bits(uint64_t bits, bool clear_on_exit) {
    auto *task = Scheduler::current_task();
    if (!task)
        return bits_;

    for (;;) {
        bool added = false;
        {
            SpinLockGuard<SpinLock> guard(lock_);
            if ((bits_ & bits) == bits) {
                if (clear_on_exit)
                    bits_ &= ~bits;
                return bits_;
            }
            added = add_waiter(*task, bits, clear_on_exit);
            if (added)
                task->state = TaskState::BLOCKED;
        }

        if (!added) {
            Scheduler::reschedule();
            continue;
        }
        break;
    }

    // Block OUTSIDE the lock (Notify::wait() pattern).  Dequeue from the ready
    // queue so a BLOCKED task is never physically queued (INV-2 desync).
    Scheduler::dequeue_ready(*task);
    Scheduler::reschedule();

    if (arch::interrupts_enabled()) {
        while (task->state == TaskState::BLOCKED) {
            arch::pause();
        }
        return bits_;
    }
    remove_waiter(*task);
    task->state = TaskState::RUNNING;
    Scheduler::enqueue_ready(*task);
    return bits_;
}

/// @brief Block until any of the requested bits are set (error-returning
/// overload).
errors::SyncError EventGroup::wait_bits_err(uint64_t bits, bool clear_on_exit,
                                            uint64_t *out_bits) {
    auto *task = Scheduler::current_task();
    if (!task)
        return errors::SYNC_ERR_NO_TASK;

    bool added = false;
    {
        SpinLockGuard<SpinLock> guard(lock_);
        if ((bits_ & bits) == bits) {
            if (clear_on_exit)
                bits_ &= ~bits;
            if (out_bits)
                *out_bits = bits_;
            return errors::SYNC_ERR_OK;
        }

        if (wait_count_ >= MAX_WAITERS) {
            return errors::SYNC_ERR_MAX_WAITERS;
        }

        added = add_waiter(*task, bits, clear_on_exit);
        if (added)
            task->state = TaskState::BLOCKED;
    }

    if (!added) {
        return errors::SYNC_ERR_MAX_WAITERS;
    }

    Scheduler::dequeue_ready(*task);
    Scheduler::reschedule();

    if (arch::interrupts_enabled()) {
        while (task->state == TaskState::BLOCKED) {
            arch::pause();
        }
    } else {
        remove_waiter(*task);
        task->state = TaskState::RUNNING;
        Scheduler::enqueue_ready(*task);
        return errors::SYNC_ERR_MAX_WAITERS;
    }

    if (out_bits)
        *out_bits = bits_;
    return errors::SYNC_ERR_OK;
}

/// @brief Check if bits are set without blocking.
///
/// M-6 serialization (audit-task-sync MEDIUM): read bits_ under the spinlock so
/// a concurrent set/clear can't return a stale answer.
bool EventGroup::try_wait_bits(uint64_t bits) {
    SpinLockGuard<SpinLock> guard(lock_);
    return (bits_ & bits) == bits;
}

/// @brief Check if bits are set without blocking (error-returning overload).
errors::SyncError EventGroup::try_wait_bits_err(uint64_t bits,
                                                bool *out_result) {
    SpinLockGuard<SpinLock> guard(lock_);
    bool result = (bits_ & bits) == bits;
    if (out_result)
        *out_result = result;
    return errors::SYNC_ERR_OK;
}

} // namespace sync
} // namespace kernel
