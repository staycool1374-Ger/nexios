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

namespace kernel::proc::proc_mgr_proxy {

#if CONFIG_PROC_MGR

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
///        or missing server with NO_SERVER (INV-PROC-2).
/// @return OK with *reply_out filled on a validated reply, else the typed
///         refusal (NO_SERVER / RPC_BOUND / BAD_REQUEST) with no block held.
[[nodiscard]] errors::ProcError proc_mgr_call(const ProcMgrRpc &req,
                                             ProcMgrRpc &reply_out) noexcept;

#endif // CONFIG_PROC_MGR

} // namespace kernel::proc::proc_mgr_proxy
