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

/// @file log_srv.hpp
/// @brief Log-sink PD contract (issue #51, strangler step a).
///        Kernel-side contract for the userspace log-srv PD: 48 B wire
///        RPC, PID cell, epoch/liveness, tombstone drain, and probe
///        entry points. Everything here is declaration-only; definitions
///        live in log_srv.cpp inside `#if CONFIG_LOG_SRV`, so the OFF
///        build (`CONFIG_LOG_SRV=0`, shipped default) is byte-identical
///        to the pre-#51 tree. The kernel panic path never leaves the
///        kernel.

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace kernel::log_srv {

#if CONFIG_LOG_SRV

/// @brief Wire ops carried in LogRpc::op (server dispatch switch).
enum class LogSrvOp : uint8_t {
    BIND = 0,          ///< server announce / capability handshake
    SINK_PUSH = 1,     ///< sink one record probe
    SINK_DRAIN = 2,    ///< drain the sink probe
    METER = 3,         ///< drop/meter counters probe
    INJECT_DROP = 4,   ///< one-shot drop drill (diagnostic)
};

/// @brief 48 B wire struct (fits CONFIG_IPC_FAST_PAYLOAD_BYTES).
struct LogRpc {
    uint8_t op;         ///< LogSrvOp
    uint8_t flags;      ///< bit0: DEGRADED
    uint16_t rsvd;      ///< reserved (zero)
    uint32_t epoch;     ///< proxy epoch (server purges on increase)
    uint32_t owner_id;  ///< requesting task id
    uint32_t owner_gen; ///< requesting task generation
    uint64_t arg0;      ///< op argument (probe selector)
    uint64_t arg1;      ///< op argument (probe bound)
    uint16_t status_op; ///< echoed op for mismatch detection
    uint16_t status;    ///< reply: negated errno (0 = ok)
    uint32_t rsvd2;     ///< reserved (zero)
};

static_assert(sizeof(LogRpc) <= 48,
              "LogRpc must fit the 48 B fast IPC payload");

/// @brief Record the PID of the log-srv daemon task.
void set_log_srv_pid(uint64_t pid);
/// @brief Get the recorded log-srv PID (0 when unset).
uint64_t get_log_srv_pid();
/// @brief True when the server task is live (present, not TERMINATED/REAPED).
bool log_srv_alive();
/// @brief Proxy epoch (bumped on restart / snapshot reset / boot table).
uint32_t log_srv_epoch();
/// @brief Bump the epoch and purge tombstones (server restart path).
void log_srv_bump_epoch();
/// @brief Supervisor hook: record a restart (store pid, bump epoch).
void log_srv_on_restart(uint64_t pid);
/// @brief Teardown hook: latch a dead owner's id+generation (lock-free).
void log_srv_note_dead_owner(uint32_t owner_id, uint32_t owner_gen);
/// @brief Generic RPC: validate server, bounded wait, validated reply.
/// @return true with *reply filled on a validated reply, false fail-closed
///         (server down, timeout, or reply mismatch) with no block held.
[[nodiscard]] bool log_srv_rpc(const LogRpc &req, LogRpc &reply);
/// @brief BIND probe: attach to a live PD, expect ok.
/// @return 0 on bound, -ENOSYS when the server is unavailable.
int64_t log_srv_probe_bind();
/// @brief Drop drill: one-shot INJECT_DROP.
/// @return 0 posted, -ENOSYS when the server is unavailable.
int64_t log_srv_fault_drop();

#endif // CONFIG_LOG_SRV

} // namespace kernel::log_srv
