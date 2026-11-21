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
/// @brief Timer/Event externalization proxy (issue #241, strangler step a;
///        issue #194 flip). Kernel-side contract for the userspace timer-srv
///        PD: 48 B wire RPC, PID cell, epoch/liveness, anchor cell, tombstone
///        drain, and the sys_* proxy entry points used by the 80–83 handlers
///        when `CONFIG_TIMER_SRV=1`. Everything here is declaration-only;
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
    ANCHOR_PUSH = 8,   ///< anchor poll (kernel->server) / feed (rtc-pd->server)
    TIMERFD_EXPIRED = 9,///< timerfd expiry event (server->kernel fire)
    SLEEP_REQUEST = 10,///< nanosleep arm (one-way; fire event follows)
    SLEEP_CANCEL = 11, ///< cancel a pending sleep (best-effort, fail-closed)
    TIMER_GETTIME = 12,///< read remaining + period of a timer
};

/// @brief Anchor/reply flag bits (TimerRpc::flags, issue #194).
/// bit0 DEGRADED, bit1 RTC_NEVER (device absent), bit2 STALE_TOKEN.
inline constexpr uint8_t kTimerFlagDegraded = 1U << 0;
inline constexpr uint8_t kTimerFlagRtcNever = 1U << 1;
inline constexpr uint8_t kTimerFlagStaleToken = 1U << 2;

/// @brief Message type of every timer-srv async fire event (mirrors
/// userspace MSG_TYPE_REPLY; sync RPCs key on sender+epoch+owner, async
/// waiters additionally match this type before touching payload).
inline constexpr uint64_t kTimerSrvReplyType = 0xF0000002ULL;

/// @brief Wire version (issue #194 S2-4): kernel stamps BIND, the server
/// rejects a mismatched version with -EPROTO instead of guessing.
inline constexpr uint64_t kSrvWireVersion = 2;

/// @brief 48 B wire struct (fits CONFIG_IPC_FAST_PAYLOAD_BYTES).
struct TimerRpc {
    uint8_t op;         ///< TimerSrvOp
    uint8_t flags;      ///< anchor/reply flag bits (see kTimerFlag*)
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
/// MONOTONIC answers from the HRT clock (no RPC); REALTIME from the
/// anchor cell (no RPC once published; bounded poll only when invalid).
int64_t sys_clock_gettime_srv(uint64_t clock_id, uint64_t out_timespec_ptr);
/// @brief sys_nanosleep proxy: one-way arm + blocking fire wait.
/// No signal-interrupt path (carried #194 limitation); server death
/// wakes with -EINTR.
int64_t sys_nanosleep_srv(uint64_t req_ptr, uint64_t rem_ptr);
/// @brief sys_timer_create multiplex proxy (op, a1, a2, a3 = is_timerfd).
int64_t sys_timer_create_srv(uint64_t op, uint64_t arg1, uint64_t arg2,
                             uint64_t arg3);
/// @brief sys_timerfd_create proxy: RPC alloc; the vnode wiring stays in
/// the handlers translation unit (timerfd_ops lives there).
int64_t sys_timerfd_create_srv(uint64_t clock_id, uint64_t flags);

/// @brief Publish a wall-clock anchor (ns since epoch + mono ns at latch).
/// Lock-free; only advances (monotone guard), never steps back.
void publish_anchor(uint64_t anchor_ns, uint64_t mono_at_anchor_ns,
                    uint8_t flags) noexcept;
/// @brief REALTIME ns from the anchor cell (saturating); boot fallback
/// (RTC read) while unpublished.
uint64_t anchor_realtime_ns() noexcept;
/// @brief True once an anchor was published (or latched at boot).
bool anchor_valid() noexcept;
/// @brief Clear the anchor latch (rtc-pd restart path: forces a re-poll
/// on the next REALTIME read instead of serving a stale wall clock).
void anchor_invalidate() noexcept;
/// @brief Refresh the anchor cell with one bounded server poll (fail-closed:
/// keeps the old cell when the server is down).
void refresh_anchor() noexcept;

/// @brief One-way nanosleep arm; stamps a per-call sleep token.
/// @param value_ns Sleep budget ns.
/// @param out_token Receives the token the fire event must echo.
/// @return true when the request reached the server queue.
bool nsleep_send(uint64_t value_ns, uint64_t &out_token) noexcept;
/// @brief Block for the SLEEP_REQUEST fire event matching @p sleep_token.
/// @return 0 on fire, -EINTR on server death, -EAGAIN when no wait possible.
int64_t nsleep_wait(uint64_t sleep_token) noexcept;

/// @brief Block for the TIMERFD_EXPIRED event matching the vnode tag.
/// @return 0 + @p out_count on fire, -EAGAIN (dead server), -EBADF stale.
int64_t timerfd_deliver(uint32_t index, uint32_t slot_gen,
                        uint64_t &out_count) noexcept;
/// @brief Best-effort timerfd disarm (SETTIME value=0) before close;
/// fail-closed, slot teardown proceeds regardless.
void timerfd_disarm(uint32_t index, uint32_t slot_gen) noexcept;
/// @brief Single fire-event poll (O_NONBLOCK path): no park, no wait.
/// @return 0 + @p out_count on a queued matching fire, -EAGAIN otherwise.
int64_t timerfd_try_consume(uint32_t index, uint32_t slot_gen,
                             uint64_t &out_count) noexcept;

/// @brief Timer alloc/arm/read/delete RPCs (handlers do ABI copies).
int64_t timer_create_rpc(uint64_t clock_id, uint64_t owner_id,
                         uint32_t owner_gen, uint64_t &out_timer_id) noexcept;
int64_t timer_settime_rpc(uint64_t timer_id, uint64_t value_ns,
                          uint64_t interval_ns, uint64_t owner_id,
                          uint32_t owner_gen) noexcept;
int64_t timer_gettime_rpc(uint64_t timer_id, uint64_t owner_id,
                          uint32_t owner_gen, uint64_t &out_value_ns,
                          uint64_t &out_interval_ns) noexcept;
int64_t timer_delete_rpc(uint64_t timer_id, uint64_t owner_id,
                         uint32_t owner_gen) noexcept;
int64_t timerfd_create_rpc(uint64_t clock_id, uint64_t owner_id,
                           uint32_t owner_gen, uint32_t &out_index,
                           uint32_t &out_gen) noexcept;
int64_t timerfd_settime_rpc(uint32_t index, uint32_t slot_gen,
                            uint64_t value_ns, uint64_t interval_ns,
                            uint64_t owner_id, uint32_t owner_gen,
                            uint32_t &out_new_gen) noexcept;

/// @brief Test-isolation reset: epoch bump + tombstone clear (INV-T6;
/// the test_isolate.cpp call-site edit is QE-owned on testbed).
void snapshot_reset() noexcept;
/// @brief Lifetime tombstone drops past the 16-entry ring (normative
/// §8.4/§10 D6: counted and reported, never silent).
uint64_t tombstone_drops_total() noexcept;
/// @brief Cached BIND failure (0 ok/unbound, -EPROTO on wire mismatch,
/// -EAGAIN on transport): the proxies mask it as -EAGAIN per-call;
/// QE asserts this cell to tell the two apart.
int64_t bind_error() noexcept;

#endif // CONFIG_TIMER_SRV

} // namespace timer_srv
} // namespace kernel::time
