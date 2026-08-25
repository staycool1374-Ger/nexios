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

/// @file queue.hpp
/// @brief Fixed-size message queue with blocking send/receive and
/// priority-sorted waiters.

#pragma once

#include <types.hpp>
#include <kernel/task/task.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/sync/sync_errors.hpp>

namespace kernel {
namespace sync {

/// Maximum payload size per queue message.
static constexpr size_t QUEUE_MAX_MSG_SIZE = 32;
/// Maximum number of messages that can be queued.
static constexpr size_t QUEUE_MAX_MSG_COUNT = 32;

/// @brief A single message in the queue (fixed-size payload + size).
struct QueueMessage {
    uint8_t data[QUEUE_MAX_MSG_SIZE]; ///< Message payload.
    size_t size;                      ///< Actual payload size in bytes.
};

class Queue {
  public:
    static constexpr size_t MAX_WAITERS = CONFIG_SYNC_MAX_WAITERS;

    Queue()
        : head_(0), tail_(0), count_(0), send_waiters_(0), recv_waiters_(0),
          last_sender_(nullptr), last_receiver_(nullptr),
          last_sender_gen_(0), last_receiver_gen_(0), send_holder_prio_(0),
          recv_holder_prio_(0) {
    }
    /// @brief Initialize the message queue to empty.
    void init();
    /// @brief Initialize the message queue to empty (error-returning overload).
    /// @return SYNC_ERR_OK on success, SYNC_ERR_ALREADY_INITIALIZED if already
    /// initialized.
    errors::SyncError init_err();

    /// @brief Send a message, blocking if the queue is full.
    /// @return true on success.
    bool send(const uint8_t *data, size_t size);
    /// @brief Send a message, blocking if the queue is full (error-returning
    /// overload).
    /// @param[out] sent_bytes Number of bytes sent (if successful).
    /// @return SYNC_ERR_OK on success, SYNC_ERR_QUEUE_FULL if full,
    /// SYNC_ERR_MSG_TOO_LARGE if size > MAX, SYNC_ERR_MAX_WAITERS if waiter
    /// limit reached.
    errors::SyncError send_err(const uint8_t *data, size_t size,
                               size_t *sent_bytes);

    /// @brief Send a message without blocking.
    /// @return true if the message was enqueued.
    bool try_send(const uint8_t *data, size_t size);
    /// @brief Send a message without blocking (error-returning overload).
    /// @param[out] sent_bytes Number of bytes sent (if successful).
    /// @return SYNC_ERR_OK on success, SYNC_ERR_QUEUE_FULL if full,
    /// SYNC_ERR_MSG_TOO_LARGE if size > MAX.
    errors::SyncError try_send_err(const uint8_t *data, size_t size,
                                   size_t *sent_bytes);

    /// @brief Receive a message, blocking if the queue is empty.
    /// @param[out] buf Destination buffer.
    /// @param[out] size Receives the message size.
    /// @return true on success.
    bool receive(uint8_t *buf, size_t *size);
    /// @brief Receive a message, blocking if the queue is empty
    /// (error-returning overload).
    /// @param[out] buf Destination buffer.
    /// @param[out] size Receives the message size.
    /// @param[out] received_bytes Number of bytes received.
    /// @return SYNC_ERR_OK on success, SYNC_ERR_QUEUE_EMPTY if empty,
    /// SYNC_ERR_MAX_WAITERS if waiter limit reached.
    errors::SyncError receive_err(uint8_t *buf, size_t *size,
                                  size_t *received_bytes);

    /// @brief Receive a message without blocking.
    /// @return true if a message was dequeued.
    bool try_receive(uint8_t *buf, size_t *size);
    /// @brief Receive a message without blocking (error-returning overload).
    /// @param[out] buf Destination buffer.
    /// @param[out] size Receives the message size.
    /// @param[out] received_bytes Number of bytes received.
    /// @return SYNC_ERR_OK on success, SYNC_ERR_QUEUE_EMPTY if empty.
    errors::SyncError try_receive_err(uint8_t *buf, size_t *size,
                                      size_t *received_bytes);

    size_t available() const {
        return count_;
    }

    /// @brief Total number of tasks currently blocked on this queue (senders
    ///        + receivers).
    size_t waiter_count() {
        SpinLockGuard<SpinLock> guard(lock_);
        return send_waiters_count_ + recv_waiters_count_;
    }

    /// @brief Detach a task from this queue's send- or recv-waiter list
    ///        (lock-safe).  Used by TaskControlBlock::cleanup() to unlink a
    ///        reaped task before its TCB is freed.  Verifies pointer AND
    ///        generation match (guards against a recycled TCB occupying the
    ///        slot).
    /// @return true if the task was found and removed.
    bool remove_waiter(TaskControlBlock &task);

  private:
    SpinLock lock_;                          ///< Protects all queue state.
    QueueMessage msgs_[QUEUE_MAX_MSG_COUNT]; ///< Circular message buffer.
    size_t head_;                            ///< Dequeue index.
    size_t tail_;                            ///< Enqueue index.
    size_t count_; ///< Number of messages in the buffer.

    TaskControlBlock
        *send_waiters_[MAX_WAITERS]; ///< Tasks blocked on full queue.
    uint32_t send_waiter_gens_[MAX_WAITERS]; ///< Generations at insertion.
    size_t send_waiters_count_;      ///< Number of blocked senders.

    TaskControlBlock
        *recv_waiters_[MAX_WAITERS]; ///< Tasks blocked on empty queue.
    uint32_t recv_waiter_gens_[MAX_WAITERS]; ///< Generations at insertion.
    size_t recv_waiters_count_;      ///< Number of blocked receivers.

    bool is_full() const {
        return count_ >= QUEUE_MAX_MSG_COUNT;
    }
    bool is_empty() const {
        return count_ == 0;
    }

    bool add_send_waiter(TaskControlBlock &task);
    bool add_recv_waiter(TaskControlBlock &task);
    void wake_send_one();
    void wake_recv_one();
    void remove_send_waiter(TaskControlBlock &task);
    void remove_recv_waiter(TaskControlBlock &task);

    /// @brief Boost the last receiver when a high-prio sender blocks.
    void boost_receiver(TaskControlBlock &blocked_sender);
    /// @brief Boost the last sender when a high-prio receiver blocks.
    void boost_sender(TaskControlBlock &blocked_receiver);
    /// @brief Restore the last receiver's priority after send completes.
    void restore_receiver();
    /// @brief Restore the last sender's priority after receive completes.
    void restore_sender();

    TaskControlBlock *last_sender_;    ///< Last task to enter send() (for PIP).
    TaskControlBlock *last_receiver_;  ///< Last task to enter receive() (for PIP).
    uint64_t last_sender_gen_;   ///< Generation of last_sender_ at capture (H-3).
    uint64_t last_receiver_gen_; ///< Generation of last_receiver_ at capture (H-3).
    uint64_t send_holder_prio_;        ///< Saved priority of last_sender_ for PIP restore.
    uint64_t recv_holder_prio_;        ///< Saved priority of last_receiver_ for PIP restore.
};

} // namespace sync
} // namespace kernel
