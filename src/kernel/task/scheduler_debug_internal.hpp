#pragma once

/// @file scheduler_debug_internal.hpp
/// @brief Shared cross-TU declarations for scheduler_debug.cpp (issue
///        #323).  Pure move: no logic changes.
///        Exposes the per-CPU interrupted-PC slot that
///        rate_monotonic_schedule() (scheduler.cpp) reads for the debugger
///        park check.

#include <kernel/task/scheduler.hpp>

namespace kernel::sched_debug {

/// @brief Issue #225: per-CPU interrupted-PC slot for the debugger park
///        check.  Own CPU writes (timer ISR), own CPU tick reads; plain
///        relaxed atomics.
extern uint64_t debug_tick_pc_[CONFIG_MAX_CPUS];

}  // namespace kernel::sched_debug
