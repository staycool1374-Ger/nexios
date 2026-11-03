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

/// @file cap_broker.hpp
/// @brief Capability-broker policy proxy (issue #240, strangler step a).
///        Kernel-side contract for the userspace cap-broker PD: 48 B wire
///        RPC, PID cell, epoch/liveness, tombstone drain, and probe
///        entry points. Everything here is declaration-only; definitions
///        live in cap_broker.cpp inside `#if CONFIG_CAP_BROKER`, so the OFF
///        build (`CONFIG_CAP_BROKER=0`, shipped default) is byte-identical
///        to the pre-#240 tree. Broker-allows/kernel-enforces: the broker
///        manages per-task policy words, the kernel enforces the cached
///        word on the grant/copy/mint/retype path. Step-a: BIND+LOOPBACK
///        live, policy ops typed -ENOSYS pre-flip.

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace kernel::cap_broker {

#if CONFIG_CAP_BROKER

/// @brief Wire ops carried in CapBrokerRpc::op (server dispatch switch).
enum class CapBrokerOp : uint8_t {
    BIND = 0,        ///< server announce / capability handshake
    LOOPBACK = 1,    ///< echo probe (parity: kernel vs server path)
    SET_POLICY = 2,  ///< set per-task delegation mask (step-b; -ENOSYS now)
    GET_POLICY = 3,  ///< read back per-task mask (step-b; -ENOSYS now)
    CLEAR_POLICY = 4,///< clear per-task mask bits (step-b; -ENOSYS now)
};

/// @brief 48 B wire struct (fits CONFIG_IPC_FAST_PAYLOAD_BYTES).
struct CapBrokerRpc {
    uint8_t op;         ///< CapBrokerOp
    uint8_t flags;      ///< reserved (zero)
    uint16_t rsvd;      ///< reserved (zero)
    uint32_t epoch;     ///< proxy epoch (server purges on increase)
    uint32_t owner_id;  ///< requesting task id (issue #441: never PID 1)
    uint32_t owner_gen; ///< requesting task generation
    uint64_t target_id; ///< policy target task id (SET/GET/CLEAR)
    uint32_t mask;      ///< delegation mask bits (SET/CLEAR)
    uint32_t rsvd2;     ///< reserved (zero)
    uint64_t arg0;      ///< LOOPBACK echo payload
    uint16_t status_op; ///< echoed op for mismatch detection
    uint16_t status;    ///< reply: negated errno (0 = ok)
    uint32_t rsvd3;     ///< reserved (zero)
};

static_assert(sizeof(CapBrokerRpc) <= 48,
              "CapBrokerRpc must fit the 48 B fast IPC payload");

/// @brief Record the PID of the cap-broker daemon task.
void set_cap_broker_pid(uint64_t pid);
/// @brief Get the recorded cap-broker PID (0 when unset).
uint64_t get_cap_broker_pid();
/// @brief True when the server task is live (present, not TERMINATED/REAPED).
bool cap_broker_alive();
/// @brief Proxy epoch (bumped on restart / snapshot reset / boot table).
uint32_t cap_broker_epoch();
/// @brief Bump the epoch and purge tombstones (server restart path).
void cap_broker_bump_epoch();
/// @brief Supervisor hook: record a restart (store pid, bump epoch).
void cap_broker_on_restart(uint64_t pid);
/// @brief Teardown hook: latch a dead owner's id+generation (lock-free).
void cap_broker_note_dead_owner(uint32_t owner_id, uint32_t owner_gen);
/// @brief Generic RPC: validate server, bounded wait, validated reply.
/// @return true with *reply filled on a validated reply, false fail-closed
///         (server down, timeout, or reply mismatch) with no block held.
[[nodiscard]] bool cap_broker_rpc(const CapBrokerRpc &req,
                                  CapBrokerRpc &reply);
/// @brief BIND probe: attach to a live PD, expect ok.
/// @return 0 on bound, -ENOSYS when the server is unavailable.
int64_t cap_broker_probe_bind();
/// @brief LOOPBACK probe: echo arg0 through the live PD.
/// @return 0 with *echo filled on success, -ENOSYS when unavailable.
int64_t cap_broker_probe_loopback(uint64_t payload, uint64_t *echo);
/// @brief SET_POLICY probe (step-b; always -ENOSYS at step-a).
/// @return -ENOSYS pre-flip.
int64_t cap_broker_probe_set_policy(uint64_t target_id, uint32_t mask);
/// @brief GET_POLICY probe (step-b; always -ENOSYS at step-a).
/// @return -ENOSYS pre-flip.
int64_t cap_broker_probe_get_policy(uint64_t target_id, uint32_t *mask);

#endif // CONFIG_CAP_BROKER

} // namespace kernel::cap_broker
