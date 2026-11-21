# timer-srv — Timer/Event Externalization Proxy (issue #194)

Increment 1 (landed, `CONFIG_TIMER_SRV` default 0): the userspace timer-srv
PD serves the full deadline engine and `time/timer_srv.cpp` proxies it.
The in-kernel POSIX registries stay compiled (OFF leg) until QE ports
`test_posix_time`; increment 2 deletes the OFF bodies (close criteria:
POSIX provably out of kernel + `srv_timer` 3/3 + `timing_core` green on
the 2×3 release matrix).

## Wire

48 B `TimerRpc` (`timer_srv.hpp`, fits `CONFIG_IPC_FAST_PAYLOAD_BYTES`):
op/flags/rsvd/epoch/owner_id/owner_gen/timer_id/value_ns/interval_ns/
clock_id/status/rsvd2. Wire v2 ops: BIND(0), CLOCK_GETTIME(1),
TIMER_CREATE(2), TIMER_SETTIME(3), TIMER_DELETE(4), TIMERFD_CREATE(5),
TIMERFD_SETTIME(6), CANCEL_OWNERS(7), ANCHOR_PUSH(8), TIMERFD_EXPIRED(9,
server→kernel fire only), SLEEP_REQUEST(10), SLEEP_CANCEL(11),
TIMER_GETTIME(12). `static_assert(sizeof(TimerRpc) <= 48)` both sides;
kernel stamps `kSrvWireVersion = 2` in BIND, the server rejects mismatch
with `-EPROTO` (fail-closed, never guesses).

Flags: bit0 DEGRADED (RTC unavailable, tick-derived), bit1 RTC_NEVER
(device absent — riscv no-goldfish refuses the mtime-1970 lie), bit2
STALE_TOKEN (named slot missed). Async fire events use message type
`0xF0000002` (userspace `MSG_TYPE_REPLY`); sync RPCs key on
sender+epoch+owner, async waiters additionally match type + token.

## Lifecycle / epoch / tombstones

- PID cell (`set/get_timer_srv_pid`); `alive()` = present and not
  TERMINATED/REAPED.
- Epoch bumps on `on_restart`, snapshot reset, boot table; server purges
  all slots on epoch increase (fail-closed reset). BIND re-runs per epoch
  (version gate) and re-rendezvouses rtc-pd (`pd_bind_rtc` +
  `pd_rtc_push_epoch`, task context only — restart hooks run in tick).
- Teardown path (`TaskControlBlock::cleanup`) only appends (owner_id, gen)
  to a bounded (16) lock-free tombstone ring; the next proxy RPC drains
  it one-way (`CANCEL_OWNERS`, NONBLOCK, no reply — the server sends none,
  so a bounded wait would only burn the guard and litter the client
  queue). Ring-full drops the NEWEST (code is truth) and counts
  `tombstone_drops_total_` (§8.4/§10 D6: counted and reported).
- Replies validated on (server pid, epoch, owner id+gen, timer id+slot
  gen); mismatch is re-queued (never eaten) or dropped at the INV-T5
  site, never consumed.
- Timer table bounds mirror each other (`CONFIG_TIMER_SRV_MAX_TIMERS` 50 /
  `_MAX_TIMERFDS` 32 = server `TIMER_SRV_MAX_*`); mismatch is a BIND
  `-EPROTO`, never an index overflow.

## Wake model (server)

Fire-due FIRST on every wake (including spurious), then the wait:
`timeout_ticks = ceil((earliest - now) / ns_per_tick)` with overflow-safe
ceil (`(a/b) + ((a/b) != 0)`), clamped to `[1, kMaxWaitTicks]`
(`kMaxWaitTicks = 4000`). `timeout_ticks == 0` means block-forever, so a
due deadline with no further traffic would hang server + client forever
if 0 were passed for a live deadline — the clamp floor is 1 exactly for
that case; idle (no deadline) blocks forever and is purely
message-woken. Boot tick-rate calibration is bounded (64 iterations,
division-by-zero guarded → 1 ms/tick + DEGRADED).

## Cancellation exactness (INV-T5)

