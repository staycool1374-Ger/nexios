# Scheduler, Ready-Queue & Task-Lifecycle Specification

**Semantics:** canonical, binding contract for the single-core deferred-context-switch
scheduler.  This document is the deduplicated synthesis of the historical
scheduler papers (scheduler-spec, ipc_blocking redesign/plan, phase1/phase3
fix-records, task-lifecycle review, task-scheduler audit fix-spec) whose
originals were removed 2026-11 after extraction — see git history for the
source papers.  The still-open RMS work lives in `specs/rms-rework.md`.
All "[IMPLEMENTED]" markers are code-verified in the current tree.

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
- **R4 — Bounded lower-level liveness (issues #242, #598).**  Strict RMS must
  not deny every lower level forever when the top READY level never drains.
  The breaker observes the lowest waiting non-idle band and forces one
  dispatch of it per `kStarvationBreakerTicks` (= 20) denied ticks (§3.3);
  worst-case interference is one forced lower tick per 20 denied ticks.

## 2. Invariants

| Inv | Rule | Status |
|---|---|---|
| INV-1 | `current_task()` is RSP-authoritative (cache only) | binding |
| INV-2 | Single deferred-switch slot; exactly one writer (`switch_to_task`), exactly one applier (ISR epilogue) | binding |
| INV-3 | ISR epilogue applies the switch gated on nesting depth ≤ 2 | binding |
| INV-4 | Runnable set derived from `state`; ready queue is a **cache, not authority**. `next_task()` = `peek_highest()` + commit-dequeue | [IMPLEMENTED] |
| INV-4a | `reschedule()` never returns to a still-BLOCKED caller: a BLOCKED caller hlt-waits (timer wakes every tick) until dispatched instead of running on while BLOCKED+dequeued (F1 #545) | [IMPLEMENTED] |
| INV-4b | A task with `state == BLOCKED` takes **no spinlock**.  The dequeue + BLOCKED marking + (where needed) unlock happen as one atomic IF=0 step; loops gate lock-taking paths on `state != BLOCKED` (F1 #545) | [IMPLEMENTED] |
| INV-APPLY-1 | An ISR epilogue may apply a deferred switch only when the interrupted context holds no `scheduler_lock_` take (`scheduler_lock_depth_own() == 0` AND `scheduler_lock_held() == false`; both conjuncts required). Fail direction: defer, never apply, never cancel (#582) | [IMPLEMENTED] |
| INV-APPLY-2 | Deferral is non-destructive and tick-deadline bounded: >=1 tick warns once, >=8 ticks with the lock held fails closed (dump + panic), >=8 ticks with the lock free re-arms (SCHED+19). No arm is ever cancelled on a deferral streak (#582) | [IMPLEMENTED] |
| INV-APPLY-3 | Deny is absolute; `force_apply` (terminate-driven) is a selector to attempt an apply, never an override. On deny the arm AND the force bit are retained (deny-and-rearm) (#582) | [IMPLEMENTED] |
| INV-PARK-2 | No context owning a `scheduler_lock_` take may enter either park; a BLOCKED runner that was not parked is latched (`SelfLock` = holds the lock, `ExternalDenial` otherwise — class is diagnostic-only). Service is hook-at-release for SelfLock in plain task context, tick-only otherwise; uncovered 8 consecutive instants fail closed (SCHED+17) (#582) | [IMPLEMENTED] |
| INV-RQ-1 | `in_ready_queue_`, `runq_next_`, `runq_prev_` and `rq_priority_` are written only inside `TaskQueue`, each mutation one IRQ-atomic unit. At every ISR boundary each TCB is physically linked in exactly one level queue, except the documented `ready_deferred_` current-task deferral (#582) | [IMPLEMENTED] |
| INV-RQ-2 | Every enqueue refusal is typed (`Linked`/`AlreadyQueued`/`RefusedInvalid`) and reported; the TCB is byte-identical. No caller sets `in_ready_queue_`/`rq_priority_` itself (#582) | [IMPLEMENTED] |
| INV-RQ-3 | `orphan_rescue_count_`, `stuck_rescue_count_`, `runq_link_flag_desync_count_`, `runq_nowhere_linked_count_`, `edf_orphan_rescue_count_` and `edf_chain_drop_count_` are the violation oracles; all read 0 in a healthy run (#582, #600) | [IMPLEMENTED] |
| INV-RQ-4 | Every READY task owned by this CPU and member of neither the bitmap level for its effective priority nor the EDF chain is rescued within one tick, irrespective of `edf_eligible` (#600: tick sweep + restore rebuild route through `enqueue_ready`) | [IMPLEMENTED] |
| INV-RQ-5 | `edf_peek_valid_own` terminates in ≤ `STRANDED_RESCUE_SCAN_MAX` iterations; a non-membership-marked head is force-dropped, a magic-bad head fails closed (head nulled, links never adopted), a bound hit nukes the chain fail-closed (#600) | [IMPLEMENTED] |
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

### INV-4a — reschedule() park contract (F1 #545) [IMPLEMENTED]

`reschedule()` only arms the deferred switch (INV-4) and historically
returned immediately — a caller that marked itself BLOCKED kept executing
while BLOCKED+dequeued until the tick switched it.  Any lock taken in that
zombie window stranded permanently when the tick-tail direct switch (which
has no INV-C1 check) preempted mid-critical-section.  The park closes the
window: when the caller is BLOCKED and the park gate holds, `reschedule()`
hlt-waits (every tick wakes the hlt) until the caller is dispatched.

Park gate (all must hold; otherwise status-quo-ante arm-only return):
`CONFIG_RESCHED_PARK_ON_BLOCKED==1`, `CONFIG_TICK_HZ!=0`,
`irq_guard_depth_own()==0`, no held tracked spinlock, not a hardware ISR
(`arch::isr_context_park_denied(entry_if)` is false, i.e. `isr_nesting==0`
**or** in syscall-trap level **or** IF==1 — interrupt gates clear IF on all
three arches, and the riscv64 nesting count is a known stale baseline,
#583), IRQ entitlement (entry IF==1 or trap level), current valid + BLOCKED
+ non-idle + multi-task guest, not the test harness (plus RSP-inside-TCB-stack
impersonation guard).

Handover baseline (issue #589, owner option (a)): exactly one boot write
records the firmware-handover nesting value via
`kernel::gs::try_set_isr_nesting_boot_baseline()` (`WriteClass::BOOT_ONLY`)
at the last zero-outstanding-frame point in `higherhalf_entry`
(single-threaded, SIE==0, right after the per-arch `percpu_init_bsp`
blocks); a read-only `[NEST]` tripwire at `init_task_main` entry prints a
nonzero first-task-context depth plus the task census for post-handover
diagnosis. Per-arch authority: x86_64 counts in the per-CPU slot
(gs:0x20, asm-owned); riscv64/aarch64 count the plain global, while their
PerCpu `isr_nesting_depth` (0x20) slots are layout parity only — on rv64
the handover normalizer writes the plain global to 0, on x86_64/aarch64
it is verify-only (no store).

Caller contract: C1 RUNNING/READY callers return immediately (zero
behavioral change); C2 BLOCKED+gated callers return only when no longer
BLOCKED; C3 BLOCKED+denied callers get the old arm-only return; C4 IRQ
state at return == entry, no scheduler_lock_, no allocation;
C5 single-task/null/no-tick/idle/harness never park; C6 the arm and
dispatch steps are untouched.  The post-publish rechecks in
`send_sync[_bounded]` and `sys_receive`/`recv_clamped` run as try-once
probes INSIDE the publish `IrqGuard`, before the BLOCKED marking
(a post-marking take strands on tick-tail preemption with no INV-C1
warning, since the check lives only on the reschedule path).

### INV-4b — no lock while BLOCKED (F1 #545) [IMPLEMENTED]

A task with `state == BLOCKED` takes **no spinlock**.  The dequeue +
BLOCKED marking + (where needed) unlock happen as one atomic IF=0 step;
every loop that can observe BLOCKED gates its lock-taking paths on
`state != BLOCKED` and falls through to `reschedule()` + `hlt`-wait.
`block_sender` releases under the caller's IF=0 guard via
`ParkLockGuard::release()`.  A `Contended` try-once probe proceeds to
block (the holder's wake-gate read observes the publish, so no retry
is needed and no IF=0 spin ever happens).

F2 (follow-up, not fixed here): the tick-tail direct switch still has no
INV-C1 check — diagnosis-only counter proposed, decision logic unchanged.

### INV-WAIT-3/4/5/6 — waitpid missed wake (issue #590) [IMPLEMENTED]

Four cooperating rules close the scan-to-publish missed-wake window (a
terminator that marks TERMINATED between the waiter's scan and its
publish):

- INV-WAIT-3 (fence pair): `child.state = TERMINATED` is separated from
  that path's read of `parent.waiting_child_pid` by SEQ_CST, and the
  waiter's publish is separated from its re-verification read by SEQ_CST.
  Both fences are required (store buffering would let both sides miss on
  weak models; release/acquire alone does not forbid it).  Four fence
  sites: `wake_waiting_parent` (covers all four funnel callers:
  `terminate_err`, `terminate_err_from_trap`, the deadline KILL arm and
  the watchdog KILL arm), `sys_exit`, `sys_thread_exit`, `sys_waitpid`.
- INV-WAIT-4 (publish order): `waiting_child_status → dequeue_ready →
  state=BLOCKED → waiting_child_pid`, pid last.  Only
  `wake_waiting_parent` re-queues on `state==BLOCKED`; `sys_exit` readies
  on `state != TERMINATED` and `sys_thread_exit` has no BLOCKED gate —
  but pid-last makes every pid observation imply BLOCKED, so the reorder
  is safe at all three wake sites.
- INV-WAIT-5 (corpse ownership): every wake site sets
  `child.wait_collect_pending_` at the match point (single writer: the
  terminator, never the waiter).  A flagged corpse is never freed by
  `flush_zombies`, `drain_zombie_list`, `cleanup_step[_try]` or the
  deferred-kill destroy while `find_task(child.parent_id)` lives and is
  not TERMINATED.  Exactly two frees: the owner's waitpid collect, or
  the owner-death escape once the parent leaves the tables.
- INV-WAIT-6 (consumption precedence, production paths only): after its
  fence, a waiter that no longer observes its own pid was consumed by a
  wake and must resolve via the normal collect — never undo, never −1,
  never block.

Deliberate divergence from the locked post-publish rechecks at lines
131-135 (`send_sync[_bounded]`, `sys_receive`/`recv_clamped` run their
rechecks inside the publish `IrqGuard`, before the BLOCKED marking): the
waitpid publish is lock-free (no guard exists on that path), so the
re-verify runs after the fence instead of under a lock.  The second
residence is `Scheduler::collect_zombie_child` (pop+claim under the
zombie leaf, owner tail outside).  Residual R-1 (terminator missed the
publication AND a concurrent drainer reclaimed the corpse first) denies
loudly with SCHED+23 + −1 — never a hang, leak or UAF.  Fourth death
path implemented (issue #593): the sys_kill self-fatal leg runs the trap
funnel (dequeue, TERMINATED, release_zombie) then arms the deferred
switch, and the wake moved after `release_zombie` so no parent can free
a current corpse; `collect_zombie_child` + `sys_waitpid` spare
`is_current_on_any_cpu`.

### INV-APPLY-1/2/3 + INV-PARK-2 — issue #582 [IMPLEMENTED]

Two intertwined defects: (B) a task suspended while holding
`scheduler_lock_` freezes the scheduler (the timer path needs the lock);
(A) READY + `in_ready_queue_==1` + linked-nowhere from a non-atomic
flag/links write. Sequencing is mandatory: B first (turns A's silent
freeze into a counted rescue), then A.

Apply gate (`scheduler_validate_pending_switch`, return ABI 1 = apply,
0 = abort/clear, 2 = defer): deny is fail-closed toward defer — never
toward apply or cancel. Per-arch branch widenings route a 2 to the
arm-preserving restore path (x86 `.restore`, aa64 `.restore`, rv64 `8f`
depth-dec only). `force_apply` (riscv64 peeks only; all clears are
C-side) is retained on deny. Escalation: >=1 tick warns once
(`[S582-APPLY-DEFER-STUCK]` + census), >=8 ticks with the lock held fails
closed (SCHED+17 + §4.9 dump + panic), >=8 ticks with the lock free
re-arms (SCHED+19, arm retained).

Park latch (INV-PARK-2): a BLOCKED runner that `reschedule()` does not
park latches `{task_id, generation, class, since_tick, covered_ticks}`
(class is diagnostic-only — no decision predicate reads it). Service:
release hook (`SchedulerLockGuard` dtor/`unlock`, post-unlock at depth 0,
five-term discriminator) parks in place for SelfLock in plain task
context; the pre-RMS tick point clears (RMS dispatches right after) or
counts a covered instant with rearm; a `:165` try_lock miss counts
lock-free. Covered >= 8 fails closed (SCHED+17); pending >= 1 tick warns
once (SCHED+18). Monotonic clock: rearm can never restart it.

Holder-bounds recount (main at implementation time): **35** static
`SchedulerLockGuard` constructions (**33** in release;
`tick.cpp:86,:105` are `CONFIG_DEBUG_IPC_SCHED`-gated), **7** adopting a
preceding `try_lock` (**5** in release), `registry.cpp:354` excluded
(test helper). The audited 26-row table's row 21
(`scheduler_lifecycle.cpp:816`) names an `ENSURE`, not a guard
construction — folded into the grouped rows; IF=1 task-context rows are
#1 (`try_direct_switch`) and #3 (queue setters), each O(1)+O(MAX_TASKS),
>=80x under the 8-tick deadline. Chained holders across 8 instants are
covered by the monotonic aggregate rule (fail-safe, availability-only).

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

### 3.3 Starvation-breaker (issues #242, #598) [IMPLEMENTED]
Strict RMS starves every lower level forever when the top READY level never
drains (convicted: two prio-11 never-blocking spinners alternate and the
prio-10 harness gets zero CPU over 3001 ticks; #598: a periodic prio-20
waker with READY window < 20 ticks made 20 identical tops unreachable, so a
top-identity counter never fired and a highest-below pick diverted the slot
to a non-starved band).  The fast path stays strict fixed-priority; a
counter-only observer forces progress:

- When lower non-idle READY bands wait while the strict pick serves higher
  levels for `kStarvationBreakerTicks` (= 20) consecutive ticks, exactly one
  dispatch of the *lowest* eligible READY non-idle band is forced
  (`next_task_below`, same eligibility filter as `next_task`), then strict
  RMS resumes.  The observation is keyed to that victim band, so a transient
  higher-priority waker can neither reset it nor divert the forced dispatch.
  Level 0 (idle band) is never a forced target.
- Counter-only: no `task->priority` write (PI boosts and admission bases
  untouched); O(1) bitmap ops (`lowest_starvable_below` + `dequeue_level`);
  identical debug/release control flow.
- EDF keeps precedence: a tick with an EDF candidate takes the EDF path and
  resets the count (a non-strict dispatch breaks the pattern).
- Breaker state is policy-runtime: reset on `init()` and snapshot restore,
  never preserved across tests.
- Schedulability re-characterization: Liu–Leyland admission is unchanged for
  the strict fast path; the breaker adds bounded interference of one forced
  tick per N (= 20) denied ticks (20 ms delay ceiling at 1 kHz).  Bands above
  the victim receive no breaker relief and remain denied while the top never
  drains (a continuously backlogged middle band keeps strict service only
  whenever the top drains; per-band fairness escalation is tracked in issue
  #599).
- Deadline coupling is deliberately absent (fixed N preserves the bound);
  deadline-urgent tasks preempt via EDF first, and the miss handler stays the
  observer of any residual miss.

### 3.4 EDF-blind-spot closure (issue #600) [IMPLEMENTED]

Three defects by code-read, one shared shape (membership proved by scan,
never by flag): the `[RUNQ] already-queued but nowhere linked` diagnostic
reused one index as CPU *and* level and misjudged every legitimately queued
task at a level with `level % CONFIG_MAX_CPUS != cpu` (repaired to a
two-loop scan + counted `runq_nowhere_linked_count_` oracle); the #249
sweep and the snapshot restore rebuild skipped every EDF-eligible task
(now rescued via `enqueue_ready` → `edf_insert`, counted by
`edf_orphan_rescue_count_`); the EDF head walk could not advance past a
non-membership-marked head (`edf_remove` is flag-gated) and is now bounded
(force head advance, magic-bad fails closed, cycle nuked fail-closed,
counted by `edf_chain_drop_count_`). `RQM::move_priority`'s
remove→enqueue composite is one `IrqGuard` unit (INV-RQ-1 extension).

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
`dequeue_ready(*cur)` immediately before `state = BLOCKED` (VULN-IPC-03).
This supersedes the historical "BLOCKED without dequeue" INV-5 exception;
any older membership table still showing that row is stale.

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

- **Auth-kill edge (issue #46):** a `FAILED`-latched authenticity verdict
  terminates via the canonical `terminate_err` path (dequeue → TERMINATED
  → `wake_waiting_parent` → zombie → deferred free), exit code
  `kElfAuthKillExitCode` (0xDB0C). No new lifecycle state; `FAILED`
  latching blocks any later `VERIFIED` transition even if the kill is
  refused/deferred.

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
- **Lock split (`specs/rms-rework.md` Phase 2):** `ready_queue_lock_` →
  `scheduler_lock_` ordering and blocked-sender wakeup ordering — not
  confirmed implemented.
- **SCHED-005:** O(1) removal via authoritative `current_bucket_` on the TCB
  — not landed; the bucket-scan in `remove()` remains the (correct but O(P))
  stopgap. Priority changes must still call `move_priority()` (INV-6).
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

## 10. Idle Authenticity Re-Verify Slice (issue #46)

- **Entry:** `Scheduler::auth_verify_step()` (called once per idle pass
  via `integrity::auth_verify_poll`, after the kernel `.text` CRC chunk).
  One candidate per call: first `UNVERIFIED`/`VERIFYING` user image with a
  loader baseline (`ro_seg_count_ > 0`), registry order; terminal states
  skipped (dead tasks have nothing left to protect).
- **Budget:** ≤2 pages CRC'd per pass through the target page table
  (`VMM::virt_to_phys_in_pml4` + HHDM; unmapped mid-range = fail closed).
  Whole-step `IrqGuard` (bounded: registry walk + 2 page CRCs, no alloc,
  no reschedule); `scheduler_lock_` around select/commit only (the kill
  runs after release — `terminate_err` retakes it).
- **Commit:** progress (`off`/`acc`) or verdict under re-validation
  (magic + `page_table_` + state + offset unchanged ⇒ same live object;
  TCB frees funnel through this same idle task, so the unlocked work
  phase cannot race a free on UP). Completion finalizes `acc` against
  `ro_crc32_` → `VERIFIED`, else latch `FAILED` + kill. A lost race
  returns `PROGRESS` (re-select later) — never a false verdict.
- **Kill reporting** lives in `auth_verify_poll` (dmesg 0xDB0C + serial +
  framebuffer), deliberately not `report_user_task_end` (parents are
  already woken by `terminate_err`; the report is a fixed security-event
  format). `VERIFIED` logs one info line per task lifetime.

## 11. Console Parity (issue #269)

- Owner contract: the framebuffer and serial consoles show the same
  user-actionable output. `Logger` serves serial + dmesg + klog only —
  never fb. Parity is per-site, not central (a Logger→fb fan-out would
  drag boot spam, ISR-context output, and scheduler internals onto the
  small fb text area, and fb rendering assumes task context).
- Mirrored sites (dmesg + Logger + `write_fb` triple, each bounded,
  lock-free, null-instance-safe): `report_user_task_end` (task end),
  loader `post_event` (load lifecycle), `auth_verify_poll` (auth kill),
  reap `terminated` lines (this file's `fb_terminated_line`).
- Serial-only by default: DEBUG/WARN internals, per-tick traces,
  suppressed runs. A line earns an fb mirror by being user-actionable.
- Console **input** and RTC source contracts live in `docs/specs/drivers.md`
  §10 (issue #355): decoder-first ordering, Goldfish/PL031/mtime sources.

## 12. F4 Admin Stop — TCB_CONFIGURE / TCB_SUSPEND / TCB_RESUME (issue #557)

Cap-gated task control (syscalls 99/100/101): a supervisor (or the
creator parent) parks a live task into an orthogonal admin latch and
resumes it later.  The latch is NOT a TaskState — the parked task stays
BLOCKED (or RUNNING-becoming-BLOCKED via the deferred stop) while seven
transient TCB fields (`task.hpp`: `admin_parked`, `admin_wake_pending`,
`admin_stop_requested`, `admin_prev_state`, `admin_suspend_tick`,
`admin_deadline_ticks`, `admin_wdog_expiry_tick`) remember the disposition.

### State table

| Parked? | TCB state | Queues | Wake path |
|---|---|---|---|
| no | READY | runq/EDF as usual | normal |
| yes, prev READY | BLOCKED | dequeued; node removed | resume -> READY + enqueue |
| yes, prev BLOCKED | BLOCKED | untouched (channel kept, INV-ADM-4) | resume -> stays BLOCKED unless a wake was absorbed |
| yes, prev RUNNING | BLOCKED (after the tick deferred stop) | dequeued at park | resume -> READY + enqueue |
| dead (TERMINATED/REAPED) | terminal | latch cleared (INV-ADM-5) | resume refuses DEAD |

### Invariants INV-ADM-1..16

- **INV-ADM-1 (single writer to runnable).** While `admin_parked` is set,
  NO path may set the target READY or link it into a runq; the only
  legal exit is `admin_resume_err`. Two layers: (a) semantic — every
  wake-decision site calls `admin_absorb_wake()` BEFORE writing READY
  (`set_task_ready`, the three tick level-triggered loops, the
  wake-while-current deferral, the mailbox drain, `reapply_wakes`,
  the deadline-monitor wake); (b) structural — `enqueue_ready` /
  `enqueue_ready_inner` refuse an owning parked TCB, restore BLOCKED,
  latch `admin_wake_pending` and count `adm_wake_backstop_count_`
  (must-read-0 outside the dedicated negative test).
- **INV-ADM-2 (absorb is a latch, never a drop).** An absorbed wake sets
  `admin_wake_pending` exactly once, consumed exactly once by resume.
- **INV-ADM-3 (no READY/BLOCKED desync).** A parked task is never
  `in_ready_queue_`, never `in_edf_queue_`, never `ready_deferred_`.
- **INV-ADM-4 (the BLOCKED channel is preserved, never torn down).**
  Suspend does NOT touch mutex/semaphore/event/queue/pager/IRQ waiter
  arrays, `waiting_*`, `reply_wait*`, `blocked_in_recv`,
  `blocked_on_pager_fault`, `waiting_child_*`.
- **INV-ADM-5 (death wins).** TERMINATED/REAPED are terminal for the
  latch: both terminate funnels and `cleanup()` clear all admin fields.
- **INV-ADM-6 (snapshot neutrality).** The seven fields are transient
  (never in TaskFields), zero at every memset/ctor site, byte-rewound
  by `MemPool::restore_pool_data`. No new ResourceTracker type
  (`cap_objects` covers TcbCap).
- **INV-ADM-7 (no new O(n) work on RT paths).** The absorb guard is O(1)
  inside loops that already exist; the mem-mgr walk is
  O(CONFIG_CSLOT_COUNT)=O(64) in AUTHORITY (syscall) context only.
- **INV-ADM-8 (lock discipline).** Suspend/resume are task-context only
  (ENSURE depth 0 + IrqGuard + SchedulerLockGuard). N1 calls
  `set_priority` / `set_affinity_err` OUTSIDE the lock (each takes it
  itself).
- **INV-ADM-9 (deferred stop of a RUNNING target).** Suspend latches
  `admin_stop_requested`; the tick consumes it at a user-mode boundary
  (same preconditions shape as the #225 debugger block, which keeps
  precedence). A task halted in `resched_park_until_dispatched` stays
  halted (the loop breaks on non-BLOCKED) and is released by resume. A
  #582-latch-armed runner is refused BUSY, never silenced.
- **INV-ADM-10 (arch neutrality).** All new logic is shared C++: no
  per-arch asm, no new apply site, no new trap-frame handling.
- **INV-ADM-11 (supervision is paused and restored, never revoked).**
  Suspend of a watchdog-armed target latches `wdog_expiry_tick` into
  `admin_wdog_expiry_tick`, clears `wdog_armed` only, and bumps
  `wdog_gen` (with the 0→1 clamp the scan paths carry — stated, not
  omitted; the suspend edge bumps even never-armed targets, which is
  harmless: kick refuses via `!armed` and the idle seam takes the
  report-only branch). The expiry/last_kick words are PRESERVED (unlike
  `disarm_target`, which zeroes the expiry). Resume, when the latched
  expiry is non-zero, shifts expiry and last_kick by the suspension
  (saturating, the same fail-safe clamp `arm_watchdog` uses), re-arms,
  bumps gen, and emits SCHED+21 (pause) / SCHED+22 (resume). Hence the
  #41 hang guarantee is restored with the phase preserved: N ticks of
  budget left at suspend means N ticks at resume. An already-overdue
  latched expiry resumes already-due and fires on the next scan
  (fail-safe). Paused vs revoked stays observable in procfs without a
  format change (paused: armed=0/expires=<non-zero>; revoked: armed=0/
  expires=0). Arming on a parked target is refused typed; kick-after-
  resume proves supervision is live.
- **INV-ADM-12 (Q9 authority — exact rule).** Evaluated in order, every
  refusal typed with zero side effects: SELF, KERNEL_TASK, IDLE,
  PROTECTED_ROLE, CREATOR (`target.parent_id == caller.id` — kernel-
  assigned at creation only, cleared on orphan/reap, never
  caller-writable), SUPERVISOR (scope == SUPERVISOR AND grantee binds
  the caller id+generation — minted only by `create_supervised`, no
  syscall producer), else NO_AUTHORITY. `DeathNotify::is_watching`
  confers NOTHING (self-grantable slots); raw WdogCap possession is
  watchdog authority only. Branch 6 is deny-all in production until
  M5/M12 and provably so (fail closed).
- **INV-ADM-13 (stop-guarantee boundary: scheduling state, not handoff
  data).** A park freezes the target's SCHEDULING state. It does NOT
  freeze data a waker legitimately hands it: `wake_waiting_parent`
  still stores the child's status into the parked parent's user slot
  (`scheduler_lifecycle.cpp:420`, fault-armed #588 path), clears the
  slot (`:440`), patches the saved return-value slot (`:494-500`) and
  clears `waiting_child_pid` (`:502`) while the READY transition is
  absorbed; `thread_join_note_termination` likewise writes
  `join_status_` / `join_ready_` (`task.cpp:156-157`). Correct on
  resume: the parent re-enters its wait loop, the rescan observes the
  zombie and returns without blocking again.
- **INV-ADM-14 (benign freezes).** While parked: the sporadic-server
  budget is untouched (nothing consumes it while BLOCKED, replenishment
  bounded by its period — no budget created or lost); `remaining_ticks`
  is frozen, consistent with `deadline_ticks` being frozen and only
  SHIFTED by the suspension at resume (both frozen, phase preserved) —
  the "suspension buys budget" direction is closed by construction.
- **INV-ADM-15 (never mem-mgr — enforceable).** Protected iff: (a) a
  registered mem-mgr pid exists and equals the target (the cell is 0
  today — truthful: no user-space mem-mgr exists — and 0 matches
  nothing); (b) the target is a user task whose CSpace holds a live
  `CapType::Untyped` (the spec's defining property of the mem-mgr),
  checked by `cnode_holds_type` under the CNode leaf lock in AUTHORITY
  context only; (c) the `kPdRoleCount == 13` static_assert tripwire
  breaks the build the moment a mem-mgr/supervisor role row is added.
  Layer (b) self-populates at M4 with no table to remember.
- **INV-ADM-16 (deadline supervision is paused, not violated).** While
  parked, `scan_deadlines` skips the task; resume's shift makes
  `deadline_ticks` future-consistent. Removing the deadline node alone
  is insufficient because the scan walks `all_tasks_`.
