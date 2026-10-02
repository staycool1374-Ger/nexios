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

/// @file watchdog.md
/// @brief Per-task software watchdog, kernel foundation (issue #41).
///
/// Owner scope for this increment: kernel foundation ONLY — the
/// userspace watchdog daemon (first externalized microservice) is a
/// follow-up issue. What lands here: two syscalls, TCB-embedded state,
/// an on_tick expiry scan, a /proc node, and a ring record.

/// ## 1. Syscalls
///
/// | Number | Name | Args | Returns |
/// |---|---|---|---|
/// | 91 | WATCHDOG_CREATE | arg0 = period_ticks (> 0), arg1 = 0 reserved | 0, -SCHED_NO_CURRENT, -SCHED_INVALID_ARGS |
/// | 92 | WATCHDOG_KICK | arg0 = arg1 = 0 reserved | 0, -SCHED_NO_CURRENT, -SCHED_INVALID_ARGS, -SCHED_INVALID_STATE (disarmed) |
///
/// Both are self-only (the caller's own TCB; no pid argument, no new
/// CapType in this increment) and stay out of k_syscall_fast[] (issue
/// #92 discipline: mutating path, full canary). Expiry saturates instead
/// of wrapping (a wrapped expiry would fire immediately — fail-open).

/// ## 2. TCB semantics
///
/// Fields on TaskControlBlock (task.hpp): wdog_armed, wdog_period_ticks,
/// wdog_last_kick_tick, wdog_expiry_tick, wdog_gen. All five are
/// snapshot-captured (TaskFields) so an arm never outlives the test that
/// made it, and explicitly initialized at all four memset sites
/// (create/create_user/clone/elf) plus disarm-on-teardown with a
/// generation bump (ABA guard on TCB slot reuse: gen 0 = never armed).
/// Expiry is one-shot: the scan disarms before dispatching, re-CREATE
/// re-arms with a fresh generation.

/// ## 3. CONFIG_WATCHDOG_ACTION table
///
/// Mirrors CONFIG_DEADLINE_ACTION 0..4 (deadline.md §3): 0 LOG_ONLY,
/// 1 PANIC, 2 DEMOTE (priority >>= 1), 3 KILL via defer_kill (default,
/// fail-closed), 4 NOTIFY_MONITOR (SIGUSR1, same monitor-PID resolution
/// as the deadline path). Range enforced by static_assert plus the
/// admission self-test. Every expiry enters the ring (TIMING+6 ERROR,
/// "Timing: watchdog expired") whatever the action.

/// ## 4. /proc format
///
/// `/proc/<pid>/watchdog` (and `/proc/self/watchdog`) reads:
/// `armed=<0|1> period=<ticks> last_kick=<tick> expires=<tick>\n`.
/// Write returns VFS_INVALID — SYS_WATCHDOG_KICK is the single kick
/// funnel. The node is embedded in PidDirVnode (no extra vnode counter,
/// stat precedent).

/// ## 5. Timer-wheel rejection rationale
///
/// The wheel was evaluated and rejected: shared 64-slots/CPU can exhaust,
/// tick-context callbacks cannot reschedule (KILL needs deferral anyway),
/// and re-arm churn duplicates TCB state. The on_tick inline scan is a
/// single bounded pass with disarmed-skip early-out under the already-held
/// scheduler_lock_ — same window as the pager watchdog_scan.

/// ## 6. #45 handoff hook
///
/// The #45 boot gate (CONFIG_BOOT_BUDGET_MS path) keeps its boot-only
/// disposition until CREATE lands, then swaps the arm to
/// SYS_WATCHDOG_CREATE with the budget as the period. This increment
/// only reserves that call-site signature; the swap itself is a
/// follow-up edit on the #45 thread.
