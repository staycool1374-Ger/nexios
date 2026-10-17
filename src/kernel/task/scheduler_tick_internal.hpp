#pragma once

/// @file scheduler_tick_internal.hpp
/// @brief Shared cross-TU declarations for scheduler_tick.cpp (issue #323).
///        Pure move support: no logic changes.
///        The timer-tick dispatch heart (on_tick, rate_monotonic_schedule,
///        reschedule) needs no cross-TU shared home: all moved functions are
///        Scheduler:: members (declared in scheduler.hpp); engine helpers
///        come from the queue/switch/debug/lifecycle internal headers.
///        This header exists for strand-pattern consistency and future
///        shared decls.

#include <kernel/task/scheduler.hpp>
