#pragma once

/// @file scheduler_lifecycle_internal.hpp
/// @brief Shared cross-TU declarations for scheduler_lifecycle.cpp
///        (issue #323).  Pure move: no logic change.
///        P5a deferred-kill worklist statics; the list is drained by the
///        on_tick tail in scheduler.cpp, which observes only the count.

#include <kernel/task/scheduler.hpp>

namespace kernel::sched_lifecycle {

// P5a: Deferred-kill list bound (design §4.C VAR-08).
inline constexpr uint64_t MAX_DEFERRED_KILLS = 16;

extern TaskControlBlock *s_deferred_kill_tasks[MAX_DEFERRED_KILLS];
extern uint64_t s_deferred_kill_count;

}  // namespace kernel::sched_lifecycle
