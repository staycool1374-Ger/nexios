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

/// @file proc_tcb_caps.hpp
/// @brief Proc-mgr TCB-cap hold table (F11 Stage A, #564; §8.8 protocol).
///        Tracks one CREATOR TcbCap per created task, installed in the
///        holder's CSpace with an F2 derivation edge. Holds are lifecycle
///        tracking only — they confer zero stop power (the PD never calls
///        the Q9 gate as caller; stops are enforced in invoking-client
///        context via the unchanged parent_id branch, INV-PROC-6).
///        Bounded: CONFIG_PROC_MGR_MAX_HELD_TCB_CAPS fixed slots, no
///        allocation on any path; overflow fails closed (CAP_TABLE_FULL,
///        no zombie, no half-hold). Generation-keyed (pid, generation):
///        a recycled pid can never match a stale hold. release_on_death()
///        runs in the M1 death funnel under IrqGuard (scheduler_lock_ NOT
///        held — scheduler_terminate.cpp:54), is idempotent across
///        double-publish, and performs no logging.

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>
#include <kernel/proc/proc_errors.hpp>

namespace kernel {

struct TaskControlBlock;

namespace proc::proc_tcb_caps {

/// @brief Hold-table capacity (INV-PROC-4): one slot per tracked child,
///        at most one CNode worth. Overflow fails closed, never a panic.
inline constexpr uint32_t kMaxHeldTcbCaps =
    CONFIG_PROC_MGR_MAX_HELD_TCB_CAPS;
static_assert(kMaxHeldTcbCaps >= 1, "hold table must hold at least one cap");
static_assert(kMaxHeldTcbCaps <= CONFIG_CSLOT_COUNT,
              "hold table must fit one holder CNode");

/// @brief Install a CREATOR TcbCap for (target_pid, target_gen) in the
///        holder's CSpace and record the F2 derivation edge. The install
///        publishes only after mint + edge both succeed (never a half-hold:
///        any failure removes the CNode slot and drops the creator ref).
/// @param holder Task whose CSpace receives the cap (ensure_cspace'd here).
/// @param err_out When non-null, carries the typed reason on failure
///        (STALE_PID / CAP_TABLE_FULL / HOLD_INSTALL_FAILED); untouched on
///        success (INV-RT-5).
/// @return Hold-table slot index (0..kMaxHeldTcbCaps-1) on success, -1 on
///         any failure. Runs in syscall context only (takes the table leaf
///         lock); never in the death funnel (use release_on_death there).
int hold(TaskControlBlock &holder, uint64_t target_pid, uint32_t target_gen,
         errors::ProcError *err_out) noexcept;

/// @brief Resolve the live target of the holder's hold on
///        (target_pid, target_gen). Generation equality is required: a
///        recycled pid never resolves (fail closed, nullptr).
/// @return The live target task, or nullptr on any failure (no hold,
///         stale CNode slot, revoked/wrong-type cap, dead or recycled
///         target). No side effects.
TaskControlBlock *lookup(TaskControlBlock &holder, uint64_t target_pid,
                         uint32_t target_gen) noexcept;

/// @brief Drop the holder's hold on (target_pid, target_gen): removes the
///        CNode slot (gen-checked; the derivation edge prunes with it),
///        releases the wrapper, clears the index entry.
/// @return OK on release or on a nothing-held no-op (idempotent);
///         STALE_PID when a same-pid entry names another generation
///         (pid recycled — the slot is never touched).
errors::ProcError release(TaskControlBlock &holder, uint64_t target_pid,
                         uint32_t target_gen) noexcept;

/// @brief Funnel hook: drop every hold naming (dead_pid, dead_gen),
///        whether as target (child died: remove the holder's CNode slot)
///        or as holder (holder died: its CNode teardown already released
///        the wrappers and pruned the edges — clear the index only).
///        Locking: caller holds IrqGuard with scheduler_lock_ NOT held
///        (publish_task_death contract); takes only the table leaf lock
///        plus the CNode leaf internals of remove_if (sequential, never
///        nested — INV-DERIV-1: no CNode slot lock is ever held across a
///        derivation-table call). Idempotent: a cleared or recycled entry
///        is a generation-mismatch no-op. No logging.
/// @param dead_pid Terminated task id.
/// @param dead_gen Terminated task generation at death.
void release_on_death(uint64_t dead_pid, uint32_t dead_gen) noexcept;

/// @brief Clear the hold index (test-isolation restore seam, INV-PROC-8).
///        Contract: the caller has already torn down the holder CNodes
///        (MemPool restore frees the wrappers); this drops the index only.
///        Production never calls this (funnel + release own the lifecycle).
void snapshot_reset() noexcept;

/// @brief Number of live holds (QE introspection for the Stage B parity
///        class; lock-free atomic read).
uint64_t live_hold_count() noexcept;

} // namespace proc::proc_tcb_caps

} // namespace kernel
