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

/// @file driver_pds.hpp
/// @brief Driver PD role table (issue #48): per-role PID cell, epoch,
///        liveness, tombstone drain, and the validated server RPC.
///        Single bounded table (one entry per PdRole); no per-role copies
///        of epoch/validation logic. All bodies in driver_pds.cpp.

#include <types.hpp>
#include <kernel/nexios_config.h>
#include <kernel/task/boot_sdf.hpp>

namespace kernel {
namespace driver_pds {

/// @brief 48 B wire budget shared with the driver PDs (mirrors the
/// timer-srv TimerRpc budget; see userspace/driver_pd.h).
struct DriverRpc {
    uint8_t op;
    uint8_t flags;
    uint16_t rsvd;
    uint32_t epoch;
    uint32_t owner_id;
    uint32_t owner_gen;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
    uint16_t status_op;
    uint16_t status;
    uint32_t rsvd2;
};

static_assert(sizeof(DriverRpc) <= 48,
              "DriverRpc must fit the 48 B fast IPC payload");

/// @brief Bounded wait budget for one kernel→PD RPC (coarse ticks).
/// Fail-closed -EAGAIN past this; never parks a kernel task forever.
inline constexpr uint64_t kRpcGuardTicks = 5000;

/// @brief Record the PID of a driver PD role.
void set_pd_pid(sdf::PdRole role, uint64_t pid);
/// @brief Get the recorded PID (0 when unset).
uint64_t get_pd_pid(sdf::PdRole role);
/// @brief True when the role's PD is live (present, not TERMINATED/REAPED).
bool pd_alive(sdf::PdRole role);
/// @brief Proxy epoch (bumped on restart / snapshot reset / boot table).
uint32_t pd_epoch();
/// @brief Bump the epoch and purge tombstones (restart path).
void pd_bump_epoch();
/// @brief Supervisor hook: record a restart (store pid, bump epoch).
void pd_on_restart(sdf::PdRole role, uint64_t pid);
/// @brief Teardown hook: latch a dead owner's id+generation (lock-free).
void pd_note_dead_owner(uint32_t owner_id, uint32_t owner_gen);
/// @brief Validated RPC to a driver PD: epoch/owner-validated reply.
/// @return true with *reply filled on a validated reply, false fail-closed.
[[nodiscard]] bool pd_rpc(sdf::PdRole role, const DriverRpc &req,
                          DriverRpc &reply);

} // namespace driver_pds
} // namespace kernel
