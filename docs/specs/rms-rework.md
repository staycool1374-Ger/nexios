# RMS Rework — Rate-Monotonic Schedule Lock Contention & Dispatch Plan

**Status:** live plan — Phase 1 + Phase 3 IMPLEMENTED, Phase 2 OPEN, Phase 4
DEFERRED (not implemented).
**Supersedes:** `docs/_archive/rms-rework-plan.md` (2026-07, removed 2026-11;
original text preserved in git history).
**Related:** `specs/scheduler.md` §8 (open gaps), `specs/deadline.md` §7
(non-goals), `specs/ipc.md` §6 (priority-ordered wakeup).

## Problem

The `ipc_kernel_block_skips_sti` test hung because `reschedule()` and
`rate_monotonic_schedule()` contended on `scheduler_lock_`. Two architectural
issues: (A) lock granularity — a read-only peek took the full spinlock;
(B) synchronous reschedule request — the lock was taken only to peek the
ready queue, which is informative but not strictly required.

## Phase 1 — Lock-free `reschedule()` [IMPLEMENTED]

`Scheduler::reschedule()` now uses `arch::IrqGuard` instead of the full
spinlock (`src/kernel/task/scheduler_tick.cpp:1123-1129`). Single-core UP:
a read-only peek needs only IRQ-safety, not `scheduler_lock_`. The atomic
release-store to `scheduler_need_resched` is safe outside the lock.

For SMP (future): replace `IrqGuard` with a dedicated `reschedule_lock_`
or a seqlock/RCU for the ready-queue peek.

## Phase 2 — Split `scheduler_lock_` into two locks [OPEN]

`scheduler_lock_` still guards all scheduler state (ready queue + `all_tasks_`
+ `id_table_` + deadline list). The split design (tracked in
`specs/scheduler.md` §8):

```
scheduler_lock_      — guards all_tasks_, id_table_, deadline_list_ (rare writes)
ready_queue_lock_    — guards ready_queue_ mutations only (frequent writes)
```

Ordering (both needed): `ready_queue_lock_` → `scheduler_lock_` (prevents
circular wait). The timer ISR `try_lock()`s `ready_queue_lock_` first; on
failure it skips only the dispatch decision while still doing other per-tick
work (deadline detection, sporadic replenishment).

Files: `src/kernel/task/scheduler.hpp` (new member),
`src/kernel/task/scheduler.cpp` + split files (lock usage),
`src/kernel/ipc/ipc.cpp` (`block_sender`/`wake_sender`).

## Phase 3 — Priority-ordered wakeup for blocked-sender chains [IMPLEMENTED]

Implemented as issue #106 Part A (2026-09-03), see `specs/ipc.md` §6:
`IPC::block_sender` inserts blocked senders in descending priority order
(`ipc.cpp:506-512`); `IPC::wake_sender` pops the head = highest priority
(`ipc.cpp:543-559`). Verified by `IpcPriorityOrderedWake` (ipc_robustness
class). This closes the FIFO-wakeup priority-inversion gap.

## Phase 4 — Deadline-pressure-aware dispatch [DEFERRED]

`deadline_rush` preemption (prefer a task with an imminent deadline over a
higher-priority task with none) is NOT implemented — recorded as a non-goal
in `specs/deadline.md` §7. Deadline-awareness stays post-facto detection
only. The design below is retained for a future cycle:

- Add `deadline_rush_` to `TaskControlBlock`, set when a deadline is within
  `CONFIG_DEADLINE_RUSH_TICKS`, cleared after the task runs.
- `needs_switch()` / `next_task()` preempt on rush even at equal/lower
  priority.

Files (if revived): `src/kernel/task/task.hpp`, `src/kernel/task/scheduler*.cpp`,
`src/kernel/nexios_config.h`.
