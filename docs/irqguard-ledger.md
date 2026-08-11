# IrqGuard Ledger — v0.3.12 G1 (Fine-Grained Locks)

Authoritative audit of every production `arch::IrqGuard` instantiation in the
kernel, as of commit `55fde391` (v0.3.12 milestone definition).

## Summary

- **18** production `arch::IrqGuard` instantiations audited.
- **18 KEEP** (class A — justified IRQ-exclusion).
- **0 MIGRATE** (after SIL-3 finding G1-B-01: the original claim that the
  task.cpp kslot sites have "no ISR accessor" was FALSE — see T1-T3 below).
- **0** migrate to `sync::Mutex` (no site needs a blocking lock; all class-A
  sites are short critical sections with an ISR accessor).
- **9** comment-only mentions refreshed/re-verified (see Comment-Refresh list).

## Classification rules

- **Class A (KEEP):** the critical section genuinely excludes the timer ISR
  (or a nested IRQ), and a plain SpinLock alone cannot provide the required
  guarantee (lock-free field snapshots, publish/apply handshakes, or a lock
  whose holder must not be preempted by an ISR that also takes it).
- **Class B (MIGRATE):** the state is mutated only from task context; there is
  no ISR-path accessor. A plain `sync::SpinLock` provides the same mutual
  exclusion without masking IRQs (lower latency, no preemption delay).

## Sites

### KEEP — class A (justified IRQ-exclusion)

| ID | Site | Function | Critical section | Concurrent accessor | Primitive | Justification |
|----|------|----------|------------------|--------------------|-----------|---------------|
| S1 | scheduler.cpp:177 | `Scheduler::effective_priority` | Consistent snapshot of `t->priority` + `sporadic_server` state | Timer ISR (sporadic consume/replenish, deadline demote); IPC PI under `q.lock_` | IrqGuard | Lock-free read must be one atomic snapshot vs ISR mutations; IrqGuard is no-op in ISR context and cheap on single-core |
| S2 | scheduler.cpp:212 | `Scheduler::set_priority` | Priority field write + O(1) ready-queue re-bucket | Timer ISR | IrqGuard | `move_priority` must not interleave with ISR priority reads; field write must be atomic vs ISR |
| S3 | scheduler.cpp:342 | `Scheduler::drain_zombie_list` | Zombie list pop + ready-queue remove | Timer ISR watchdog (on_tick) | IrqGuard | List pop must be atomic vs ISR; no scheduler_lock_ held (cleanup starves ISR) |
| S4 | scheduler.cpp:370 | `Scheduler::cleanup_step` | Single zombie pop | Timer ISR watchdog | IrqGuard | Same as S3 |
| S5 | scheduler.cpp:416 | `Scheduler::set_task_ready` | `state=READY` + `enqueue_ready` | Timer ISR | IrqGuard | Enqueue + state change must be atomic vs tick (INV-5 [WEDGE] invariant) |
| S6 | scheduler.cpp:477 | `Scheduler::terminate` | Dequeue + TERMINATED store + wake_waiting_parent + zombie release | Timer ISR | IrqGuard + scheduler_lock_ | IRQ exclusion prevents ISR try_lock starvation while this task holds scheduler_lock_ |
| S7 | scheduler.cpp:1373 | `on_tick` gated tail | Deferred-kill flush, sporadic budget mgmt, zombie reap/flush | Timer ISR; lock holder mid-mutation | IrqGuard (gated on lock_acquired) | Tail sections must be atomic vs nested IRQs AND the lock holder's partial writes |
| S8 | scheduler.cpp:2264 | `Scheduler::switch_to_task` | `release_lock()` + deferred-switch publish (generation-lock arm) | Timer ISR epilogue (isr_stubs.asm apply) | IrqGuard | Publish/apply handshake: ISR must never observe a half-published switch pair (H2 ring) |
| S9 | scheduler.cpp:2392 | `Scheduler::reschedule` | Ready-queue peek + `scheduler_need_resched` publish | Timer ISR | IrqGuard | UP read-only peek needs IRQ-safety; holding the lock would starve ISR try_lock |
| S10 | scheduler.cpp:2532 | `Scheduler::switch_away_from_terminating` | `next_task_id` + generation bump + `save_rsp_to` arm | Timer ISR epilogue | IrqGuard | Atomic publish vs ISR apply (same H2 ring discipline as S8) |
| S11 | scheduler.cpp:2853 | `Scheduler::monitor_task_entry` | `dequeue_ready` + BLOCKED store under lock | on_tick wake path (also takes lock) | IrqGuard | Dequeue + state change atomic with respect to the wake handshake (INV-5) |
| T4 | task.cpp:1285 | `TaskControlBlock::cleanup` | `Scheduler::unregister_task` | Timer ISR (on_tick); reaper | IrqGuard | With IRQs off, only the reaper can hold scheduler_lock_ — unregister's try_lock cleanly distinguishes skip/do-it |
| TD1 | taskdefs.cpp:181 | `reboot_from_table` | Whole teardown (kill all tasks, drain zombies, reset queues) | Timer ISR | IrqGuard | Teardown touches every task; ISR concurrency would corrupt the tables mid-reset |
| I1 | ipc.cpp:421 | `IPC::block_sender` PI boost | `q.owner->priority` RMW + old/new snapshot + `move_priority` | Timer ISR (deadline demote / sporadic change) | IrqGuard (inside `q.lock_`) | `q.lock_` is a plain spinlock (does not mask IRQs); IrqGuard excludes the ISR so the old/new effective-priority snapshots stay consistent |
| I2 | ipc.cpp:469 | `IPC::wake_sender` PI restore | Receiver priority restore + old/new snapshot + `move_priority` | Timer ISR | IrqGuard (inside `q.lock_`) | Same as I1 |
| T1 | task.cpp:391 | `alloc_kslot` free-list scope | First-fit scan of `s_kslot_list`/`s_kslot_pool` + free-head update | Timer ISR (on_tick → reap_orphans → idle recreate) | IrqGuard | ISR-reachable: `Scheduler::on_tick` (timer ISR) calls `reap_orphans()` every 100 ticks, which re-creates the idle task via `TaskControlBlock::create()` → `alloc_kslot()`. A plain SpinLock deadlocks: a task-context holder preempted by the timer ISR can never release it. IRQ masking is REQUIRED (SIL-3 finding G1-B-01; the original "no ISR accessor" claim was wrong). |
| T2 | task.cpp:411 | `alloc_kslot` bump scope | `s_kslot_bump` bump-allocate | Timer ISR (same chain as T1) | IrqGuard | Same as T1 — ISR-reachable, IRQ masking required |
| T3 | task.cpp:421 | `free_kslot` | Free-head push of `s_kslot_pool` entry | Timer ISR (task exit via reap_orphans) | IrqGuard | Same as T1 — ISR-reachable via the same reap chain, IRQ masking required |

