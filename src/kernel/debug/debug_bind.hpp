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

/// @file debug_bind.hpp
/// @brief Debugger-handle binding helpers (issue #226). The binding table
///        itself stays owned by syscall_handlers_debug.cpp (issue #225);
///        this header exposes the narrow cross-module queries the stop
///        router (debug_stop.cpp) and task cleanup need: ownership lookup
///        without liveness (death events stay pollable), the debugger-death
///        drain, and the test-isolation reset.

#pragma once

#include <types.hpp>

namespace kernel {
struct TaskControlBlock;
}

namespace kernel::debug {

/// @brief Handle validation without liveness (issue #226): live binding
///        owned by @p caller for @p handle? Takes g_bind_lock internally —
///        never call with it held (the lock is non-recursive; nesting
///        self-deadlocks, e.g. poll-then-lookup).
/// @return true when the handle decodes to a live owned binding.
bool debug_validate_handle(uint64_t caller_id, uint64_t handle) noexcept;

/// @brief Debugger-death fail-safe: for every live binding owned by
///        @p dying, apply default dispositions (fault-stopped targets
///        terminate, cleanly-stopped resume), restore breakpoint shadows,
///        drop queued events, clear flags. No orphaned parked tasks.
///        Granted slots (issue #239, spec §14) participate by grantee
///        ownership; slots this task granted to others revoke here too
///        (grantor-death, same shared disposition). Runs in cleanup()
///        (task context, may terminate/block).
void debug_drain_debugger(TaskControlBlock &dying) noexcept;

/// @brief Test-isolation reset for the binding table (issue #226).
void debug_bindings_reset() noexcept;

/// @brief Live-binding query for the Logger-mute predicate (issue #232):
/// true when any binding is live. Backed by a lock-free live counter
/// (updated under g_bind_lock at every transition), so it is safe from
/// any context including tick/ISR — Logger::info/warn consult it on
/// paths reachable from the timer tick.
bool debug_session_active() noexcept;

/// @brief Owned-target snapshot for the stop poll (issue #226): collects///        all (target_id, target_gen) pairs owned by @p debugger_id (up to
///        @p cap) under g_bind_lock and releases it before returning, so
///        the poll can scan the stop queue WITHOUT nesting bind inside
///        the queue lock (lock order is bind -> stop everywhere; nesting
///        stop -> bind would invert detach's bind -> stop and deadlock).
/// @return Number of pairs stored (0 when none).
size_t debug_collect_owned(uint64_t debugger_id, uint64_t *ids_out,
                           uint32_t *gens_out, size_t cap) noexcept;

/// @brief Launch handoff for `runelf --debug` (issue #231): sel1-claim
///        immediately followed by the sel7 grant-transfer to @p grantee_id,
///        then the sel9 belt arm — one call with explicit ids (a literal
///        syscall-entry sequence cannot work: EBUSY after claim, EBADF on
///        a shell sel9 against the debugd-owned binding). Denial table
///        mirrors sel1+sel7 (ESRCH/EPERM/EBUSY, no new errno); no new
///        syscall or selector (spec §9.6).
/// @return 0 with @p handle_out minted, else -ESRCH/-EPERM/-EBUSY.
uint64_t debug_launch_handoff(uint64_t shell_id, uint64_t target_id,
                              uint64_t grantee_id,
                              uint64_t &handle_out) noexcept;

/// @brief Pointer-based handoff for the pre-add window (issue #295):
///        identical checks + slot mint as debug_launch_handoff, but the
///        target is passed directly — a freshly taken completion was
///        never registered, so find_task cannot resolve it, and
///        registering it first (add_task) would open a run-unobserved
///        window before the belt + entry breakpoint land (§9.7
///        fail-closed: handoff → bp → grant all precede add_task).
/// @return Same denial table as debug_launch_handoff.
uint64_t debug_launch_handoff_tc(uint64_t shell_id, TaskControlBlock &tgt,
                                 uint64_t grantee_id,
                                 uint64_t &handle_out) noexcept;

/// @brief Fail-closed teardown for a `--debug` launch (issue #231, spec
///        §9.2e/§9.7): drop the handoff binding slot (when @p handle is
///        nonzero), restore breakpoint shadows, terminate the just-added
///        task via the admitted-task path and drain. Never leaves the
///        target running.
void debug_launch_teardown(TaskControlBlock &task,
                           uint64_t handle) noexcept;

/// @brief Pre-add fail-closed undo for a `--debug` launch (issue #295):
///        the slot-drop + shadow-restore + flag-clear half of teardown
///        WITHOUT terminate/drain — the target was never added, so
///        terminate_err (registry removal) must not run. Pair with
///        ElfLoader::destroy_completed_tcb on the same failure paths.
void debug_launch_undo(TaskControlBlock &task, uint64_t handle) noexcept;

} // namespace kernel::debug
