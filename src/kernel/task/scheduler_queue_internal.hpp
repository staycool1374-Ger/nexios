#pragma once

/// @file scheduler_queue_internal.hpp
/// @brief Shared cross-TU declarations for scheduler_queue.cpp (issue #323).
///        Pure move: no logic changes.
///        Declares the EDF list + membership helpers and the pending-switch
///        invalidator used both by scheduler_queue.cpp and by code that
///        remains in scheduler.cpp.

#include <kernel/task/scheduler.hpp>

namespace kernel::sched_queue {

struct EdfReadyList {
    TaskControlBlock *head_ = nullptr;
};

extern EdfReadyList edf_ready_[CONFIG_MAX_CPUS];

bool edf_eligible(const TaskControlBlock &t) noexcept;
bool edf_earlier(const TaskControlBlock &a,
                 const TaskControlBlock &b) noexcept;
void edf_insert(TaskControlBlock &t, uint64_t cpu) noexcept;
void edf_remove(TaskControlBlock &t, uint64_t cpu) noexcept;
void edf_resort(TaskControlBlock &t) noexcept;
void migrate_dispatch_queue(TaskControlBlock &t) noexcept;
TaskControlBlock *edf_peek_valid_own() noexcept;
TaskControlBlock *edf_preempt_candidate() noexcept;

/// @brief Invalidate any pending deferred-switch arm targeting task_id.
void invalidate_pending_switch_to(uint64_t task_id) noexcept;

}  // namespace kernel::sched_queue
