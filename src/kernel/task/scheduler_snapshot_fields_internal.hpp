#pragma once

/// @file scheduler_snapshot_fields_internal.hpp
/// @brief Shared cross-TU declarations for scheduler_snapshot_fields.cpp
///        (issue #323).  Pure move support: no logic changes.
///        The field snapshot/restore pair (read_times, restore_task_fields)
///        needs no cross-TU shared home: both are Scheduler:: members
///        (declared in scheduler.hpp); engine helpers come from
///        scheduler_switch_internal.hpp and the EDF insert from
///        scheduler_queue_internal.hpp.  This header exists for
///        strand-pattern consistency and future shared decls.

#include <kernel/task/scheduler.hpp>
