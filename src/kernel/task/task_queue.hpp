/// @file task_queue.hpp
/// @brief Intrusive doubly-linked list queue for TaskControlBlock scheduling.

#pragma once

#include <types.hpp>
#include <kernel/task/task_fwd.hpp>

namespace kernel {

/// @brief Intrusive doubly-linked list queue for TaskControlBlock.
/// Operations are O(1), no dynamic allocation. TCBs embed next/prev pointers.
/// Issue #582 INV-RQ-1: in_ready_queue_, runq_next_/runq_prev_ (and the
/// RQM-level rq_priority_) are written ONLY inside TaskQueue, each mutation
/// being one arch::IrqGuard-atomic unit, so no ISR can observe a half-linked
/// node.  Every public mutation body is a single IrqGuard unit; refused
/// operations leave the TCB untouched.
class TaskQueue {
  public:
    TaskQueue() : head_(nullptr), tail_(nullptr), count_(0) {
    }

    /// @brief Appends a TCB to the tail of the queue.
    ///        Compatibility shim over try_link (test-only callers): refusal
    ///        is silent here — production code must use try_link and handle
    ///        the result (INV-RQ-2: every enqueue refusal is typed).
    void push_back(TaskControlBlock &tcb) noexcept;
    /// @brief Links @p tcb at the tail when valid and not already flagged.
    ///        Single IrqGuard unit (flag+links+rq_priority_ unobservable
    ///        mid-mutation, issue #582 INV-RQ-1/§5.3).
    /// @param level Ready-queue level (stored to rq_priority_ on commit).
    /// @return False when the TCB is invalid, already flagged, or the level
    ///         is out of range — the TCB is left byte-identical (INV-RQ-2).
    bool try_link(TaskControlBlock &tcb, uint64_t level) noexcept;
    /// @brief Removes and returns the head of the queue, or nullptr if empty.
    TaskControlBlock *pop_front() noexcept;
    /// @brief Removes a specific TCB from anywhere in the queue.
    ///        Compatibility shim over try_unlink (test-only callers).
    void remove(TaskControlBlock &tcb) noexcept;
    /// @brief Splices @p tcb out of the level that contains it.
    ///        Single IrqGuard unit (splice+flag-clear unobservable).
    /// @return True when the node was contained and spliced; false (TCB
    ///         untouched) when invalid or not in this queue.
    bool try_unlink(TaskControlBlock &tcb) noexcept;
    /// @brief Forced membership detach for the documented no-walk sites
    ///        (issue #582 §5.4 row 17): splices when contained, then clears
    ///        flag+links+rq_priority_ as one IrqGuard unit.  Used where the
    ///        caller provably owns the TCB and no queue walk is wanted.
    void detach_out_of_band(TaskControlBlock &tcb) noexcept;

    /// @brief Returns true if the queue contains no entries.
    bool empty() const noexcept {
        return count_ == 0;
    }
    /// @brief Returns the number of entries in the queue.
    uint64_t count() const noexcept {
        return count_;
    }

    /// @brief Returns the head of the queue (for iteration), or nullptr.
    TaskControlBlock *head() const noexcept {
        return head_;
    }

    /// @brief Returns true if the given TCB is physically linked in this
    ///        queue (walks the intrusive list).  Used by diagnostics to detect
    ///        in_ready_queue_ flag vs physical-link desync.
    bool contains(const TaskControlBlock &tcb) const noexcept;
    /// @brief Returns the tail of the queue (for append), or nullptr.
    TaskControlBlock *tail() const noexcept {
        return tail_;
    }

    /// @brief Resets queue to empty state without following pointers.
    ///        Safe to call even when pointers may be dangling.
    void reset_level() noexcept;

    /// @brief Validated restore from POD data (issue #582 §5.2): walks the
    ///        whole [head, tail] chain (bounded by @p count), checking every
    ///        node is_valid with consistent prev-links and exact length.
    /// @return True when the chain validated (installed); false when any
    ///         check failed (queue untouched — the caller drops the level).
    bool restore_level(TaskControlBlock *h, TaskControlBlock *t,
                       uint64_t c) noexcept;

  private:
    TaskControlBlock *head_; ///< Pointer to the first TCB in the queue.
    TaskControlBlock *tail_; ///< Pointer to the last TCB in the queue.
    uint64_t count_;         ///< Number of TCBs currently enqueued.
};

} // namespace kernel
