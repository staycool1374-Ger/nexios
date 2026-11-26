#pragma once

/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/// @file tick_watchdog.hpp
/// @brief TSC-domain tick watchdog (M2, issue #546 §6.2 item 6).
/// Fires when the raw counter advances past grace while the timer tick count is
/// frozen and the CPU is out of idle. Domain honesty (#546-C): on x86_64 the
/// TSC is independent of the APIC/PIT tick source; on aarch64/riscv64 the
/// sampled counter is the same system counter the tick comparator uses, so
/// there it supervises tick *flow*, not an independent clock. Fire policy is
/// log/latch only (CONFIG_TICK_WATCHDOG_ACTION, default 0); a panic arm exists
/// for the Q12-A reboot hook but is not wired by default. No allocation, no
/// locks, no scheduler services on any path.

#include <types.hpp>

namespace kernel {
namespace tick_watchdog {

/// @brief Initialise from raw_counter_freq_hz (called once from
///        Scheduler::init).  Disabled when either frequency is 0.
void init() noexcept;

/// @brief Record a live tick (called at the end of the BSP on_tick body).
void note_tick() noexcept;

/// @brief Check for a stalled tick from task context.  No-op when disabled,
///        before the first tick, or while the test harness owns the clock.
/// @param where Static tag naming the call site (serial/dmesg evidence).
void checkpoint(const char *where) noexcept;

/// @brief Idle-loop checkpoint: resync only, never fires (idle-current
///        implies no schedulable work is stranded by a dead tick).
/// @param where Static tag naming the call site.
void idle_checkpoint(const char *where) noexcept;

/// @brief True once the watchdog has fired (latched, one-shot).
bool fired() noexcept;

/// @brief Number of fire events (0 or 1 — one-shot latch).
uint64_t fire_count() noexcept;

/// @brief Raw TSC value at the last fire (0 if never fired).
uint64_t last_evidence() noexcept;

/// @brief Snapshot readout (schedoracles shape, issue #546-C). Single
///        struct, read-only, no locks.
struct TickWatchdogRecord {
    uint64_t grace_cycles = 0;
    uint64_t grace_source = 0; ///< 0 = floor default, 1 = measured auto-derive
    bool enabled = false;
    bool fired = false;
    uint64_t fire_count = 0;
    uint64_t overrun_cycles = 0;
    const char *where_tag = nullptr; ///< fire site, null if never fired
    uint64_t per_cpu_last_stale = 0; ///< max (ticks - snapshot) in ticks
};

/// @brief Read the current watchdog record (see TickWatchdogRecord).
TickWatchdogRecord read_tick_watchdog() noexcept;

/// @brief Test-isolation reset (clears snapshots, latch, counters).
void reset_for_test() noexcept;

/// @brief Pretend a tick happened (test seam for set_ticks_for_test:
///        callers that rewind the tick count must call this so a
///        deliberate rewind is not misread as a stall).
void note_tick_override() noexcept;

} // namespace tick_watchdog
} // namespace kernel
