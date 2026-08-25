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

/// @file queue.cpp
/// @brief Message queue implementation — send, receive, try_send, try_receive
/// with blocking waiters.

#include <kernel/sync/queue.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <string.hpp>

namespace kernel {
namespace sync {

/// @brief Initialise the message queue to empty.
void Queue::init() {
    head_ = 0;
    tail_ = 0;
    count_ = 0;
    send_waiters_count_ = 0;
    recv_waiters_count_ = 0;
    last_sender_ = nullptr;
    last_receiver_ = nullptr;
    last_sender_gen_ = 0;
    last_receiver_gen_ = 0;
    send_holder_prio_ = 0;
    recv_holder_prio_ = 0;
}

/// @brief Initialise the message queue (error-returning overload).
errors::SyncError Queue::init_err() {
    if (count_ != 0 || send_waiters_count_ != 0 || recv_waiters_count_ != 0) {
        return errors::SYNC_ERR_ALREADY_INITIALIZED;
    }
    head_ = 0;
    tail_ = 0;
    count_ = 0;
    send_waiters_count_ = 0;
    recv_waiters_count_ = 0;
    last_sender_ = nullptr;
    last_receiver_ = nullptr;
    last_sender_gen_ = 0;
    last_receiver_gen_ = 0;
    send_holder_prio_ = 0;
    recv_holder_prio_ = 0;
    return errors::SYNC_ERR_OK;
}

/// @brief Add a task to the send-waiter array (caller must hold lock_).
bool Queue::add_send_waiter(TaskControlBlock &task) {
    if (send_waiters_count_ >= MAX_WAITERS)
        return false;
    send_waiters_[send_waiters_count_] = &task;
    send_waiter_gens_[send_waiters_count_] = task.generation;
    task.waiting_on_queue = this;
    ++send_waiters_count_;
    return true;
}

/// @brief Add a task to the recv-waiter array (caller must hold lock_).
bool Queue::add_recv_waiter(TaskControlBlock &task) {
    if (recv_waiters_count_ >= MAX_WAITERS)
        return false;
    recv_waiters_[recv_waiters_count_] = &task;
    recv_waiter_gens_[recv_waiters_count_] = task.generation;
    task.waiting_on_queue = this;
    ++recv_waiters_count_;
    return true;
}

/// @brief Wake the highest-priority send waiter (caller must hold lock_).
void Queue::wake_send_one() {
    if (send_waiters_count_ == 0)
        return;

    for (size_t i = 0; i < send_waiters_count_;) {
        if (send_waiters_[i]->generation != send_waiter_gens_[i]) {
            send_waiters_[i] = send_waiters_[--send_waiters_count_];
            send_waiter_gens_[i] = send_waiter_gens_[send_waiters_count_];
        } else {
            ++i;
        }
    }

    if (send_waiters_count_ == 0)
        return;

    size_t best = 0;
    for (size_t i = 1; i < send_waiters_count_; ++i) {
        if (send_waiters_[i]->priority > send_waiters_[best]->priority)
            best = i;
    }
    // Harden: a cleaned-up task is REAPED, not TERMINATED.  Never feed a
    // freed TCB to set_task_ready (ready-queue corruption / UAF).
    if (send_waiters_[best]->state != TaskState::TERMINATED &&
        send_waiters_[best]->state != TaskState::REAPED) {
        send_waiters_[best]->waiting_on_queue = nullptr;
        Scheduler::set_task_ready(*send_waiters_[best]);
    }
    send_waiters_[best] = send_waiters_[--send_waiters_count_];
    send_waiter_gens_[best] = send_waiter_gens_[send_waiters_count_];
}

/// @brief Wake the highest-priority recv waiter (caller must hold lock_).
void Queue::wake_recv_one() {
    if (recv_waiters_count_ == 0)
        return;

    for (size_t i = 0; i < recv_waiters_count_;) {
        if (recv_waiters_[i]->generation != recv_waiter_gens_[i]) {
            recv_waiters_[i] = recv_waiters_[--recv_waiters_count_];
            recv_waiter_gens_[i] = recv_waiter_gens_[recv_waiters_count_];
        } else {
            ++i;
        }
    }

    if (recv_waiters_count_ == 0)
        return;

    size_t best = 0;
    for (size_t i = 1; i < recv_waiters_count_; ++i) {
        if (recv_waiters_[i]->priority > recv_waiters_[best]->priority)
            best = i;
    }
    // Harden: a cleaned-up task is REAPED, not TERMINATED.  Never feed a
    // freed TCB to set_task_ready (ready-queue corruption / UAF).
    if (recv_waiters_[best]->state != TaskState::TERMINATED &&
        recv_waiters_[best]->state != TaskState::REAPED) {
        recv_waiters_[best]->waiting_on_queue = nullptr;
        Scheduler::set_task_ready(*recv_waiters_[best]);
    }
    recv_waiters_[best] = recv_waiters_[--recv_waiters_count_];
    recv_waiter_gens_[best] = recv_waiter_gens_[recv_waiters_count_];
}

/// @brief Remove a specific task from the send-waiter array (caller must hold
///        lock_).  Used to roll back a block attempt when interrupts are
///        disabled and the deferred switch can never apply.
void Queue::remove_send_waiter(TaskControlBlock &task) {
    for (size_t i = 0; i < send_waiters_count_;) {
        if (send_waiters_[i] == &task &&
            send_waiter_gens_[i] == task.generation) {
            send_waiters_[i] = send_waiters_[--send_waiters_count_];
            send_waiter_gens_[i] = send_waiter_gens_[send_waiters_count_];
            task.waiting_on_queue = nullptr;
            return;
        }
        ++i;
    }
}

/// @brief Remove a specific task from the recv-waiter array (caller must hold
///        lock_).  Used to roll back a block attempt when interrupts are
///        disabled and the deferred switch can never apply.
void Queue::remove_recv_waiter(TaskControlBlock &task) {
    for (size_t i = 0; i < recv_waiters_count_;) {
        if (recv_waiters_[i] == &task &&
            recv_waiter_gens_[i] == task.generation) {
            recv_waiters_[i] = recv_waiters_[--recv_waiters_count_];
            recv_waiter_gens_[i] = recv_waiter_gens_[recv_waiters_count_];
            task.waiting_on_queue = nullptr;
            return;
        }
        ++i;
    }
}

/// @brief Detach a task from the send- or recv-waiter array (lock-safe).
///        Verifies pointer AND generation match so a recycled TCB that now
///        occupies the slot is never removed.
/// @return true if the task was found and removed.
bool Queue::remove_waiter(TaskControlBlock &task) {
    SpinLockGuard<SpinLock> guard(lock_);
    size_t before = send_waiters_count_ + recv_waiters_count_;
    remove_send_waiter(task);
    remove_recv_waiter(task);
    return (send_waiters_count_ + recv_waiters_count_) != before;
}

/// @brief Send a message, blocking if the queue is full.
bool Queue::send(const uint8_t *data, size_t size) {
    if (size > QUEUE_MAX_MSG_SIZE)
        return false;

    auto *task = Scheduler::current_task();

    for (;;) {
        {
            SpinLockGuard<SpinLock> guard(lock_);
            last_sender_ = task;
            last_sender_gen_ = task->generation;
            if (!is_full()) {
                memcpy(msgs_[tail_].data, data, size);
                msgs_[tail_].size = size;
                tail_ = (tail_ + 1) % QUEUE_MAX_MSG_COUNT;
                ++count_;
                restore_receiver();
                wake_recv_one();
                return true;
            }
            boost_receiver(*task);
            if (!add_send_waiter(*task))
                return false;
            // H-7: perform the BLOCKED transition inside the lock so no
            // observer can see the waiter registered but still RUNNING, nor
            // a half-transitioned state between the unlocked store and the
            // deferred switch.
            task->state = TaskState::BLOCKED;
        }

        // Block OUTSIDE the queue lock so the receiver can drain the queue and
        // wake us (holding lock_ across the block would deadlock the drain).
        Scheduler::dequeue_ready(*task);
        Scheduler::reschedule();

        // reschedule() is deferred (INV-4): the current task keeps running
        // with state=BLOCKED until the timer ISR applies the switch and the
        // receiver drains + wakes us.  Spin-wait (mirrors IPC::send).  If
        // interrupts are off the ISR can't fire — roll back and fail.
        if (arch::interrupts_enabled()) {
            while (task->state == TaskState::BLOCKED) {
                arch::pause();
            }
        } else {
            remove_send_waiter(*task);
            task->state = TaskState::RUNNING;
            Scheduler::enqueue_ready(*task);
            return false;
        }
        // Woken — re-check for space.
    }
}

/// @brief Send a message, blocking if full (error-returning overload).
errors::SyncError Queue::send_err(const uint8_t *data, size_t size,
                                  size_t *sent_bytes) {
    if (size > QUEUE_MAX_MSG_SIZE)
        return errors::SYNC_ERR_MSG_TOO_LARGE;

    auto *task = Scheduler::current_task();
    if (!task)
        return errors::SYNC_ERR_NO_TASK;

    for (;;) {
        {
            SpinLockGuard<SpinLock> guard(lock_);
            last_sender_ = task;
            last_sender_gen_ = task->generation;
            if (!is_full()) {
                memcpy(msgs_[tail_].data, data, size);
                msgs_[tail_].size = size;
                tail_ = (tail_ + 1) % QUEUE_MAX_MSG_COUNT;
                ++count_;
                restore_receiver();
                wake_recv_one();
                if (sent_bytes)
                    *sent_bytes = size;
                return errors::SYNC_ERR_OK;
            }
            if (send_waiters_count_ >= MAX_WAITERS)
                return errors::SYNC_ERR_MAX_WAITERS;
            boost_receiver(*task);
            add_send_waiter(*task);
            // H-7: BLOCKED transition inside the lock (see send()).
            task->state = TaskState::BLOCKED;
        }

        Scheduler::dequeue_ready(*task);
        Scheduler::reschedule();

        if (arch::interrupts_enabled()) {
            while (task->state == TaskState::BLOCKED) {
                arch::pause();
            }
        } else {
            remove_send_waiter(*task);
            task->state = TaskState::RUNNING;
            Scheduler::enqueue_ready(*task);
            return errors::SYNC_ERR_MAX_WAITERS;
        }
    }
}

/// @brief Send a message without blocking.
bool Queue::try_send(const uint8_t *data, size_t size) {
    SpinLockGuard<SpinLock> guard(lock_);
    if (size > QUEUE_MAX_MSG_SIZE || is_full())
        return false;

    auto *snd = Scheduler::current_task();
    last_sender_ = snd;
    last_sender_gen_ = snd ? snd->generation : 0;
    memcpy(msgs_[tail_].data, data, size);
    msgs_[tail_].size = size;
    tail_ = (tail_ + 1) % QUEUE_MAX_MSG_COUNT;
    ++count_;

    restore_receiver();
    wake_recv_one();
    return true;
}

/// @brief Send a message without blocking (error-returning overload).
errors::SyncError Queue::try_send_err(const uint8_t *data, size_t size,
                                      size_t *sent_bytes) {
    SpinLockGuard<SpinLock> guard(lock_);
    if (size > QUEUE_MAX_MSG_SIZE)
        return errors::SYNC_ERR_MSG_TOO_LARGE;
    if (is_full())
        return errors::SYNC_ERR_QUEUE_FULL;

    auto *snd = Scheduler::current_task();
    last_sender_ = snd;
    last_sender_gen_ = snd ? snd->generation : 0;
    memcpy(msgs_[tail_].data, data, size);
    msgs_[tail_].size = size;
    tail_ = (tail_ + 1) % QUEUE_MAX_MSG_COUNT;
    ++count_;

    restore_receiver();
    wake_recv_one();

    if (sent_bytes)
        *sent_bytes = size;
    return errors::SYNC_ERR_OK;
}

/// @brief Receive a message, blocking if the queue is empty.
bool Queue::receive(uint8_t *buf, size_t *size) {
    auto *task = Scheduler::current_task();

    for (;;) {
        {
            SpinLockGuard<SpinLock> guard(lock_);
            last_receiver_ = task;
            last_receiver_gen_ = task->generation;
            if (!is_empty()) {
                size_t copy_size = msgs_[head_].size;
                if (buf && size) {
                    if (*size < copy_size)
                        copy_size = *size;
                    memcpy(buf, msgs_[head_].data, copy_size);
                    *size = copy_size;
                }

                head_ = (head_ + 1) % QUEUE_MAX_MSG_COUNT;
                --count_;

                restore_sender();
                wake_send_one();
                return true;
            }
            boost_sender(*task);
            if (!add_recv_waiter(*task))
                return false;
            // H-7: BLOCKED transition inside the lock (see send()).
            task->state = TaskState::BLOCKED;
        }

        // Block OUTSIDE the queue lock so a sender can enqueue and wake us.
        Scheduler::dequeue_ready(*task);
        Scheduler::reschedule();

        // reschedule() is deferred (INV-4): spin-wait until the timer ISR
        // applies the switch and the sender enqueues + wakes us.  If
        // interrupts are off the ISR can't fire — roll back and fail.
        if (arch::interrupts_enabled()) {
            while (task->state == TaskState::BLOCKED) {
                arch::pause();
            }
        } else {
            remove_recv_waiter(*task);
            task->state = TaskState::RUNNING;
            Scheduler::enqueue_ready(*task);
            return false;
        }
        // Woken — re-check for a message.
    }
}

/// @brief Receive a message, blocking if empty (error-returning overload).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
errors::SyncError Queue::receive_err(uint8_t *buf, size_t *size,
                                     size_t *received_bytes) {
    auto *task = Scheduler::current_task();
    if (!task)
        return errors::SYNC_ERR_NO_TASK;

    for (;;) {
        {
            SpinLockGuard<SpinLock> guard(lock_);
            last_receiver_ = task;
            last_receiver_gen_ = task->generation;
            if (!is_empty()) {
                size_t copy_size = msgs_[head_].size;
                if (buf && size) {
                    if (*size < copy_size)
                        copy_size = *size;
                    memcpy(buf, msgs_[head_].data, copy_size);
                    *size = copy_size;
                }

                head_ = (head_ + 1) % QUEUE_MAX_MSG_COUNT;
                --count_;

                restore_sender();
                wake_send_one();

                if (received_bytes)
                    *received_bytes = copy_size;
                return errors::SYNC_ERR_OK;
            }
            if (recv_waiters_count_ >= MAX_WAITERS)
                return errors::SYNC_ERR_MAX_WAITERS;
            boost_sender(*task);
            add_recv_waiter(*task);
            // H-7: BLOCKED transition inside the lock (see send()).
            task->state = TaskState::BLOCKED;
        }

        Scheduler::dequeue_ready(*task);
        Scheduler::reschedule();

        if (arch::interrupts_enabled()) {
            while (task->state == TaskState::BLOCKED) {
                arch::pause();
            }
        } else {
            remove_recv_waiter(*task);
            task->state = TaskState::RUNNING;
            Scheduler::enqueue_ready(*task);
            return errors::SYNC_ERR_MAX_WAITERS;
        }
    }
}

/// @brief Receive a message without blocking.
bool Queue::try_receive(uint8_t *buf, size_t *size) {
    SpinLockGuard<SpinLock> guard(lock_);
    if (is_empty())
        return false;

    auto *rcv = Scheduler::current_task();
    last_receiver_ = rcv;
    last_receiver_gen_ = rcv ? rcv->generation : 0;
    size_t copy_size = msgs_[head_].size;
    if (buf && size) {
        if (*size < copy_size)
            copy_size = *size;
        memcpy(buf, msgs_[head_].data, copy_size);
        *size = copy_size;
    }

    head_ = (head_ + 1) % QUEUE_MAX_MSG_COUNT;
    --count_;

    restore_sender();
    wake_send_one();
    return true;
}

/// @brief Receive a message without blocking (error-returning overload).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
errors::SyncError Queue::try_receive_err(uint8_t *buf, size_t *size,
                                         size_t *received_bytes) {
    SpinLockGuard<SpinLock> guard(lock_);
    if (is_empty())
        return errors::SYNC_ERR_QUEUE_EMPTY;

    auto *rcv = Scheduler::current_task();
    last_receiver_ = rcv;
    last_receiver_gen_ = rcv ? rcv->generation : 0;
    size_t copy_size = msgs_[head_].size;
    if (buf && size) {
        if (*size < copy_size)
            copy_size = *size;
        memcpy(buf, msgs_[head_].data, copy_size);
        *size = copy_size;
    }

    head_ = (head_ + 1) % QUEUE_MAX_MSG_COUNT;
    --count_;

    restore_sender();
    wake_send_one();

    if (received_bytes)
        *received_bytes = copy_size;
    return errors::SYNC_ERR_OK;
}

//
// --- Priority Inheritance Protocol (PIP) helpers ---
//

/// @brief Boost the last receiver when a high-prio sender blocks on a full
/// queue.
///
/// H-3 (audit-task-sync-v0.4.2): (a) never dereference a freed/recycled TCB —
/// validate against REAPED state and the generation captured when
/// last_receiver_ was set (a TERMINATED zombie is still allocated and safe to
/// boost; a REAPED or recycled block is not); (b) route the priority mutation
/// through the scheduler's re-bucketing pattern (mirrors ipc.cpp:block_sender
/// / Scheduler::set_priority) so a boosted task sitting in the ready queue
/// moves to its new priority bucket.
void Queue::boost_receiver(TaskControlBlock &blocked_sender) {
#if CONFIG_QUEUE_PIP
    if (!last_receiver_)
        return;
    if (last_receiver_->state == TaskState::REAPED ||
        last_receiver_->generation != last_receiver_gen_) {
        last_receiver_ = nullptr;
        recv_holder_prio_ = 0;
        return;
    }
    if (blocked_sender.priority > last_receiver_->priority) {
        // IrqGuard excludes the timer ISR (deadline demote / sporadic
        // priority changes) so the old/new effective_priority snapshots stay
        // consistent — same discipline as ipc.cpp:block_sender.
        arch::IrqGuard irq_guard{};
        TaskControlBlock &receiver = *last_receiver_;
        uint64_t old_prio = Scheduler::effective_priority(&receiver);
        if (recv_holder_prio_ == 0)
            recv_holder_prio_ = receiver.priority;
        receiver.priority = blocked_sender.priority;
        uint64_t new_prio = Scheduler::effective_priority(&receiver);
        // Only re-bucket a ready-queue member; a BLOCKED receiver has no
        // bucket and will be enqueued at the boosted priority on wake.
        if (old_prio != new_prio && receiver.in_ready_queue_)
            Scheduler::move_priority(receiver, old_prio, new_prio);
    }
#else
    (void)blocked_sender;
#endif
}

/// @brief Boost the last sender when a high-prio receiver blocks on an empty
/// queue.
///
/// H-3: same freed-state validation + re-bucketing discipline as
/// boost_receiver.
void Queue::boost_sender(TaskControlBlock &blocked_receiver) {
#if CONFIG_QUEUE_PIP
    if (!last_sender_)
        return;
    if (last_sender_->state == TaskState::REAPED ||
        last_sender_->generation != last_sender_gen_) {
        last_sender_ = nullptr;
        send_holder_prio_ = 0;
        return;
    }
    if (blocked_receiver.priority > last_sender_->priority) {
        arch::IrqGuard irq_guard{};
        TaskControlBlock &sender = *last_sender_;
        uint64_t old_prio = Scheduler::effective_priority(&sender);
        if (send_holder_prio_ == 0)
            send_holder_prio_ = sender.priority;
        sender.priority = blocked_receiver.priority;
        uint64_t new_prio = Scheduler::effective_priority(&sender);
        if (old_prio != new_prio && sender.in_ready_queue_)
            Scheduler::move_priority(sender, old_prio, new_prio);
    }
#else
    (void)blocked_receiver;
#endif
}

/// @brief Restore the last receiver's priority after a message is enqueued.
/// H-3: freed-state validation + re-bucketing on the way back down.
void Queue::restore_receiver() {
#if CONFIG_QUEUE_PIP
    if (!last_receiver_ || recv_holder_prio_ == 0)
        return;
    if (last_receiver_->state == TaskState::REAPED ||
        last_receiver_->generation != last_receiver_gen_) {
        last_receiver_ = nullptr;
        recv_holder_prio_ = 0;
        return;
    }
    {
        arch::IrqGuard irq_guard{};
        TaskControlBlock &receiver = *last_receiver_;
        uint64_t old_prio = Scheduler::effective_priority(&receiver);
        receiver.priority = recv_holder_prio_;
        uint64_t new_prio = Scheduler::effective_priority(&receiver);
        if (old_prio != new_prio && receiver.in_ready_queue_)
            Scheduler::move_priority(receiver, old_prio, new_prio);
    }
    recv_holder_prio_ = 0;
#endif
}

/// @brief Restore the last sender's priority after a message is dequeued.
/// H-3: freed-state validation + re-bucketing on the way back down.
void Queue::restore_sender() {
#if CONFIG_QUEUE_PIP
    if (!last_sender_ || send_holder_prio_ == 0)
        return;
    if (last_sender_->state == TaskState::REAPED ||
        last_sender_->generation != last_sender_gen_) {
        last_sender_ = nullptr;
        send_holder_prio_ = 0;
        return;
    }
    {
        arch::IrqGuard irq_guard{};
        TaskControlBlock &sender = *last_sender_;
        uint64_t old_prio = Scheduler::effective_priority(&sender);
        sender.priority = send_holder_prio_;
        uint64_t new_prio = Scheduler::effective_priority(&sender);
        if (old_prio != new_prio && sender.in_ready_queue_)
            Scheduler::move_priority(sender, old_prio, new_prio);
    }
    send_holder_prio_ = 0;
#endif
}

} // namespace sync
} // namespace kernel
