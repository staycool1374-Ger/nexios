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

/// @brief Ready-queue op-ring tags (issue #582 A1, permanent CONFIG_DEBUG
///        instrumentation).  One tag per typed link/flag mutation so the
///        tick sweep can dump the interleaving that produced a desync.
///        Type-only in release (zero cost); the ring lives in debug builds.
enum class RunqTraceTag : uint64_t {
    LinkBegin = 0,         ///< try_link entry (flag observed set/clear)
    LinkCommit = 1,        ///< try_link linked (flag+links one unit)
    LinkRefuseInvalid = 2, ///< try_link refused: invalid TCB (untouched)
    LinkRefuseFlagged = 3, ///< try_link refused: already flagged (untouched)
    PopCommit = 4,          ///< pop_front returned head (flag cleared)
    PopRefuseEmpty = 5,    ///< pop_front on empty level (stale bit cleared)
    PopDropInvalidHead = 6, ///< pop_front dropped a poisoned head
    UnlinkSplice = 7,      ///< try_unlink spliced a contained node
    UnlinkNotInQueue = 8,  ///< try_unlink: node not in this queue
    UnlinkInvalid = 9,     ///< try_unlink refused: invalid TCB (untouched)
    ResetLevel = 10,       ///< reset_level: heads/tails/count nulled
    SetRawLevel = 11,      ///< set_raw restore (validated chain only)
    RqmEnqueueRefuse = 12, ///< RQM enqueue refused (already/invalid)
    RqmEnqueueCommit = 13, ///< RQM enqueue committed via try_link
    RqmPriorityCommit = 14, ///< RQM move_priority committed
    RqmRemoveHintMiss = 15, ///< RQM remove: hint missed, scanning
    RqmRemoveForcedClear = 16, ///< RQM remove: absent+flagged, force-cleared
    RqmRestoreLevel = 17,  ///< RQM restore: level chain validated
    RqmRestoreDrop = 18,   ///< RQM restore: level dropped (invalid chain)
    RqmClearAll = 19,      ///< RQM clear_all pass (drain via pop)
    SnapFieldWrite = 20,   ///< Snapshot TCB runq-membership field restore
    RebuildLevel = 21,     ///< rebuild_ready_queue level pass
    OutOfBandDetach = 22,  ///< detach_out_of_band (flag+links one unit)
    EdfInsert = 23,        ///< EDF list insert
    EdfRemove = 24,        ///< EDF list remove
    EdfResort = 25,        ///< EDF list re-sort
};

#if defined(CONFIG_DEBUG)
/// @brief Record one ready-queue mutation (issue #582 A1).  Per-CPU ring,
///        single-producer, relaxed index, no allocation.  ISR-safe.
/// @param tag What happened.
/// @param task_id TCB id (UINT64_MAX when no task).
/// @param flag_before Flag value before the mutation.
/// @param flag_after Flag value after the mutation.
/// @param next runq_next_ after the mutation (0 when null).
/// @param prev runq_prev_ after the mutation (0 when null).
/// @param rq_prio rq_priority_ after the mutation.
/// @param level Ready-queue level (UINT64_MAX when not a leveled op).
void sched_runq_trace(RunqTraceTag tag, uint64_t task_id,
                      uint64_t flag_before, uint64_t flag_after,
                      uint64_t next, uint64_t prev, uint64_t rq_prio,
                      uint64_t level) noexcept;
/// @brief Dump up to @p max_entries ring records for @p task_id
///        (UINT64_MAX = all tasks, newest last) to the debug console.
///        Called on the first orphan/desync detection in the tick sweep.
void sched_runq_trace_dump(uint64_t task_id,
                            uint64_t max_entries) noexcept;
/// @brief Dump up to @p max_entries of the H2 deferred-switch ring
///        (newest last).  FailClosed DEBUG tail (bounded to 4 entries).
void h2_dump_tail(uint64_t max_entries) noexcept;
#else
inline void sched_runq_trace(RunqTraceTag, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t,
                             uint64_t) noexcept {}
inline void sched_runq_trace_dump(uint64_t, uint64_t) noexcept {}
inline void h2_dump_tail(uint64_t) noexcept {}
#endif

}  // namespace kernel::sched_debug
