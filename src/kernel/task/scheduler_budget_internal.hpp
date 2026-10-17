#pragma once

/// @file scheduler_budget_internal.hpp
/// @brief Shared cross-TU declarations for scheduler_budget.cpp
///        (issue #323).  Pure move: no logic change.
///        The memory-budget surface owns no shared cross-TU state:
///        memory_budget_pages_ stays a Scheduler class static (declared in
///        scheduler.hpp, defined once in scheduler_budget.cpp).  This header
///        exists only to keep the per-strand convention uniform (one internal
///        header per extracted TU).

#include <kernel/task/scheduler.hpp>

namespace kernel::sched_budget {

// No shared cross-TU state.  memory_budget_pages_ stays a Scheduler class
// static; all six budget functions are declared in scheduler.hpp and defined
// once in scheduler_budget.cpp.

}  // namespace kernel::sched_budget
