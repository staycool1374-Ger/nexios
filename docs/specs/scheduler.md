# Scheduler, Ready-Queue & Task-Lifecycle Specification

**Semantics:** canonical, binding contract for the single-core deferred-context-switch
scheduler.  This document is the deduplicated synthesis of the historical papers
`_archive/scheduler-spec.md`, `_archive/ipc_blocking-redesign-v1.md`,
`_archive/ipc_blocking-plan.md`, `_archive/phase1-peek-highest.md`,
`_archive/phase3-clear-stale-switch.md`, `_archive/task-lifecycle-review.md`,
`_archive/rms-rework-plan.md`, `_archive/task-scheduler-audit-fix.md` and
`zombie-list-spec.md`.  All "[IMPLEMENTED]" markers are code-verified in the current tree.

```
          timer ISR
              │  on_tick()
              ▼
      ┌─────────────────┐
      │ rate_monotonic_ │  try_lock → peek_highest → publish deferred switch
      │ schedule()      │  (load_rsp/save_rsp/kstack/cr3 + generation)
      └────────┬────────┘
               │ ISR epilogue (isr_stubs.asm): generation check → apply iretq
               ▼
      ┌─────────────────┐        ┌──────────────────────┐
      │ switch_to_task  │ ─────▶ │ ReadyQueueManager     │
      │ (owner-resolve, │        │ bitmap(0-127) + per-  │
      │  frame-validate,│        │ priority TaskQueues   │
      │  scratch-save)  │        └──────────────────────┘
      └─────────────────┘              ▲  peek_highest (non-destructive)
                                       │  dequeue_highest (commit/corrective)
                              task-context: block/wake/terminate → move_priority
```

## 0. Priority Convention (binding)

**Higher numeric priority value = higher scheduling priority.**

- Range 0–127 (`PriorityMap` = two `uint64_t` words; `find_highest_bit` → bit
  127 is highest, bit 0 lowest).
- Concretely: idle = 0; vfsd/iocd daemons = 20; test harness / init = 10 (during
  tests, 0 as background reaper); deadline-monitor = 127.
- **Sporadic-server background priority:** an EXHAUSTED server's
  `current_priority()` returns `bg_priority_`, which MUST be numerically LOWER
  than `base_priority_` AND lower than the harness (10).  (The historical
  `bg_prio=42` bug made an exhausted task outrank the test runner → the
  `ss_deadline` hang; fixed to bg_prio=2.)

```
  higher  ──▶ 127 deadline-monitor
                 20 vfsd / iocd
                 10 test harness (active) / init
                  2 shell / ss_deadline bg_prio
                  0 idle (and init as reaper)
  lower
```

## 1. Requirements (the contract)

- **R1 — Exactly one physical runner.**  At every instant exactly one task owns
  the live kernel stack (RSP ∈ `[kernel_stack, kernel_stack_top)`); resolved by
  RSP ownership, not by `current_task_ptr_`.
- **R2 — A deferred switch is applied exactly once, atomically, never to a freed
  stack.**  [IMPLEMENTED: generation-lock pair + apply-side RSP-owner check.]
- **R3 — Clean teardown.**  A task is freed only after (a) it is no longer the
  physical runner, (b) no deferred switch targets it, (c) peers waiting on its
  IPC are resolved.

## 2. Invariants

| Inv | Rule | Status |
|---|---|---|
| INV-1 | `current_task()` is RSP-authoritative (cache only) | binding |
| INV-2 | Single deferred-switch slot; exactly one writer (`switch_to_task`), exactly one applier (ISR epilogue) | binding |
| INV-3 | ISR epilogue applies the switch gated on nesting depth ≤ 2 | binding |
| INV-4 | Runnable set derived from `state`; ready queue is a **cache, not authority**. `next_task()` = `peek_highest()` + commit-dequeue | [IMPLEMENTED] |
| INV-5 | State-transition coherence: leaving runnable states ⇒ dequeue. `send_sync` is the historical exception (BLOCKED-in-runq, healed by the peek loop) | [PARTIALLY CHANGED] — see §5 |
| INV-6 | Every priority change must re-index the ready queue via `move_priority()` | [IMPLEMENTED] |
| INV-7 | Reaper/termination invariants (a-f): full unlink from RQ / AllTasks / DeadlineList / id_table / blocked_senders / current before free | binding |

