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

/// @file wdog_mon.hpp
/// @brief Watchdog-monitor policy proxy (issue #390, strangler step a).
///        Kernel-side contract for the userspace wdog-mon PD: 48 B wire
///        RPC, PID cell, epoch/liveness, tombstone drain, and probe
///        entry points. Everything here is declaration-only; definitions
///        live in wdog_mon.cpp inside `#if CONFIG_WDOG_MON`, so the OFF
///        build (`CONFIG_WDOG_MON=0`, shipped default) is byte-identical
///        to the pre-#390 tree. Monitor-composes-policy: the kernel keeps
///        the tick-scan (#41) + death pulses (#105) + watchdogd base
///        (#277); the monitor adds class policy + SHM + escalation over
///        that mechanism. Step-a: BIND+LOOPBACK live, register/heartbeat
///        ops typed -ENOSYS pre-flip.

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace kernel::wdog_mon {

#if CONFIG_WDOG_MON

/// @brief Wire ops carried in WdogMonRpc::op (server dispatch switch).
enum class WdogMonOp : uint8_t {
    BIND = 0,      ///< server announce / capability handshake
    LOOPBACK = 1,  ///< echo probe (parity: kernel vs server path)
    REGISTER = 2,  ///< register a supervised pid (step-b; -ENOSYS now)
    HEARTBEAT = 3, ///< heartbeat probe for a pid (step-b; -ENOSYS now)
    UNREGISTER = 4,///< drop a supervised pid (step-b; -ENOSYS now)
};

/// @brief 48 B wire struct (fits CONFIG_IPC_FAST_MSG_BYTES).
struct WdogMonRpc {
    uint8_t op;         ///< WdogMonOp
    uint8_t flags;      ///< reserved (zero)
    uint16_t rsvd;      ///< reserved (zero)
    uint32_t epoch;     ///< proxy epoch (server purges on increase)
    uint32_t owner_id;  ///< requesting task id (issue #441: never PID 1)
    uint32_t owner_gen; ///< requesting task generation
    uint64_t target_id; ///< supervised task id (REGISTER/HEARTBEAT/UNREG)
    uint32_t klass;     ///< supervision class (REGISTER: 200ms/1s/2s id)
    uint32_t rsvd2;     ///< reserved (zero)
    uint64_t arg0;      ///< LOOPBACK echo payload
    uint16_t status_op; ///< echoed op for mismatch detection
    uint16_t status;    ///< reply: negated errno (0 = ok)
    uint32_t rsvd3;     ///< reserved (zero)
};

static_assert(sizeof(WdogMonRpc) <= 48,
              "WdogMonRpc must fit the 48 B fast IPC payload");

/// @brief Record the PID of the wdog-mon daemon task.
void set_wdog_mon_pid(uint64_t pid);
/// @brief Get the recorded wdog-mon PID (0 when unset).
uint64_t get_wdog_mon_pid();
/// @brief True when the server task is live (present, not TERMINATED/REAPED).
bool wdog_mon_alive();
/// @brief Proxy epoch (bumped on restart / snapshot reset / boot table).
uint32_t wdog_mon_epoch();
/// @brief Bump the epoch and purge tombstones (server restart path).
void wdog_mon_bump_epoch();
/// @brief Supervisor hook: record a restart (store pid, bump epoch).
void wdog_mon_on_restart(uint64_t pid);
/// @brief Teardown hook: latch a dead owner's id+generation (lock-free).
void wdog_mon_note_dead_owner(uint32_t owner_id, uint32_t owner_gen);
/// @brief Generic RPC: validate server, bounded wait, validated reply.
/// @return true with *reply filled on a validated reply, false fail-closed
///         (server down, timeout, or reply mismatch) with no block held.
[[nodiscard]] bool wdog_mon_rpc(const WdogMonRpc &req, WdogMonRpc &reply);
/// @brief BIND probe: attach to a live PD, expect ok.
/// @return 0 on bound, -ENOSYS when the server is unavailable.
int64_t wdog_mon_probe_bind();
/// @brief LOOPBACK probe: echo arg0 through the live PD.
/// @return 0 with *echo filled on success, -ENOSYS when unavailable.
int64_t wdog_mon_probe_loopback(uint64_t payload, uint64_t *echo);
/// @brief REGISTER probe (step-b; always -ENOSYS at step-a).
/// @return -ENOSYS pre-flip.
int64_t wdog_mon_probe_register(uint64_t target_id, uint32_t klass);
/// @brief HEARTBEAT probe (step-b; always -ENOSYS at step-a).
/// @return -ENOSYS pre-flip.
int64_t wdog_mon_probe_heartbeat(uint64_t target_id);

#endif // CONFIG_WDOG_MON

} // namespace kernel::wdog_mon
