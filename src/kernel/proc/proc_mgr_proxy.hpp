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

/// @file proc_mgr_proxy.hpp
/// @brief Proc-mgr policy proxy (F11, #564, strangler step a).
///        Kernel-side contract for the userspace proc-mgr PD: 48 B wire
///        RPC, PID cell, epoch/liveness, and the bounded Call. Everything
///        here is declaration-only; definitions live in proc_mgr_proxy.cpp
///        inside `#if CONFIG_PROC_MGR`, so the OFF build
///        (`CONFIG_PROC_MGR=0`, shipped default) carries no proc-mgr
///        symbols. Stage A wires no caller (all seven ops stay on the
///        kernel path); Stage B routes verdicts through proc_mgr_call() in
///        invoking-client context (INV-PROC-1), enforced via the unchanged
///        Q9 parent_id branch — the PD never calls the stop gate as caller
///        (INV-PROC-6).

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>
#include <kernel/proc/proc_errors.hpp>
#include <kernel/proc/proc_wire.hpp>

namespace kernel {

struct TaskControlBlock;

namespace proc::proc_mgr_proxy {

#if CONFIG_PROC_MGR

/// @brief Resolve the live proc-mgr holder task (the PD itself).
/// @return The holder task when the PID cell names a live (non
///         TERMINATED/REAPED, magic-checked) task, else nullptr (down).
///         The fork path installs holds in this task's CSpace.
TaskControlBlock *proc_mgr_holder() noexcept;
/// @brief Bounded verdict Call: build the owner-tagged (id+gen, #441)
///        request, run it through proc_mgr_call, and validate the reply
///        shape via ProcSvc::handle_decision. A forged/mismatched echo is
///        refused here as BAD_REQUEST (never consumed); STOP/RESUME stay
///        STOP_REFUSED; a non-zero PD verdict status fails the op closed
///        (deny-only, INV-PROC-11). NO_SERVER/RPC_BOUND/BAD_REQUEST pass
///        through. No lock is held anywhere in this function (INV-PROC-3).
///        The RPC_BOUND leg pushes the +21 dmesg row (outside any lock).
/// @return OK (verdict accepted for dispatch) or the typed refusal.
[[nodiscard]] errors::ProcError proc_mgr_verdict(ProcMgrOp op, uint64_t arg0,
                                                uint64_t arg1,
                                                uint64_t arg2) noexcept;
/// @brief Record the PID of the proc-mgr daemon task.
void set_proc_mgr_pid(uint64_t pid);
/// @brief Get the recorded proc-mgr PID (0 when unset).
uint64_t get_proc_mgr_pid();
/// @brief True when the server is live (present, not TERMINATED/REAPED).
bool proc_mgr_alive();
/// @brief Proxy epoch (bumped on restart; server purges on increase).
uint32_t proc_mgr_epoch();
/// @brief Bump the epoch (server restart path).
void proc_mgr_bump_epoch();
/// @brief Supervisor hook: record a restart (store pid, bump epoch).
void proc_mgr_on_restart(uint64_t pid);
/// @brief Bounded verdict Call: validate server, bounded wait, validated
///        reply (sender + epoch + owner + op echo). No lock is held across
///        the Call (INV-PROC-3); expiry fails closed with RPC_BOUND, a dead
///        or missing server with NO_SERVER (INV-PROC-2). Self-call from the
///        proc-mgr PD itself is refused as SELF_REFUSED before any IPC
///        (INV-PROC-14, bounded no-block, never starves the PD).
/// @return OK with *reply_out filled on a validated reply, else the typed
///         refusal (NO_SERVER / RPC_BOUND / BAD_REQUEST / SELF_REFUSED) with no block held.
[[nodiscard]] errors::ProcError proc_mgr_call(const ProcMgrRpc &req,
                                             ProcMgrRpc &reply_out) noexcept;
/// @brief QE probe seam (procfs_srv shape): BIND round-trip status.
/// @return 0 on an allowed BIND, else the PD/transport status; -38
///         (-ENOSYS) when the server is unavailable pre-flip. No
///         production caller; whole TU already knob-guarded.
int64_t proc_mgr_probe_bind();
/// @brief QE probe seam (procfs_srv shape): LOOPBACK echo round-trip.
/// @param echo_out When non-null, receives the echoed arg0 on success.
/// @return 0 on an echoed LOOPBACK, else the PD/transport status; -38
///         (-ENOSYS) when the server is unavailable pre-flip.
int64_t proc_mgr_probe_loopback(uint64_t payload, uint64_t *echo_out);

#endif // CONFIG_PROC_MGR

} // namespace proc::proc_mgr_proxy

} // namespace kernel
