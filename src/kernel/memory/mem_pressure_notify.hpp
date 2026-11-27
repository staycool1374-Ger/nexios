/// @file kernel/memory/mem_pressure_notify.hpp
/// @brief R-MEM supervisor observation seam (issue #575, Increment 2).
///
/// Single-supervisor slot: a task registers itself via WATCH, the tick
/// tail pokes it with a pulse when a warning is pending, and RECV copies
/// the 32-B record out. Fail-closed (taken slot, dead supervisor) and
/// allocation-free on every path.
#pragma once

#include <types.hpp>
#include <kernel/memory/mem_pressure.hpp>

namespace kernel {

struct TaskControlBlock;

namespace mem_pressure {

// Pulse encoding: "MEMP" prefix with the reason in the low byte (reasons
// 0..7 fit; 6 = MEMPOOL_EMPTY, 7 = PMM_PRESSURE).
constexpr uint64_t kMemPressurePulseBase = 0x4D454D5000000000ULL;
constexpr uint64_t pulse_for(MemPressureReason reason) noexcept {
    return kMemPressurePulseBase | static_cast<uint64_t>(reason);
}

/// @brief 32-B snapshot record copied to the supervisor via RECV.
struct PressureRecord {
    uint64_t reason = 0;
    uint64_t count = 0;
    uint64_t latch_tick = 0;
    uint64_t observed_tick = 0;
};

/// @brief Self-register the caller as pressure supervisor. Idempotent
/// for the current occupant; refuses while a different live task holds
/// the slot; takes over when the occupant is dead. Never blocks.
bool register_supervisor(TaskControlBlock &caller) noexcept;
/// @brief Tick-tail service: capture the post-freeze baseline, evaluate
/// the waterline, and poke a registered supervisor when pending.
/// No locks, no allocation. Callable from the on_tick lock-free tail.
void service_pending() noexcept;
/// @brief Current record snapshot (latch state, never the dmesg ring).
PressureRecord record() noexcept;
/// @brief Tick of the last successful supervisor poke (0 = none yet).
uint64_t observed_tick() noexcept;

} // namespace mem_pressure
} // namespace kernel
