/// @file task/liveness_ledger.hpp
/// @brief Positive liveness guarantee bookkeeping (issue #546-D).
///        Per-task ready/wait ages (derived state, window-based) + monotone
///        escalation counters. No TCB growth (per-CPU side table indexed by
///        task-id hash); BSS only; atomics only; no locks, no allocation,
///        no scheduler services, no serial output on any path.
#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace kernel {
namespace liveness {

/// @brief Slots per CPU: ID_TABLE_SIZE id-hashed slots (task.hpp).
///        128 slots x 8 CPUs x 16 B = 16 KB static BSS.
struct LivenessSlot {
    uint64_t owner_id = 0;
    uint16_t ready_age = 0;
    uint16_t wait_ticks = 0;
    uint32_t reserved = 0;
};

/// @brief Readout for the liveness family (schedoracles shape).
struct LivenessReadout {
    uint64_t wait_ceiling_count = 0;
    uint64_t relief_fires = 0;
    uint64_t relief_deferred = 0;
    uint64_t breaker_fires = 0;
    uint64_t max_ready_age = 0;
    uint64_t wait_windows_max = 0;
    uint64_t holder_blocked_observed = 0;
};

/// @brief Consecutive-BLOCKED-tick ceiling before a wait-ceiling hit.
constexpr uint64_t kWaitCeilingTicks = 1000;

/// @brief Record one tick of observation for a task (called from the
///        on_tick all-tasks walk, scheduler_lock_ held, IF == 0).
///        Advances ready_age for denied READY tasks, wait_ticks for
///        covered BLOCKED tasks; zeroes excluded states. Never dispatches.
/// @param wait_covered True iff this BLOCKED task is in the D.5 closed
///        covered set (peer/channel-released wait). The caller decides
///        coverage from sleep/admin/teardown flags; the ledger counts.
void note_task_tick(uint64_t task_id, uint8_t state, bool in_ready_queue,
                    bool admin_parked, bool sleep_armed, bool wait_covered) noexcept;

/// @brief Record a forced relief dispatch (mutually exclusive with the
///        #598 victim pick — shared 1-in-20 budget).
void note_relief_fire() noexcept;
/// @brief Record a plain (non-EDF-displacing) breaker dispatch — the
///        other half of the shared budget.
void note_breaker_fire() noexcept;

/// @brief Record a deferred relief tick (status quo EDF served instead).
void note_relief_deferred() noexcept;

/// @brief Record a wait-ceiling hit for a task (count + one-shot WARN).
/// @return true if this hit crossed the ceiling (first crossing only).
bool note_wait_ceiling(uint64_t task_id) noexcept;

/// @brief Read the current readout (test/diagnostic context only).
LivenessReadout read_liveness() noexcept;

/// @brief Test seam: deterministic ceiling for wait_ceiling tests.
///        Test-isolation only; reset by reset_liveness_ledger().
void set_wait_ceiling_for_test(uint32_t ticks) noexcept;

/// @brief Snapshot/quiesce reset for the DERIVED family (ages, waits,
///        windows). Wired at init + restore_state. Never touches the
///        monotone family.
void reset_liveness_ledger() noexcept;

/// @brief Test seam: reset the MONOTONE counters (called only where
///        reset_sched_oracles_for_test() is called today).
void reset_liveness_counters_for_test() noexcept;

} // namespace liveness
} // namespace kernel
