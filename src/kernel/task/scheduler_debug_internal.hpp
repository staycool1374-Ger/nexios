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

/// @brief TEMP DEBUG (BUGS.md#020): detect a SporadicServer pointer that
///        aliases a freed/poisoned MemPool block.  Single definition lives
///        in scheduler_debug.cpp; scheduler.cpp observes it via using-decl.
bool is_poisoned_block(const void *p) noexcept;

#if defined(CONFIG_DEBUG)
// Issue #299/#280 stall diagnostics: count timer-tick dispatcher entries
// and actual switch_to_task calls (owned by the diagnostics tail in
// scheduler_debug.cpp; incremented from on_tick/switch paths).
extern uint64_t g_diag_rms_count;
extern uint64_t g_diag_switch_count;
extern uint64_t g_diag_tick_count;
#endif

}  // namespace kernel::sched_debug
