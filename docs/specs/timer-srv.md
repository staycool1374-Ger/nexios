# timer-srv — Timer/Event Externalization Proxy (issue #241)

Strangler step a (no flip): userspace timer-srv PD + kernel proxy behind
`CONFIG_TIMER_SRV` (default 0 = today's in-kernel wheel, bit-identical).

## Wire

48 B `TimerRpc` (`timer_srv.hpp`, fits `CONFIG_IPC_FAST_PAYLOAD_BYTES`):
op/flags/rsvd/epoch/owner_id/owner_gen/timer_id/value_ns/interval_ns/
clock_id/status/rsvd2. Ops: BIND, CLOCK_GETTIME, TIMER_CREATE, TIMER_SETTIME,
TIMER_DELETE, TIMERFD_CREATE, TIMERFD_SETTIME, CANCEL_OWNERS.

## Lifecycle / epoch / tombstones

- PID cell (`set/get_timer_srv_pid`); `alive()` = present and not
  TERMINATED/REAPED.
- Epoch bumps on `on_restart`, snapshot reset, boot table; server purges all
  slots on epoch increase (fail-closed reset).
- Teardown path (`TaskControlBlock::cleanup`) only appends (owner_id, gen) to
  a bounded (16) lock-free tombstone ring; the next proxy RPC piggybacks
  CANCEL_OWNERS. Ring-full drops oldest (bounded residual ≤ table size).
- Replies validated on (server pid, epoch, owner id+gen, timer id+slot gen);
  mismatch is dropped with a bounded re-wait, never consumed.

## Bounded wait

`IPC::send_sync_bounded(dest, msg, reply, timeout_ticks)`: mirrors
`send_sync` exactly (reply matching, dest-death bail, BLOCKED +
`dequeue_ready` + `reschedule`, hlt wait) plus `recv_wait_arm`/
`recv_wait_cancel` with guard `kRpcGuardTicks` (5000). Expiry returns
false (`-EAGAIN` at the syscall) — never a silent success, never a held
block. No IRQ window widened vs `send_sync`.

## Pre-flip status

80–83 handlers delegate to `-ENOSYS` stubs when ON (shape proof, not
behavior); the flip (real server behavior, EINTR path, timerfd wiring)
is a later milestone with the §7 gate (audit + test-full debug AND
release green + delta-0). OFF (default) keeps the in-kernel registry.
