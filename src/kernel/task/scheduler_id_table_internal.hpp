#pragma once

/// @file scheduler_id_table_internal.hpp
/// @brief Shared cross-TU declarations for scheduler_id_table.cpp
///        (issue #323).  Pure move: no logic change.
///        The task-ID hash table owns no shared cross-TU state:
///        ID_TOMBSTONE/id_table_/next_task_id_ stay Scheduler class
///        statics (definitions remain in scheduler.cpp).  This header
///        exists only to keep the per-strand convention uniform (one
///        internal header per extracted TU).

#include <kernel/task/scheduler.hpp>

namespace kernel::sched_ids {

// No shared cross-TU state.  All id-table entry points are Scheduler
// members declared in scheduler.hpp and defined once in
// scheduler_id_table.cpp.

}  // namespace kernel::sched_ids