## Migration outcome

- 18 keep / 0 migrate / 0 sync::Mutex.
- KEEP sites retain `arch::IrqGuard` exactly as found — no statement was
  reordered inside any IrqGuard-scoped section, and the deferred-switch
  publish/clear sequences (S8/S10) were not touched.
- G1-B outcome CORRECTED (SIL-3 finding G1-B-01, auditor-confirmed):
  the v0.3.12 G1-B migration of the kslot sites (T1-T3) from `arch::IrqGuard`
  to `static sync::SpinLock s_kslot_lock` was based on a FALSE premise
  ("no ISR accessor").  The timer ISR chain
  `on_tick` (scheduler.cpp:974) → `reap_orphans()` (scheduler.cpp:1509) →
  idle `TaskControlBlock::create()` (scheduler.cpp:1624) → `alloc_kslot()`
  (task.cpp:647) would spin forever on `s_kslot_lock` when a task-context
  holder is preempted by the tick — the same S3/S4/S7 deadlock class the
  original IrqGuard prevented.  The migration was REVERTED to `arch::IrqGuard`
  (safe minimal fix).  Every site reachable from the timer ISR or the asm
  epilogue keeps IrqGuard; NO production site uses a plain SpinLock for
  ISR-reachable state.

## Comment-refresh list

Comment-only `IrqGuard` mentions verified still accurate after G1 (no code
change required):

| Site | Comment |
|------|---------|
| scheduler.cpp:175 | "IrqGuard is a no-op when IRQs are already disabled (ISR context)..." — S1 |
| scheduler.cpp:1370 | "Gate them on lock_acquired and hold IrqGuard so they are atomic..." — S7 |
| scheduler.cpp:1518 | "end: gated tail sections (lock_acquired && IrqGuard)" — S7 |
| scheduler.cpp:2422 | "IrqGuard destructor re-enables IRQs here, allowing the timer ISR to fire..." — S9 |
| ipc.cpp:189 | "If interrupts are off (e.g. under IrqGuard) the ISR can't fire..." — IPC::send spin-wait |
| ipc.cpp:371 | "Callers should ensure IRQ safety is managed externally (e.g., via arch::IrqGuard in IPC::send)" — caller discipline note |
| ipc.cpp:418 | "spinlock (does not mask IRQs); IrqGuard excludes the ISR so old/new snapshots stay consistent" — I1 |
| taskdefs.cpp:204 | "IRQs are disabled (IrqGuard at function entry), so no ISR concurrency..." — TD1 |
| mutex.cpp:300 | "the test pattern (IrqGuard + set_current with disabled interrupts) causes a false positive" — VULN-SYNC-01 safety net |

