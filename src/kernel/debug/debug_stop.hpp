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

/// @file debug_stop.hpp
/// @brief Debugger stop-event routing + kernel breakpoint shadows (issue
///        #226, spec docs/specs/debugd.md §4–§5). Fault/breakpoint/step
///        traps on attached targets enqueue a bounded stop event and park
///        the target instead of applying the default disposition. The
///        kernel owns breakpoint insertion bookkeeping (original bytes
///        shadowed); the controller keeps a write-through cache only.
///        Static bounded tables — no dynamic allocation on any path.

#pragma once

#include <types.hpp>

namespace kernel {
struct TaskControlBlock;
}

namespace kernel::debug {

/// @brief Stop-event kinds (spec §4). Zero is never enqueued (TCB
///        debug_stop_kind == 0 means cleanly parked, not fault-stopped).
enum class StopKind : uint64_t {
    BREAKPOINT = 1,
    STEP = 2,
    FAULT = 3,
    DEATH = 4,
};

/// @brief Stop event (64 bytes, plain layout — also the user ABI for the
///        ATTACH selector-2 poll: copied verbatim to the debugger buffer).
struct StopEvent {
    uint64_t target_id = 0;
    uint64_t target_gen = 0;
    uint64_t kind = 0;        // StopKind numerics above
    uint64_t fault_num = 0;   // vector / scause / ESR-EC
    uint64_t fault_addr = 0;  // CR2 / FAR / stval / stop VA
    uint64_t snap_state = 0;  // task-info snapshot: state at park time
    uint64_t snap_prio = 0;   // task-info snapshot: priority
    uint64_t snap_budget = 0; // task-info snapshot: reserved (0 in Phase 2)
};

/// @brief Notify pulse delivered to the debugger TCB on every enqueue.
///        Distinct from DEATH_WAKE_PULSE (different recipient contract:
//        debugger drains via selector-2 poll, not DeathNotify recv).
constexpr uint64_t DEBUG_STOP_PULSE = 0xDE8B6001ULL;

/// @brief Route a trap on @p target to its debugger (ISR fault context).
///        Enqueues the stop event, parks the target (BLOCKED + dequeued,
///        stop kind/VA latched), pokes the debugger Notify, and arms the
///        deferred switch away. Returns true when routed (caller skips the
///        default terminate/signal disposition); false when unattached or
///        ineligible (caller applies the existing disposition unchanged).
/// @param kind StopKind numerics (BREAKPOINT/STEP/FAULT).
/// @param num Arch fault identifier (vector / scause / ESR EC).
/// @param addr Fault address or stop VA (CR2 / FAR / stval / pc).
bool debug_route_fault(TaskControlBlock &target, uint64_t kind, uint64_t num,
                       uint64_t addr) noexcept;

/// @brief Non-blocking dequeue of the oldest stop event for @p debugger_id.
///        Locking: snapshots owned bindings first (bind released), then
///        scans under the queue lock only — never nests bind inside stop
///        (see debug_collect_owned; detach orders bind -> stop).
/// @return true with @p out filled; false when the queue holds nothing for
///         this debugger (caller reports EAGAIN).
bool debug_poll_event(uint64_t debugger_id, StopEvent &out) noexcept;

/// @brief Events dropped (overflow) since boot. Reported alongside polls.
/// @return Monotonic drop-oldest count (death-evicted deaths counted too).
uint64_t debug_dropped_count() noexcept;

/// @brief Insert a software breakpoint at @p va in @p target (kernel shadow
///        authoritative). Idempotent re-insert returns true.
/// @return false when the table is full, the VA is unmapped, or the target
///         is not debuggable.
bool debug_bp_insert(TaskControlBlock &target, uint64_t va) noexcept;

/// @brief Clear the breakpoint at @p va, restoring original bytes.
/// @return false when no shadow exists for (target, va).
bool debug_bp_clear(TaskControlBlock &target, uint64_t va) noexcept;

/// @brief Restore all shadowed breakpoints of @p target (detach + death).
void debug_bp_restore_all(TaskControlBlock &target) noexcept;

/// @brief Match a trap PC against @p target's shadows.
/// @param pc Trap-adjusted PC (x86: RIP-1 for int3; ARM/RISC-V: ELR/sepc).
/// @return VA of the shadow hit, 0 when no shadow matches.
uint64_t debug_bp_match(TaskControlBlock &target, uint64_t pc) noexcept;

/// @brief Continue a parked @p target: plain resume, or single-step-over
///        with re-arm when stopped on a breakpoint (restores orig byte,
///        arms one step, resumes; the STEP completion re-inserts).
/// @return false when the target is not parked for this debugger.
bool debug_continue(TaskControlBlock &target) noexcept;

/// @brief Single-step a parked @p target (arch-native TF/SS or RISC-V
///        next-insn emulation). Resumes the target; arrival routes a STEP
///        event through debug_route_fault.
/// @return false when the target is not parked or the arch step fails.
bool debug_step(TaskControlBlock &target) noexcept;

/// @brief Publish a task-death stop event (reserved slot, never dropped)
///        and poke the debugger. Runs in cleanup() before page-table free
///        while bindings and Notify are intact.
void debug_publish_death(TaskControlBlock &dying) noexcept;

/// @brief Debugger-death fail-safe (cleanup() hook): for every binding owned
///        by @p dying, apply default dispositions (fault-stopped targets
///        terminate, cleanly-stopped resume), restore shadows, drop queued
///        events, clear flags. No orphaned parked tasks.
void debug_drain_debugger(TaskControlBlock &dying) noexcept;

/// @brief Drop all queued events + shadows for a target by id (detach and
///        dead-target paths — works after the TCB is gone, no byte restore).
void debug_drop_target(uint64_t target_id, uint32_t target_gen) noexcept;

/// @brief Test-isolation reset: bindings, stop queue, shadows (issue #226).
///        Wired into test_isolate alongside DeathNotify.
void debug_snapshot_reset() noexcept;

} // namespace kernel::debug
