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

/// @file term_srv.hpp
/// @brief Terminal/emulation policy proxy (issue #51, strangler step a).
///        Kernel-side contract for the userspace term-srv PD: 48 B wire
///        RPC, PID cell, epoch/liveness, tombstone drain, and probe
///        entry points. Everything here is declaration-only; definitions
///        live in term_srv.cpp inside `#if CONFIG_TERM_SRV`, so the OFF
///        build (`CONFIG_TERM_SRV=0`, shipped default) is byte-identical
///        to the pre-#51 tree. Emulation policy only — the kernel keeps
///        panic raw_write + serial mechanism.

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace kernel::term_srv {

#if CONFIG_TERM_SRV

/// @brief Wire ops carried in TermRpc::op (server dispatch switch).
enum class TermSrvOp : uint8_t {
    BIND = 0,          ///< server announce / capability handshake
    PROMPT = 1,        ///< prompt render probe
    READLINE = 2,      ///< readline probe
    HISTORY = 3,       ///< history probe
    SCROLL = 4,        ///< bounded scroll probe
    CLEAR = 5,         ///< clear-screen probe
    REDRAW = 6,        ///< redraw probe
    ECHO = 7,          ///< echo probe
    CANCEL_OWNERS = 8, ///< piggybacked tombstone drain (no reply expected)
    INJECT_POISON = 9, ///< one-shot poison probe (diagnostic)
};

/// @brief 48 B wire struct (fits CONFIG_IPC_FAST_PAYLOAD_BYTES).
struct TermRpc {
    uint8_t op;         ///< TermSrvOp
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

static_assert(sizeof(TermRpc) <= 48,
              "TermRpc must fit the 48 B fast IPC payload");

/// @brief Record the PID of the term-srv daemon task.
void set_term_srv_pid(uint64_t pid);
/// @brief Get the recorded term-srv PID (0 when unset).
uint64_t get_term_srv_pid();
/// @brief True when the server task is live (present, not TERMINATED/REAPED).
bool term_srv_alive();
/// @brief Proxy epoch (bumped on restart / snapshot reset / boot table).
uint32_t term_srv_epoch();
/// @brief Bump the epoch and purge tombstones (server restart path).
void term_srv_bump_epoch();
/// @brief Supervisor hook: record a restart (store pid, bump epoch).
void term_srv_on_restart(uint64_t pid);
/// @brief Teardown hook: latch a dead owner's id+generation (lock-free).
void term_srv_note_dead_owner(uint32_t owner_id, uint32_t owner_gen);
/// @brief Generic RPC: validate server, bounded wait, validated reply.
/// @return true with *reply filled on a validated reply, false fail-closed
///         (server down, timeout, or reply mismatch) with no block held.
[[nodiscard]] bool term_srv_rpc(const TermRpc &req, TermRpc &reply);
/// @brief BIND probe: attach to a live PD, expect ok.
/// @return 0 on bound, -ENOSYS when the server is unavailable.
int64_t term_srv_probe_bind();
/// @brief Poison probe: one-shot INJECT_POISON drill.
/// @return 0 posted, -ENOSYS when the server is unavailable.
int64_t term_srv_fault_poison();

#endif // CONFIG_TERM_SRV

} // namespace kernel::term_srv
