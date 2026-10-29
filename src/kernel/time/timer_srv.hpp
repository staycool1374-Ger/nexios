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

/// @file timer_srv.hpp
/// @brief Timer/Event externalization proxy (issue #241, strangler step a).
///        Kernel-side contract for the userspace timer-srv PD: 48 B wire
///        RPC, PID cell, epoch/liveness, tombstone drain, and the sys_*
///        proxy entry points used by the 80-83 handlers when
///        `CONFIG_TIMER_SRV=1`.  Everything here is declaration-only;
///        definitions live in timer_srv.cpp inside `#if CONFIG_TIMER_SRV`,
///        so the OFF build (`CONFIG_TIMER_SRV=0`, shipped default) is
///        byte-identical to the pre-#241 tree.

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace kernel::time {
namespace timer_srv {

#if CONFIG_TIMER_SRV

/// @brief Wire ops carried in TimerRpc::op (server dispatch switch).
enum class TimerSrvOp : uint8_t {
    BIND = 0,          ///< server announce / capability handshake
    CLOCK_GETTIME = 1, ///< CLOCK_MONOTONIC / CLOCK_REALTIME read
    TIMER_CREATE = 2,  ///< create a one-shot/periodic timer
    TIMER_SETTIME = 3, ///< arm/disarm an existing timer
    TIMER_DELETE = 4,  ///< destroy a timer
    TIMERFD_CREATE = 5,///< create an fd-backed timer
    TIMERFD_SETTIME = 6,///< arm/disarm a timerfd
    CANCEL_OWNERS = 7, ///< piggybacked tombstone drain (no reply expected)
};

/// @brief 48 B wire struct (fits CONFIG_IPC_FAST_PAYLOAD_BYTES).
struct TimerRpc {
    uint8_t op;         ///< TimerSrvOp
    uint8_t flags;      ///< bit0: DEGRADED (RTC unavailable, tick-derived)
    uint16_t rsvd;      ///< reserved (zero)
    uint32_t epoch;     ///< proxy epoch (server purges on increase)
    uint32_t owner_id;  ///< requesting task id
    uint32_t owner_gen; ///< requesting task generation
    uint64_t timer_id;  ///< timer handle (slot id + slot gen)
    uint64_t value_ns;  ///< expiry / now / interval (op-dependent)
    uint64_t interval_ns; ///< periodic interval (0 = one-shot)
    uint16_t clock_id;  ///< CLOCK_MONOTONIC / CLOCK_REALTIME
    int16_t status;     ///< reply: negated errno (0 = ok)
    uint32_t rsvd2;     ///< reserved (zero)
};

static_assert(sizeof(TimerRpc) <= 48,
              "TimerRpc must fit the 48 B fast IPC payload");

/// @brief Record the PID of the timer-srv daemon task.
void set_timer_srv_pid(uint64_t pid);
/// @brief Get the recorded timer-srv PID (0 when unset).
uint64_t get_timer_srv_pid();
/// @brief True when the server task is live (present, not TERMINATED/REAPED).
bool timer_srv_alive();
/// @brief Proxy epoch (bumped on restart / snapshot reset / boot table).
uint32_t timer_srv_epoch();
/// @brief Bump the epoch and purge tombstones (server restart path).
void timer_srv_bump_epoch();
/// @brief Supervisor hook: record a restart (store pid, bump epoch).
void on_restart(uint64_t pid);
/// @brief Teardown hook: latch a dead owner's id+generation (lock-free).
void note_dead_owner(uint32_t owner_id, uint32_t owner_gen);
/// @brief Generic RPC: validate server, bounded wait, validated reply.
/// @return true with *reply filled on a validated reply, false fail-closed
///         (server down, timeout, or reply mismatch) with no block left held.
[[nodiscard]] bool timer_srv_rpc(const TimerRpc &req, TimerRpc &reply);

/// @brief sys_clock_gettime proxy (called under CONFIG_TIMER_SRV only).
int64_t sys_clock_gettime_srv(uint64_t clock_id, uint64_t out_timespec_ptr);
/// @brief sys_nanosleep proxy (bounded wait; no signal-interrupt path yet).
int64_t sys_nanosleep_srv(uint64_t req_ptr, uint64_t rem_ptr);
/// @brief sys_timer_create proxy.
int64_t sys_timer_create_srv(uint64_t clock_id, uint64_t sevp_ptr,
                             uint64_t timerid_ptr);
/// @brief sys_timerfd_create proxy.
int64_t sys_timerfd_create_srv(uint64_t clock_id, uint64_t flags);

#endif // CONFIG_TIMER_SRV

} // namespace timer_srv
} // namespace kernel::time
