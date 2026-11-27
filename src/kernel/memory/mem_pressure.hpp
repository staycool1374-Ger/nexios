/// @file kernel/memory/mem_pressure.hpp
/// @brief R-MEM unstable-state warning latch (#575 kernel half).
///
/// Memory exhaustion is unrecoverable: the manager MUST emit an explicit
/// unstable-state warning (diagnostic + supervisor notify) instead of
/// failing silently. This TU is the emit half: a zero-alloc, lock-free,
/// one-shot latch callable from inside pmm_lock_/pool_lock_/OOM-handler
/// windows. The static latch state is the authoritative record; the
/// dmesg row is a best-effort diagnostic copy. Production never clears
/// the latch (reboot is the only recovery, Q12-A); clear()/
/// snapshot_reset() are test-isolation seams only. The supervisor
/// Notify sink is wired by service_pending() (Increment 2).
#pragma once

#include <types.hpp>

namespace kernel {
namespace mem_pressure {

/// @brief Why the pressure latch fired. NONE = never latched.
enum class MemPressureReason : uint8_t {
    NONE = 0,
    PMM_FROZEN_DENY = 1, ///< post-freeze allocation denied (STATIC_POOLS_ONLY)
    PMM_OOM = 2,         ///< OOM-handler retry exhausted
    BUDGET_DENY = 3,     ///< per-task budget exceeded
    BUFPOOL_EMPTY = 4,    ///< BufferPool cache exhausted, no PMM fallback
    HANDOVER_CAPPED = 5, ///< handover run table full, surplus retained
    MEMPOOL_EMPTY = 6,   ///< MemPool class exhausted (free_count == 0)
    PMM_PRESSURE = 7,    ///< post-freeze waterline trip (service_pending)
};

/// @brief Latch the unstable-state warning. First caller wins and emits
///        exactly one dmesg row (kDmesgBase_PMM + pressure code);
///        latecomers only bump count(). No locks, no allocation, no
///        Logger — safe from any lock context. Never panics.
void latch(MemPressureReason reason) noexcept;
/// @brief Latch and report whether this call's ordinal is notable
///        (multiple of 64) for row budgeting under sustained pressure.
bool latch_notable(MemPressureReason reason) noexcept;
/// @brief Take the pending supervisor Notify flag (atomic exchange).
///        True iff a Notify was owed and is now owned by the caller.
bool take_pending() noexcept;
/// @brief Latch tick of the winning call (arch::Timer::ticks()).
uint64_t latch_tick() noexcept;
/// @brief Re-arm after supervisor ack (test hook uses the same path).
void clear() noexcept;
/// @brief Test-isolation reset: zeroes latched/reason/pending/ticks/count.
void snapshot_reset() noexcept;
/// @brief True once latched (until clear()).
bool latched() noexcept;
/// @brief Latching reason (NONE when clear).
MemPressureReason reason() noexcept;
/// @brief Total latch calls since the last clear (diagnostic: sustained
///        pressure reads > 1 while exactly one row is emitted).
uint64_t count() noexcept;
/// @brief True when a supervisor Notify is owed (set by latch, taken
///        by take_pending()).
bool pending_notify() noexcept;
/// @brief Configured pressure threshold percent (policy input for the
///        waterline trip; default 90 — see CONFIG_MEM_PRESSURE_THRESHOLD_PCT).
uint8_t threshold_pct() noexcept;

} // namespace mem_pressure
} // namespace kernel
