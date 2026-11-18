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
/// Issue #582 INV-RQ-1/2: all flag/link writes go through TaskQueue's
/// IRQ-atomic typed API; only RQM::enqueue sets rq_priority_/flag, and
/// only inside a successful try_link.  Refusals are typed
/// (RunqEnqueueResult) and leave the TCB byte-identical.
class ReadyQueueManager {
    PriorityMap bitmap_; ///< Bitmap tracking non-empty priority levels.
    TaskQueue queues_[CONFIG_PRIORITY_CEILING +
                      1]; ///< One FIFO queue per priority level.

  public:
    /// @brief Typed enqueue outcome (issue #582 INV-RQ-2).
    enum class RunqEnqueueResult : uint8_t {
        Linked = 0,        ///< Committed: flag+links+prio set via try_link.
        AlreadyQueued = 1, ///< Refused: flag already set (untouched).
        RefusedInvalid = 2, ///< Refused: invalid TCB (byte-identical).
    };
    /// @brief Enqueues a TCB at the given priority level.
    /// @return Linked on commit; any refusal leaves the TCB untouched and
    ///         is reported (no silent drops — the caller counts + warns).
    RunqEnqueueResult enqueue(TaskControlBlock &tcb,
                              uint64_t priority) noexcept;
    /// @brief Dequeues and returns the highest-priority ready TCB.
    TaskControlBlock *dequeue_highest() noexcept;
    /// @brief Dequeues and returns the head TCB at one priority level
    ///        (issue #242 starvation-breaker: forced dispatch below the top
    ///        level).  Mirrors dequeue_highest() flag discipline; O(1).
    /// @return Head TCB, or nullptr when the level holds nothing (stale bit
    ///         cleared so a phantom level cannot spin the caller).
    TaskControlBlock *dequeue_level(uint64_t prio) noexcept;
    /// @brief Returns the highest-priority ready TCB without removing it.
    TaskControlBlock *peek_highest() noexcept;
    /// @brief Removes a specific TCB from its priority queue.
    void remove(TaskControlBlock &tcb, uint64_t priority) noexcept;
    /// @brief Level-agnostic typed detach (issue #582 §5.4): splices from
    ///        the hint level when contained, then clears flag+links+prio
    ///        as one unit.  For sites that proved absence (sweep) or own
    ///        a fresh TCB (register/add) and want no level walk.
    void detach(TaskControlBlock &tcb) noexcept;
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
    ///        Every level chain is validated via TaskQueue::restore_level;
    ///        invalid levels are dropped (bitmap cleared, node membership
    ///        force-cleared own-fields-only) and counted in the return.
    /// @return Number of dropped levels (0 = all levels validated).  The
    ///         caller rebuilds the failed levels from all_tasks_ (§5.5).
    uint64_t restore_pod(const ReadyQueuePOD &src) noexcept;
    /// @brief Empties all per-priority queues (draining each via pop_front).
    void clear_all() noexcept;
    /// @brief Resets all queues and bitmap to empty state.
    void reset() noexcept;

  private:
    /// @brief Pop the head of level @p prio and detach its membership flags
    ///        (issue #255 H8: the pop + empty-clear + flag-detach tail was
    ///        duplicated in dequeue_highest/dequeue_level).  Null-safe on
    ///        empty levels (stale bit cleared).  WCET: O(1) flat.
    TaskControlBlock *pop_and_detach(uint64_t prio) noexcept;
};

} // namespace kernel
