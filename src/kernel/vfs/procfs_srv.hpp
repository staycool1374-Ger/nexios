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

/// @file procfs_srv.hpp
/// @brief Procfs-server policy proxy (issue #54, strangler step a).
///        Kernel-side contract for the userspace procfs-srv PD: 48 B wire
///        RPC, PID cell, epoch/liveness, tombstone drain, and probe
///        entry points. Everything here is declaration-only; definitions
///        live in procfs_srv.cpp inside `#if CONFIG_PROCFS_SRV`, so the
///        OFF build (`CONFIG_PROCFS_SRV=0`, shipped default) is
///        byte-identical to the pre-#54 tree. Renders stay in-kernel:
///        the vfsd PROCFS_RENDER=7 probe retains sole render ownership
///        pre-flip and this PD claims none. Step-a: BIND+LOOPBACK live,
///        render ops typed -ENOSYS pre-flip.

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace kernel::procfs_srv {

#if CONFIG_PROCFS_SRV

/// @brief Wire ops carried in ProcfsSrvRpc::op (server dispatch switch).
enum class ProcfsSrvOp : uint8_t {
    BIND = 0,     ///< server announce / capability handshake
    LOOPBACK = 1, ///< echo probe (parity: kernel vs server path)
    RENDER = 2,   ///< node render (step-b; -ENOSYS now)
};

/// @brief 48 B wire struct (fits CONFIG_IPC_FAST_MSG_BYTES).
struct ProcfsSrvRpc {
    uint8_t op;         ///< ProcfsSrvOp
    uint8_t flags;      ///< reserved (zero)
    uint16_t rsvd;      ///< reserved (zero)
    uint32_t epoch;     ///< proxy epoch (server purges on increase)
    uint32_t owner_id;  ///< requesting task id (issue #441: never PID 1)
    uint32_t owner_gen; ///< requesting task generation
    uint64_t arg0;      ///< LOOPBACK echo payload / RENDER ino (op-scoped)
    uint64_t offset;    ///< byte offset (RENDER-scoped, step-b)
    uint64_t size;      ///< byte count (RENDER-scoped, step-b)
    uint16_t status_op; ///< echoed op for mismatch detection
    uint16_t status;    ///< reply: negated errno (0 = ok)
    uint32_t rsvd3;     ///< reserved (zero)
};

static_assert(sizeof(ProcfsSrvRpc) <= 48,
              "ProcfsSrvRpc must fit the 48 B fast IPC payload");

/// @brief Record the PID of the procfs-srv daemon task.
void set_procfs_srv_pid(uint64_t pid);
/// @brief Get the recorded procfs-srv PID (0 when unset).
uint64_t get_procfs_srv_pid();
/// @brief True when the server is live (present, not TERMINATED/REAPED).
bool procfs_srv_alive();
/// @brief Proxy epoch (bumped on restart / snapshot reset / boot table).
uint32_t procfs_srv_epoch();
/// @brief Bump the epoch and purge tombstones (server restart path).
void procfs_srv_bump_epoch();
/// @brief Supervisor hook: record a restart (store pid, bump epoch).
void procfs_srv_on_restart(uint64_t pid);
/// @brief Teardown hook: latch a dead owner's id+generation (lock-free).
void procfs_srv_note_dead_owner(uint32_t owner_id, uint32_t owner_gen);
/// @brief Generic RPC: validate server, bounded wait, validated reply.
/// @return true with *reply filled on a validated reply, false fail-closed
///         (server down, timeout, or reply mismatch) with no block held.
[[nodiscard]] bool procfs_srv_rpc(const ProcfsSrvRpc &req,
                                  ProcfsSrvRpc &reply);
/// @brief BIND probe: attach to a live PD, expect ok.
/// @return 0 on bound, -ENOSYS when the server is unavailable.
int64_t procfs_srv_probe_bind();
/// @brief LOOPBACK probe: echo arg0 through the live PD.
/// @return 0 with *echo filled on success, -ENOSYS when unavailable.
int64_t procfs_srv_probe_loopback(uint64_t payload, uint64_t *echo);
/// @brief RENDER probe (step-b; always -ENOSYS at step-a).
/// @return -ENOSYS pre-flip.
int64_t procfs_srv_probe_render(uint64_t ino, uint64_t offset,
                                uint64_t size);

#endif // CONFIG_PROCFS_SRV

} // namespace kernel::procfs_srv
