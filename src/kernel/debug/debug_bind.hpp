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
///        Runs in cleanup() (task context, may terminate/block).
void debug_drain_debugger(TaskControlBlock &dying) noexcept;

/// @brief Test-isolation reset for the binding table (issue #226).
void debug_bindings_reset() noexcept;

/// @brief Owned-target snapshot for the stop poll (issue #226): collects
///        all (target_id, target_gen) pairs owned by @p debugger_id (up to
///        @p cap) under g_bind_lock and releases it before returning, so
///        the poll can scan the stop queue WITHOUT nesting bind inside
///        the queue lock (lock order is bind -> stop everywhere; nesting
///        stop -> bind would invert detach's bind -> stop and deadlock).
/// @return Number of pairs stored (0 when none).
size_t debug_collect_owned(uint64_t debugger_id, uint64_t *ids_out,
                           uint32_t *gens_out, size_t cap) noexcept;

} // namespace kernel::debug
