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

/// @file tcb_control.hpp
/// @brief F4 TCB control gate (issue #557): handle -> live TCB resolve,
///        the Q9 authority rule, N1 field semantics, the never-mem-mgr
///        predicate.  Pure functions (no scheduler lock, no mutation
///        apart from the CNode leaf take) so syscall and tests share one
///        gate.  Lock order: the mem-mgr Untyped walk takes the target
///        CSpace leaf lock (cap::lookup class); the gate runs with NO
///        scheduler lock held, so no order is created.

#pragma once

#include <types.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/task/tcb_control_errors.hpp>

namespace kernel {

class TaskControlBlock;

namespace tcb_control {

/// @brief Captured TcbCap authority tuple (INV-ADM-12 branch 6 input).
///        POD: avoids the five-scalar swappable-parameter smell (S3-b).
///        Captured BEFORE the lookup reference is released (no TOCTOU on
///        the scope/grantee words).
struct TcbControlAuthority {
    uint8_t scope = 0;     ///< TcbCap::Authority numerics
    uint64_t target_id = 0;  ///< cap's bound target id
    uint32_t target_gen = 0; ///< cap's bound target generation
    uint64_t grantee_id = 0; ///< SUPERVISOR grantee id (0 otherwise)
    uint32_t grantee_gen = 0; ///< SUPERVISOR grantee generation
};

/// @brief Resolved control request: live target + captured authority.
struct TcbControlResolved {
    TaskControlBlock *target = nullptr; ///< live, validated target
    TcbControlAuthority cap;            ///< captured cap tuple
    errors::TcbCtlError err = errors::TCBCTL_ERR_OK; ///< typed outcome
};

/// @brief Resolve @p handle in the caller's CSpace to a live TCB.
///        ensure_cspace(); lookup(TCB, WRITE); TcbCap tag check; capture
///        the authority tuple BEFORE release(); validate_id; find_task;
///        magic; generation equality; TERMINATED/REAPED filter — the
///        exact thread_join_resolve shape.
///        Pure/read-only apart from the leaf CNode take; no scheduler
///        lock (callable from the syscall gate and from tests).
/// @param caller Caller TCB (owns the CSpace the handle names).
/// @param handle User handle of a TcbCap.
/// @return Resolved target + authority, or err != OK (target null).
TcbControlResolved tcb_control_resolve(TaskControlBlock &caller,
                                       uint64_t handle) noexcept;

/// @brief Q9 authority rule (INV-ADM-12), evaluated in order; every
///        refusal is typed with ZERO side effects:
///        SELF, KERNEL_TASK, IDLE, PROTECTED_ROLE, CREATOR branch
///        (target.parent_id == caller.id), SUPERVISOR branch (cap scope
///        == SUPERVISOR bound to caller id+generation), else
///        NO_AUTHORITY.  DeathNotify::is_watching confers NOTHING (its
///        slots are self-grantable); raw WdogCap possession is not stop
///        authority either.
/// @param caller Caller TCB.
/// @param target Live resolved target.
/// @param cap  Captured authority tuple from tcb_control_resolve.
/// @return TCBCTL_ERR_OK when allowed, else the typed refusal.
errors::TcbCtlError
tcb_control_authorize(const TaskControlBlock &caller,
                      const TaskControlBlock &target,
                      const TcbControlAuthority &cap) noexcept;

/// @brief Never-mem-mgr predicate (INV-ADM-15): true iff ANY of (a) a
///        registered mem-mgr pid exists and equals the target id (the
///        cell is 0 today — truthful: no user-space mem-mgr exists —
///        and 0 matches nothing); (b) the target is a user task whose
///        CSpace holds a live CapType::Untyped cap (the spec's defining
///        property of the mem-mgr, §6.1); (c) the compile-time tripwire
///        (see below) breaks the build when a mem-mgr/supervisor role
///        row is added.  Layer (b) is the durable part: when M4 hands
///        the root Untyped to a user PD, that PD becomes unsuspendable
///        automatically.
/// @param target Live resolved target.
/// @return true when the target is protected (refuse with
///         PROTECTED_ROLE).
bool tcb_control_role_protected(const TaskControlBlock &target) noexcept;

/// @brief Registered mem-mgr pid cell (M4/F10 hook).  0 today (no
///        user-space mem-mgr exists; memory is kernel-internal).  The
///        bootstrap will set it via the setter; 0 matches nothing.
/// @return The registered mem-mgr pid, or 0.
uint64_t tcb_control_mem_mgr_pid() noexcept;

/// @brief Set the mem-mgr pid cell (M4/F10 bootstrap hook).  __atomic
///        accessors, no lock, no config key.
/// @param pid Mem-mgr task id (0 clears).
void tcb_control_set_mem_mgr_pid(uint64_t pid) noexcept;

} // namespace tcb_control
} // namespace kernel