### INV-4 — peek-based `next_task()` [IMPLEMENTED]

```
while (auto *candidate = ready_queue_.peek_highest()) {
    if (candidate == current || (state != READY && state != RUNNING))
        ready_queue_.dequeue_highest();   // corrective removal (stale head)
    else {
        ready_queue_.dequeue_highest();   // commit
        return candidate;
    }
}
return idle_task_;
```
Peek is non-destructive; stale-state heads are drained, never orphaned.  The
historical O(n) lazy-rebuild is dead code (removed).

### INV-6 — `move_priority()` call sites [IMPLEMENTED]

```
IPC::block_sender   q.owner->priority = task.priority   → move_priority (IrqGuard)
IPC::wake_sender    receiver.priority = max_prio        → move_priority (IrqGuard)
deadline DEMOTE     task->priority >>= 1                → move_priority (on_tick, lock)
on_tick sporadic    EXHAUSTED→ACTIVE (non-current)      → move_priority (lock+IrqGuard)
```

**Concurrency contract:** `t->priority` and `sporadic_server->state_` are plain
fields mutated by the timer ISR and by task context (IPC PI).  They MUST be
read/written inside `arch::IrqGuard` and/or `scheduler_lock_`;
`effective_priority()` takes an internal `IrqGuard` so the composite read is
consistent; `move_priority`'s `old/new` args must be computed in the same
IRQ-safe section (a stale `old_prio` desyncs the bucket).

## 3. Ready-Queue Architecture

```
ReadyQueueManager
├─ PriorityMap          two uint64_t words (prio 0..127), ctz/clz O(1)
├─ TaskQueue queues_[CONFIG_PRIORITY_CEILING+1]   intrusive per-priority lists
└─ peek_highest() / dequeue_highest() / enqueue_ready() / move_priority(old,new)
```

### 3.1 WEDGE detector (CONFIG_DEBUG, every tick)
- **blocked-in-runq** (BLOCKED/WAITING with `in_ready_queue_=1`): benign, filtered
  by the peek loop.
- **orphan** (READY/RUNNING with `in_ready_queue_=1` but not physically linked):
  HALT — deterministic evidence of stale flag / incomplete dequeue.

### 3.2 `rate_monotonic_schedule()` — disarm stale pending switch [IMPLEMENTED]
A pending switch superseded before the ISR applies it is **cleared** (all four
atomics + `next_task_id`), then scheduling proceeds; `next_task()` re-selects
immediately.  Dropped switch worst case = one tick of lag.  This removed the
"frozen switch window" that blocked the old lazy-rebuild.

## 4. Sporadic Server Interaction

```
              ┌──────────────────┐
              │     IDLE         │  current_priority() = base_priority_
              └────────┬─────────┘
                       │ on_activation()
                       ▼
              ┌──────────────────┐
        ┌────▶│     ACTIVE       │  current_priority() = base_priority_
        │     └────────┬─────────┘
        │              │ consume() → budget == 0
        │              ▼
        │     ┌──────────────────┐
        │     │   EXHAUSTED      │  current_priority() = bg_priority_  (< base, < 10)
        │     └────────┬─────────┘
        │              │ process_replenishments()
        └──────────────┘
```
Priority-change re-indexing table (INV-6) and `on_tick` budget management
(`process_replenishments` / `consume` / `reschedule`) are gated inside
`if (lock_acquired) { arch::IrqGuard ... }` — they mutate the ready queue,
sporadic state and zombie list that task context also touches.

## 5. IPC Ready-Queue Interaction

| Op | Effect on ready queue | INV-5 conformance |
|---|---|---|
| `IPC::send` (full, blocking) | `block_sender()` → state=BLOCKED + `dequeue_ready` | conformant |
| `IPC::send` (wake dest) | `set_task_ready(dest)` → READY + `enqueue_ready` | conformant |
| `IPC::recv` | pop + `wake_sender()` → `set_task_ready` | conformant |
| `IPC::send_sync` | state=BLOCKED, **dequeue then block** | **CHANGED** |

