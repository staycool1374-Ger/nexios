#pragma once

/// @file scheduler_snapshot_internal.hpp
/// @brief Shared cross-TU declarations for scheduler_snapshot.cpp
///        (issue #323).  Pure move: no logic change.
///        The test-isolation capture/restore surface owns no shared
///        cross-TU state: all helpers are Scheduler members declared in
///        scheduler.hpp and defined once in scheduler_snapshot.cpp
///        (`edf_ready_` resolves via scheduler_queue_internal.hpp).
///        This header exists only to keep the per-strand convention
///        uniform (one internal header per extracted TU).

#include <kernel/task/scheduler.hpp>

namespace kernel::sched_snapshot {

// No shared cross-TU state.  All snapshot/restore entry points are
// Scheduler members declared in scheduler.hpp and defined once in
// scheduler_snapshot.cpp.

}  // namespace kernel::sched_snapshot
