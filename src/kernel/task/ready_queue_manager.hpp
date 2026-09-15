/// @file ready_queue_manager.hpp
/// @brief O(1) priority-ordered ready queue with bitmap-based scheduling.

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>
#include <kernel/task/priority_map.hpp>
#include <kernel/task/task_queue.hpp>
#include <kernel/task/task_fwd.hpp>

namespace kernel {

/// @brief POD snapshot of the full ReadyQueueManager state.
///        All fields are plain data; memcpy-safe by construction.
struct alignas(64) ReadyQueuePOD {
    uint64_t bitmap_hi; ///< Snapshot of priority-map upper 64 bits.
    uint64_t bitmap_lo; ///< Snapshot of priority-map lower 64 bits.
    uint64_t queue_heads[CONFIG_PRIORITY_CEILING +
                         1]; ///< Per-priority queue head pointers.
    uint64_t queue_tails[CONFIG_PRIORITY_CEILING +
                         1]; ///< Per-priority queue tail pointers.
    uint64_t queue_counts[CONFIG_PRIORITY_CEILING +
                          1]; ///< Per-priority queue entry counts.
};

/// @brief O(1) priority-ordered ready queue.
/// Uses a bitmap (PriorityMap) to track non-empty priority levels and a
/// per-priority TaskQueue array. Enqueue/dequeue by priority are O(1).
class ReadyQueueManager {
    PriorityMap bitmap_; ///< Bitmap tracking non-empty priority levels.
    TaskQueue queues_[CONFIG_PRIORITY_CEILING +
                      1]; ///< One FIFO queue per priority level.

  public:
    /// @brief Enqueues a TCB at the given priority level.
    void enqueue(TaskControlBlock &tcb, uint64_t priority) noexcept;
    /// @brief Dequeues and returns the highest-priority ready TCB.
    TaskControlBlock *dequeue_highest() noexcept;
    /// @brief Returns the highest-priority ready TCB without removing it.
    TaskControlBlock *peek_highest() noexcept;
    /// @brief Removes a specific TCB from its priority queue.
    void remove(TaskControlBlock &tcb, uint64_t priority) noexcept;
    /// @brief Moves a TCB from one priority queue to another.
    void move_priority(TaskControlBlock &tcb, uint64_t old_prio,
                       uint64_t new_prio) noexcept;

    /// @brief Returns true if any priority level has ready tasks.
    bool has_ready() const noexcept {
        return !bitmap_.empty();
    }
    /// @brief Total queued TCBs across all priority levels (issue #61).
    ///        O(priorities), non-mutating; caller must hold
    ///        scheduler_lock_ or have interrupts disabled.
    uint64_t depth() const noexcept;
    /// @brief Returns the highest priority level with non-empty queue.
    uint64_t highest_ready_priority() const noexcept {
        return bitmap_.get_highest_priority();
    }
    /// @brief Returns a const reference to the underlying bitmap.
    const PriorityMap &bitmap() const noexcept {
        return bitmap_;
    }
    /// @brief Returns a const reference to the queue at a given priority.
    const TaskQueue &queue(uint64_t prio) const noexcept {
        return queues_[prio];
    }

    /// @brief Captures full state into a POD for snapshot/restore.
    void capture_pod(ReadyQueuePOD &out) const noexcept;
    /// @brief Restores full state from a previously captured POD.
    void restore_pod(const ReadyQueuePOD &src) noexcept;
    /// @brief Empties all per-priority queues (draining each via pop_front).
    void clear_all() noexcept;
    /// @brief Resets all queues and bitmap to empty state.
    void reset() noexcept;
};

} // namespace kernel