Every server-issued client event carries
`token = (owner_id, owner_gen, slot, slot_gen, epoch)`. The kernel drops
any event whose token does not match the live registration before waking
anybody: the vnode carries the server-issued `(slot, slot_gen)` opaquely
in its tag (never a kernel deadline table — INV-T2 holds);
`timerfd_read_entry` compares event token vs tag (stale reuse and
post-delete in-flight events die there). A freed slot bumps `slot_gen`
(timerfd disarm also bumps and returns the new token; the kernel
re-tags the vnode before re-arming). Plain-timer disarm does not bump
(the opaque handle stays re-armable; plain timers have no async
in-flight events). `timer_delete`/close frees the registration first.

Timerfd expiry is deliver-and-clear per period: each expiry is delivered
exactly once; periods that pass between reads arrive as one fire per
period (counts split across reads, never lost, never doubled).

## Anchor (wall clock)

`rtc-pd` feeds the server (`ANCHOR_PUSH`: value_ns = wall ns,
interval_ns = mono ns at read, flags = `RTC_NEVER` when the device is
absent). The server latches monotone (backward samples hold + count).
The kernel answers REALTIME/`SYS_GETTOD` from its lock-free anchor cell
(saturating `anchor + (mono - mono_at_anchor)`), wait-free once
published; boot fallback (one RTC read) covers early boot. The cell
refreshes by poll-on-stale: every sync proxy RPC refreshes while
invalid, and REALTIME/GETTOD poll while invalid (bounded, fail-closed);
`rtc-pd` restart invalidates the cell (re-poll on next read). No client
ever blocks on a wall-clock read; no per-tick broadcast exists.
MONOTONIC answers from the HRT clock directly (no RPC — this is also
what lets the server itself call `clock_gettime` without recursing).

Degraded mode: no anchor (or `RTC_NEVER`) serves REALTIME as
tick-derived ns-since-boot with DEGRADED set; a later anchor push clears
it and resyncs forward monotonically (never backwards).

## Bounded wait

Sync RPCs: `IPC::send_sync_bounded(dest, msg, reply, timeout_ticks)` with
guard `kRpcGuardTicks` (5000). Expiry returns false (`-EAGAIN` at the
syscall) — never a silent success, never a held block.

Async waits (`nsleep_wait`, `timerfd_deliver`): BLOCKED + dequeue +
reschedule (§11.1/§11.2), no locks held, no kernel timer. Wakers: (a)
the server's expiry message (arrival wakes via `blocked_in_recv`, never
cleared by send; Dekker recheck under the publish guard), (b) server
death (death-wake via `reply_wait` tags → `-EINTR` for sleep, `-EAGAIN`
for timerfd reads). Stale queue traffic is re-queued, never eaten;
generation-tagged death checks close the recycle window.

Carried limitation (explicit, QE-pinned): signal-EINTR for recv-parked
sleepers has no pending-signal probe under ON (the sleeper parks with
`sleep_armed == false`, unreachable by delivery wakes). A signal
arriving mid-sleep is deferred until the sleep completes; server death
still wakes with `-EINTR` (with `rem` reported).

## What stays in kernel (decided)

Syscall 33 ALARM stays: per-task tick-counter signal delivery is
kernel-internal; only the ALARM sleeper-wake leg goes dead under ON
(no sleeper sets `sleep_armed` there anymore). `PosixTime::reapply_wakes`
is a no-op stub under ON (the `scheduler_tick.cpp` scan stays but finds
nothing — bounded reads, fence intact). The event-timer wheel stays for
kernel-internal waits only (bounded IPC receives, delivery timeouts,
watchdog); the ON leg's only `arm()` callers are those paths.

## Pre-flip → increment-1 status

80–83 + 34 handlers delegate unconditionally under `CONFIG_TIMER_SRV`
(no runtime dual path); the OFF bodies stay behind `#if` (both legs
green) until the QE port lands. `test_isolate.cpp` `snapshot_reset`
hookup is QE-owned (`testbed`); dev ships `timer_srv::snapshot_reset`
unconnected-but-specified.
