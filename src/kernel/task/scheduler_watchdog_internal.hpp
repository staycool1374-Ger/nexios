#pragma once

/// @file scheduler_watchdog_internal.hpp
/// @brief Shared cross-TU declarations for scheduler_watchdog.cpp (issue
///        #323).  Pure move support: no logic changes.
///        The watchdog/deadline engine (scan_deadlines, monitor_task_entry,
///        ensure_monitor, deadline_miss_handler, scan_watchdogs_locked,
///        fire_stall_watchdog, watchdog_expiry_handler, wcet_overrun_handler)
///        needs no cross-TU shared home: all callees are Scheduler:: members
///        (declared in scheduler.hpp), hpp-inline helpers, or TU-local
///        file-statics.  The three weak default handlers keep their single
///        definitions in scheduler_watchdog.cpp (strong test overrides, if
///        any, win at link).  This header exists for strand-pattern
///        consistency and future shared decls.

#include <kernel/task/scheduler.hpp>