**[CHANGED] send_sync dequeue (ipc.cpp:301):** `send_sync` now calls
`dequeue_ready(*cur)` immediately before `state = BLOCKED` (VULN-IPC-03,
`_archive/ipc-sync-audit-fix.md`).  This supersedes the historical "BLOCKED
without dequeue" INV-5 exception documented in `_archive/ipc_blocking-redesign-v1.md`;
the ready-queue membership table in that paper is stale on this row.

## 6. Task Lifecycle — Ready-Queue Membership

```
 create+add_task ──▶ READY  in_rq=1 ──▶ RUNNING (dequeued by dispatch)
      │                                     │
      │                                 preempt ──▶ READY in_rq=1 (set_current re-enqueue)
      ▼                                     │
  TERMINATED (terminate → dequeue)          ▼ block
      │                                 BLOCKED in_rq=0 (dequeue_ready)
      ▼
  ZOMBIE (cleanup → remove/unregister)     WAITING in_rq=0
      │
      ▼
  MemPool::free (poison 0xDD by MemPool::free only)
```
**D5 contract:** `magic` stays `TCB_MAGIC` until `remove_task()`/`unregister_task()`
fully unlinks the node; poison is applied ONLY by `MemPool::free()`.
**D6 contract:** every termination path uses the canonical sequence
`cleanup() → remove_task()/unregister_task() → MemPool::free()`.

See `zombie-list-spec.md` for the zombie/reaper lifecycle detail.

## 7. Snapshot/Restore Ready-Queue Handling

1. `snapshot_create()` captures `ReadyQueuePOD` (heads/tails/counts/bitmap).
2. `restore_state()`: `restore_pod()` validates pointers; `rebuild_ready_queue()`
   re-enqueues READY tasks from `all_tasks_` with fresh `effective_priority()`.
3. `sporadic_server` pointer cleared on restore.

**Key consequence:** snapshot rebuild heals all ready-queue desyncs (stale
priorities, orphaned flags, dangling pointers).

## 8. Open Gaps / Follow-ups

- **`isr_nesting_depth` → per-CPU asm** (GS/TPIDR-relative) — deferred to SMP
  (Phase 5, ROADMAP §0.4.1).
- **`hhdm_modified_` (VAR-17)** — single-core safe today; re-audit under SMP.
- **Lock split (rms-rework Plan Phase 2):** `ready_queue_lock_` → `scheduler_lock_`
  ordering and blocked-sender wakeup ordering — not confirmed implemented.
- **SCHED-007:** non-nullable APIs `TaskControlBlock*` → `TaskControlBlock&`
  (incremental UAF hardening) — not yet landed.

## 9. Observability — cpuinfo/top (issue #172)

Read-only introspection for the shell monitor; no dispatch-path changes
beyond one placement store:

- **`running_on_cpu`** (TCB field, `CPU_PLACEMENT_NONE` = never dispatched):
  single writer — the dispatching CPU stores `sched_cpu()` (release) at
  the `switch_to_task` commit point (`next.state = RUNNING`) and in the
  `set_current_task` adoption path; shell readers use acquire loads.
  Never used for dispatch decisions (affinity/balancer own placement).
- **Load averages** (`loadavg_1min_/5min_/15min_`): integer EMA of the
  per-tick non-idle sample, updated on the BSP `on_tick()` tail under
  the already-held `scheduler_lock_` (additive only, no alloc/log);
  `N = 60/300/900 × CONFIG_TICK_HZ`.  Stored at ×1000000 internal
  precision (a ×1000 quantum truncates every per-tick step to zero);
  accessors divide by 1000 back to per-mille for display.  Tickless
  windows skip decay (stale, bounded).  Readers via atomic accessors.
- **Zombie snapshot** (`snapshot_zombies(out, max)`): copies list entries
  under IrqGuard + zombie leaf (drain_zombie_list precedent, read-only —
  no surgery, no free); shell prints lock-free from the snapshot with
  magic + TERMINATED/REAPED filtering.
