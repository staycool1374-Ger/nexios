/// @file kernel/memory/mem_pressure.hpp
/// @brief R-MEM unstable-state warning latch (M4, #575 kernel half).
///
/// Memory exhaustion is unrecoverable: the manager MUST emit an explicit
/// unstable-state warning (diagnostic + supervisor notify) instead of
/// failing silently. This TU is the emit half: a zero-alloc, lock-free,
/// one-shot latch callable from inside pmm_lock_/pool_lock_/OOM-handler
/// windows. The supervisor Notify sink is wired at M5
/// (drain_pending() is a no-op until then).
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
};

/// @brief Latch the unstable-state warning. First caller wins and emits
///        exactly one dmesg row (kDmesgBase_PMM + pressure code);
///        latecomers only bump count(). No locks, no allocation, no
///        Logger — safe from any lock context. Never panics.
void latch(MemPressureReason reason) noexcept;
/// @brief Re-arm after supervisor ack (test hook uses the same path).
void clear() noexcept;
/// @brief True once latched (until clear()).
bool latched() noexcept;
/// @brief Latching reason (NONE when clear).
MemPressureReason reason() noexcept;
/// @brief Total latch calls since the last clear (diagnostic: sustained
///        pressure reads > 1 while exactly one row is emitted).
uint64_t count() noexcept;
/// @brief True when a supervisor Notify is owed (set by latch, cleared
///        by drain_pending()).
bool pending_notify() noexcept;
/// @brief M5 sink: hand the pending notify to the supervisor. No-op
///        while no sink is installed (clears the flag regardless so the
///        latch never wedges on an absent consumer).
void drain_pending() noexcept;
/// @brief Configured pressure threshold percent (policy input for the
///        M5 supervisor reaction; the latch itself fires on exhaustion
///        events, which are definitionally past-threshold).
uint8_t threshold_pct() noexcept;

} // namespace mem_pressure
} // namespace kernel