## Stale-comment correction (ipc.cpp:365-366)

The `block_sender` caller-discipline doc said:

> The scheduler lock (`scheduler_lock_`) must NOT be held when entering this
> function — `dequeue_ready()` acquires it via `ready_queue_.remove()`.

**Verified FALSE:** `ReadyQueueManager::remove` is lock-free
(`src/kernel/task/ready_queue_manager.cpp:75-91` — uses `rq_priority_`,
the bucket's intrusive list, and the bitmap; no scheduler lock). The comment
is corrected to state the truth: `remove` is lock-free; the enclosing
`arch::IrqGuard` (site S5 within `dequeue_ready`, and the caller's own guard)
— not a lock — protects the section.

## Cross-reference (G2/G3)

- G2 reference-migration audit: see §G2-A appendix below (appended by G2).
- G3: tmpfs remains MemPool-only (see tmpfs.cpp G3-A comments).
- G3 note: tmpfs ignores refcount (an fd held open across unlink reads freed
  memory) — pre-existing VFS-model item, out of G3 scope.

---

# Appendix G2-A — Reference-Enforced Tasks (v0.3.12 G2)

Audit of pointer-to-reference migration opportunities. A site is
reference-migratable only when the caller *provably holds a live object*
(after a null guard / magic check in the same scope).

## Reference-migratable (caller provably holds live object)

| Site | Context | Migration |
|------|---------|-----------|
| scheduler.cpp:564-566 | `Scheduler::init` — after `if (!idle_task_) panic(...)` at :563 | `TaskControlBlock &idle = *idle_task_;` then `idle.state = READY`, `strncpy(idle.name, ...)`, `idle.name[...] = 0` |
| scheduler.cpp:1285-1290 | `on_tick` monitor wake — inside `if (s_monitor_task_ && magic && state==BLOCKED)` | `TaskControlBlock &m = *s_monitor_task_;` then `m.state = READY; enqueue_ready(m);` |
| ipc.cpp:421-427 | `IPC::block_sender` PI boost — inside `if (q.owner && ...)` | `TaskControlBlock &owner = *q.owner;` then old/new effective_priority snapshot + `move_priority(owner, old, new)` on the reference (IrqGuard I1 kept; move_priority stays between snapshots) |
| scheduler.cpp:1304 / 1473 / 2837 | `deadline_miss_handler` call sites — loop conditions / magic checks guarantee non-null | Signature changed to `deadline_miss_handler(TaskControlBlock &task, uint64_t missed_by_ticks)`; call sites pass `*task` / `*t` |

## Null-check-required (keep raw pointer)

| API | Reason |
|-----|--------|
| `Scheduler::find_task` / `id_table_find` | Returns null for unknown/removed ids |
| `Scheduler::task_at` | Indexed table access — may be null |
| `Scheduler::current_task` | May be null before init / on boot stack |
| `get_harness_task` / `get_monitor_task` | Optional objects — may not exist |
| `wake_waiting_parent`'s `find_task(child.parent_id)` | Parent may have exited — null-checked |
| `Scheduler::effective_priority` | Public API takes `const TCB *`; callers legitimately pass null |

## Already-reference-based (no change)

`enqueue_ready(TaskControlBlock &)`, `dequeue_ready(TaskControlBlock &)`,
`set_task_ready(TaskControlBlock &)`, `move_priority(TaskControlBlock &, ...)`,
`terminate(TaskControlBlock &, ...)`, `set_priority(TaskControlBlock &, ...)`,
`switch_to_task(TaskControlBlock &, ...)`, `wake_waiting_parent(TaskControlBlock &)`,
`register_task(TaskControlBlock &)`, `unregister_task`-adjacent TCB APIs,
`IPC::block_sender(MessageQueue &, TaskControlBlock &)`,
`IPC::wake_sender(MessageQueue &, TaskControlBlock &)`.
