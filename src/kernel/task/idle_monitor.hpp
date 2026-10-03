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

#pragma once

/// @file idle_monitor.hpp
/// @brief Idle-task safety monitors, foundation slice (issues #43/#282).
///        Normative contract: docs/specs/idle_monitor.md §1 (M1-M6).
///        The monitor READS task state and SETS flags; it never kills,
///        reaps, reprioritizes, or rebudgets (M6 — violation is an S1
///        audit finding). Escalation flows only through existing
///        handler paths (spec §3 E1-E3).

#include <types.hpp>

namespace kernel {

struct TaskControlBlock;

/// @brief Resume cursor for chunked registry scans (M2).
struct IdleScanCursor {
    TaskControlBlock *pos = nullptr;
};

/// @brief Per-pass scan outcome (M5 test seam).
struct IdleScanProgress {
    uint64_t scanned = 0;
    uint64_t skipped = 0;
    bool wrapped = false;
};

/// @brief Verify stack canaries of up to @p budget TCBs from @p cursor.
/// @param cursor Resume position; advanced past the scanned chunk, wraps
///        to the registry head at the end (wrapped=true).
/// @param budget Maximum TCBs to inspect this pass (M2 chunk budget).
/// @return Progress counts. Invalid/teardown/idle TCBs are skipped
///         (M4), never dereferenced beyond the validity guards.
/// @note On a canary mismatch calls the existing
///       CONFIG_STACK_OVERFLOW_HOOK path (fail-stop in production, same
///       as the guard-page #PF path) — the scanner itself performs no
///       kill/reap/reprioritize (M6). Test suites override the weak
///       hook with a latch instead.
IdleScanProgress idle_scan_stack(IdleScanCursor &cursor,
                                 uint64_t budget) noexcept;

/// @brief Flag stall suspects among up to @p budget TCBs from @p cursor.
/// @param now_tick Current tick for the threshold comparison.
/// @return Progress counts. Sets stuck_suspected only — never arms a
///         watchdog the owning task did not itself arm (spec §5 P6 rule;
///         cross-task fresh arms are #41 follow-up scope).
IdleScanProgress idle_scan_stall(IdleScanCursor &cursor, uint64_t budget,
                                 uint64_t now_tick) noexcept;

/// @brief Publish the switch-path low-water sample as saturated bytes.
/// @param low_water_rsp Lowest kernel-stack RSP observed (kstack_low_water_
///        semantics: 0 = never switched out).
/// @param stack_top Kernel-stack top for the byte computation.
/// @param dst Destination snapshot field (stack_low_water_bytes).
void idle_publish_stack_low_water(uint64_t low_water_rsp, uint64_t stack_top,
                                  uint32_t &dst) noexcept;

/// @brief One bounded idle-monitor slice (M1-M3). Runs the enabled
///        scanners for CONFIG_IDLE_MONITOR_CHUNK TCBs total, then
///        returns. BSP-idle only; never blocks, allocates, or skips hlt.
void idle_monitor_slice() noexcept;

} // namespace kernel

#if CONFIG_STACK_OVERFLOW_HOOK
/// @brief Existing overflow hook (weak default in kernel.cpp: fail-stop
///        panic; strong test overrides latch). Global scope (defined
///        outside any namespace in kernel.cpp). Declared here so the
///        scanner TU can feed the existing escalation path (spec §3 E1).
void stack_overflow_hook(kernel::TaskControlBlock *task);
#endif
