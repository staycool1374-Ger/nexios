# Asynchronous Task-Death Notification — Crash Supervisor (issue #105 Part B)

## Purpose

Let a user-space supervisor detect when a server task dies (clean exit or
crash), learn *which* task died and *why*, reclaim the dead task's leaked
capabilities, and restart the failed server.  This is the kernel-side death
notification mechanism behind the "Crash Supervisor" fault-isolation story of
issue #105; the External Pager protocol (issue #105 Part A) is explicitly out
of scope and deferred to a separate design paper.

## Motivation & Non-Goals

- **Motivation:** the existing `daemon_mgr` restart is kernel-internal and only
  covers the two system daemons.  User-space supervisors need the same
  catch-crash / reclaim / restart loop for arbitrary servers.
- **Non-goals:** no external pager (#105 Part A), no `daemon_mgr` change, no
  signal-handler semantics change, no change to the #106 shared-memory ring
  itself (this mechanism is the *wakeup trigger* that pairs with it).

## Syscall ABI

Numbers 66–68 (all NON-FAST — they take the registry lock and DEATH_RECV writes
user memory; see syscall-fastpath.md §4 discipline).

| Syscall | arg0 | arg1 | Return |
|---|---|---|---|
| `SYS_DEATH_WATCH` (66) | watched pid | supervisor pid (0 = caller) | 0 on success, -1 on invalid pair / not authorized / registry full |
| `SYS_DEATH_RECV` (67) | `DeathRecord*` out | — | 1 (record copied), 0 (none pending, never blocks), -1 (bad pointer) |
| `SYS_DEATH_UNWATCH` (68) | watched pid | — | 0 |

### `DeathRecord` layout

```c
struct DeathRecord {
    uint64_t dead_id;    // task that died
    uint64_t exit_code;  // exit code, or sign-extended signal on crash
    uint64_t flags;      // DEATH_FLAG_SIGNAL = 1 on abnormal (crash) death
};
```

`DEATH_FLAG_SIGNAL` is set when `exit_code` has its high bit set — the kernel's
convention for signal-terminated tasks (e.g. `-SIGSEGV`).  The supervisor
distinguishes "clean server exit" from "crashed" without parsing process
semantics.

## Registry Semantics (`ipc/death_notify.{hpp,cpp}`)

- Static bounded table, `CONFIG_CAP_MAX_DEATH_WATCHES` (16) slots — no dynamic
  allocation on RT paths (CODING_STYLE §4).  Registration **fails closed** when
  full.
- Slot lifecycle: `ACTIVE` (watch installed) → `PENDING` (watched task died,
  record latched) → `FREE` (supervisor consumed via recv, or drain freed it).
- **Exactly-once:** records latch only from `TaskControlBlock::cleanup()`
  (the single death funnel, REAPED + magic idempotency guards).  A task that
  dies through self-exit, signal-death, or daemon teardown produces exactly one
  record; repeated `drain_zombie_list()` cannot duplicate it.
- **Asynchronous, non-blocking:** `on_task_death` never blocks the dying task's
  cleanup; `SYS_DEATH_RECV` never blocks the supervisor.  A crashed or stalled
  supervisor can never wedge the kernel.
- **Authority:** a watch is installed only by (a) the supervisor for the watched
  task, or (b) the watched task designating a supervisor.  A third party cannot
  register (worst case a spurious wakeup pulse — the mechanism is info-only,
  no privilege transfer).
- **Supervisor-alive safety:** every slot stores the supervisor's `(id,
  generation)`.  The poke goes through the supervisor's own `Notify::notify()`
  which already guards `TERMINATED`/`REAPED`/generation — a poke to a just-died
  supervisor is a safe no-op.  `drain_task` in the supervisor's `cleanup()`
  frees its ACTIVE + PENDING slots before its `Notify` is destroyed.

### Lock ordering

`DeathNotify::s_lock_` is a leaf SpinLock.  Supervisors are collected under the
lock and poked **outside** it (a poke can reach the scheduler).  `s_lock_` is
never held while acquiring `scheduler_lock_` or a `Notify` lock, so the single
direction `scheduler_lock_ → s_lock_` (on_tick → flush_zombies → cleanup →
on_task_death) forms no cycle.  As with the other per-object locks taken in
`cleanup()`, `s_lock_` is acquired in that ISR-reachable path; a preempted
holder could in theory make a zombie-watchdog ISR spin on it, but no deadlock
by ordering is possible.

## Supervisor Protocol

1. `SYS_DEATH_WATCH(server_pid, 0)` — the supervisor registers itself for the
   server it manages.
2. Block on the supervisor's `Notify` (e.g. `sys_notify_wait`), or poll
   `SYS_DEATH_RECV`.
3. On wakeup pulse (`DEATH_WAKE_PULSE`), drain `SYS_DEATH_RECV` in a loop until
   it returns 0 — the pulse is a wakeup only; the authoritative data (and any
   fan-in of multiple deaths) lives in the registry.
4. Reclaim: the dead task's capabilities are already released by the kernel
   (`release_all_objects` → CNode dispose → FrameCap/Endpoint/etc. dispose).
   Any shared-memory ring frames are reclaimed through the existing
   `FrameUserMap::drain_task` (#106 closure) before `free_user_pages`.
5. Restart the server; re-arm the watch.
6. `SYS_DEATH_UNWATCH` is available to tear a watch down cleanly.

### Register/death race

`watch()` re-validates both tasks alive under the registry lock at install
time, so a watch on a task that died moments earlier is rejected (`-1`).  There
is a microscopic window where a task dies *just after* the liveness check and
the watch is installed for a task that is already reaping; a supervisor SHOULD
treat a never-firing watch as "server pre-died" and poll once after a failed
restart before assuming the kernel lost the record.

## Pairing with the #106 Shared-Memory Ring

The death pulse is the recovery trigger when a ring producer/consumer dies: the
supervisor wakes, drains the death record, and reclaims the ring frames through
its own `FrameCap` after the kernel's `FrameUserMap::drain_task` has already
cleared the dead task's PTEs.  The ring itself is untouched by the death path.

## Edge Cases & Failure Policy

- **Supervisor death while notifications pending:** its slots (ACTIVE +
  PENDING) are freed by `drain_task` in its cleanup; pokes are safe no-ops.
- **Registry full:** new registrations fail closed (`-1`); the supervisor must
  drain pending records before registering more.
- **Unwatch before death:** the watch is removed; the eventual death produces no
  record.
- **Exactly-once:** guaranteed by the cleanup() single funnel + the atomic
  ACTIVE→PENDING transition under the lock.

## Invariants

1. Every task death produces at most one record per watching supervisor.
2. `on_task_death` never blocks; `SYS_DEATH_RECV` never blocks.
3. A slot is freed exactly once (consume, unwatch, or drain — never both).
4. No poke dereferences a recycled supervisor TCB (id + generation revalidated
   at poke time).
5. ResourceTracker `death_watches` is zero-delta across every test.

## Test Plan (landed — class `cap_death`, 9 TF_KERNEL)

| Test | Asserts |
|---|---|
| `death_watch_consume_roundtrip` | watch → terminate(42) → one record, dead_id, exit_code, no SIGNAL |
| `death_watch_crash_reason` | signal termination → high-bit exit_code + DEATH_FLAG_SIGNAL |
| `death_watch_multi_fan_in` | 3 deaths → 3 records, distinct ids, baseline restored |
| `death_watch_after_death_rejected` | watch on reaped task fails closed |
| `death_watch_supervisor_death_drains` | supervisor death frees ACTIVE + PENDING slots |
| `death_watch_registry_full_fails_closed` | exhaustion fails closed; consume frees a slot |
| `death_watch_exactly_once` | one death → one record under repeated drains |
| `death_watch_unwatch_before_death` | unwatch suppresses the notification |
| `death_recv_nonblocking_none` | empty recv returns 0 immediately |