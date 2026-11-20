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

/// @file proc_wire.hpp
/// @brief Proc-mgr (F11, #564) wire protocol: verdict op + 48 B RPC.
///        Pointer-free by construction (ids + generations only, never TCB
///        pointers or kernel stack addresses — INV-PROC-1); every reply is
///        validated against owner_id/owner_gen (the #441 dead-letter rule).

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace kernel::proc {

/// @brief Wire ops carried in ProcMgrRpc::op (server dispatch switch).
///        BIND/LOOPBACK are the step-a liveness probes (procfs_srv shape);
///        the seven process ops ride the Stage B MOVE; STOP/RESUME are the
///        verdict-carried dispositions (enforced in invoking-client context
///        via the unchanged Q9 parent_id branch — INV-PROC-6, the PD never
///        calls the gate as caller).
enum class ProcMgrOp : uint8_t {
    BIND = 0,     ///< server announce / capability handshake
    LOOPBACK = 1, ///< echo probe (parity: kernel vs server path)
    FORK = 2,     ///< fork policy decision (quota/ownership)
    EXEC = 3,     ///< exec policy decision (path policy)
    WAITPID = 4,  ///< waitpid ownership predicate
    KILL = 5,     ///< kill disposition class (fatal vs handler)
    GETPID = 6,   ///< identity read (O(1), no RPC on the hot path)
    SIGNAL = 7,   ///< signal handler-table decision
    SIGRETURN = 8, ///< trap-frame restore (mechanism only, no proxy leg)
    STOP = 9,     ///< stop disposition (Stage B verdict only)
    RESUME = 10,  ///< resume disposition (Stage B verdict only)
};

/// @brief 48 B wire struct (fits CONFIG_IPC_FAST_PAYLOAD_BYTES).
struct ProcMgrRpc {
    uint8_t op;         ///< ProcMgrOp
    uint8_t flags;      ///< reserved (zero)
    uint16_t rsvd;      ///< reserved (zero)
    uint32_t epoch;     ///< proxy epoch (server purges on increase)
    uint32_t owner_id;  ///< requesting task id (issue #441: never PID 1)
    uint32_t owner_gen; ///< requesting task generation
    uint64_t arg0;      ///< op-scoped argument 0
    uint64_t arg1;      ///< op-scoped argument 1
    uint64_t arg2;      ///< op-scoped argument 2
    uint16_t status_op; ///< echoed op for mismatch detection
    uint16_t status;    ///< reply: negated errno (0 = ok)
    uint32_t rsvd3;     ///< reserved (zero)
};

static_assert(sizeof(ProcMgrRpc) == 48,
              "ProcMgrRpc must be exactly 48 B (fast IPC payload)");
static_assert(sizeof(ProcMgrRpc) <= CONFIG_IPC_FAST_PAYLOAD_BYTES,
              "ProcMgrRpc must fit the fast IPC payload");

} // namespace kernel::proc
