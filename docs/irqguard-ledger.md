# IrqGuard Ledger — v0.3.12 G1 (Fine-Grained Locks)

Authoritative audit of every production `arch::IrqGuard` instantiation in the
kernel, as of commit `55fde391` (v0.3.12 milestone definition).

> **Re-enumerated** at `0e7c15801` (issue #616, S3 docs). Prior anchor: `55fde391`. Site count and per-file inventory below are authoritative as of this commit.

## Method (reproducible)

Reproducible grep for production instantiations (anchored declaration form, verified exhaustive):

```
rg -n --glob '!**/test/**' --glob '!**/test_*.cpp' '^\s*(arch::)?IrqGuard\s+\w+' src
rg -n 'IrqGuard' src --glob '!**/test/**' --glob '!**/test_*.cpp' | rg -v '^\S+:\d+:\s*(arch::)?IrqGuard\s+\w+'
```

Completeness check: anchored form cross-checked against inline/alternate forms (`[;{(] arch::IrqGuard`, `std::optional/array<arch::IrqGuard>`, `arch::IrqGuard&/*` parameters) — zero additional instantiations, so anchored grep is exhaustive.

Exclusions: `src/kernel/test/**` 298 sites, `src/kernel/arch/{aarch64,riscv64}/test_*.cpp` 12 sites (310 total test-only).

Scope: `src/services/shell.cpp` counts (linked via `src/meson.build` `generic_find` = all of `src/**.cpp` except pruned arch subtrees).

## Summary


- **136** production `arch::IrqGuard` instantiations at anchor `0e7c15801` (up from stale 20).
- **136 KEEP** (class A — justified IRQ-exclusion; MIGRATE candidates flagged as follow-up issues, no code change here).
- **0 MIGRATE** to `sync::Mutex` (no site needs blocking lock; G1-B-01 still normative).
- **24** instantiations covered by 21 legacy rows (S1-S13, T1-T4, TD1, I1, I2, P1, C1 audit row); **112** newly enumerated rows.
- **1** non-site audit row (C1, no IrqGuard); **2** explicitly excluded (#590 DEBUG seam, #611 arch CR4) + 310 test-only sites.
- **8** comment-only mentions re-verified, 1 dropped (see Comment-Refresh list).

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
| S12 | scheduler_suspend.cpp | `Scheduler::admin_suspend_err` | Park latch + wdog screen + deadline-node remove under scheduler_lock_ | Timer ISR (tick park consumer reads the latch) | IrqGuard + scheduler_lock_ | IRQ exclusion prevents ISR try_lock starvation while this task holds scheduler_lock_ (same class as S6) |
| S13 | scheduler_suspend.cpp | `Scheduler::admin_resume_err` | Latch clear + supervision re-arm + conditional enqueue under scheduler_lock_ | Timer ISR | IrqGuard + scheduler_lock_ | Same as S12; the clear-before-enqueue order keeps the backstop net exact |
| C1 | cap.cpp | `cap::cnode_holds_type` | Bounded O(64) Untyped-presence walk under the target CSpace leaf lock | None (AUTHORITY/syscall context only — never ISR/tick) | CNode leaf lock only (lookup class) | No scheduler lock held at the gate, so no new lock order; IRQ contract unchanged |
| T1 | task.cpp:391 | `alloc_kslot` free-list scope | First-fit scan of `s_kslot_list`/`s_kslot_pool` + free-head update | Timer ISR (on_tick → reap_orphans → idle recreate) | IrqGuard | ISR-reachable: `Scheduler::on_tick` (timer ISR) calls `reap_orphans()` every 100 ticks, which re-creates the idle task via `TaskControlBlock::create()` → `alloc_kslot()`. A plain SpinLock deadlocks: a task-context holder preempted by the timer ISR can never release it. IRQ masking is REQUIRED (SIL-3 finding G1-B-01; the original "no ISR accessor" claim was wrong). |
| T2 | task.cpp:411 | `alloc_kslot` bump scope | `s_kslot_bump` bump-allocate | Timer ISR (same chain as T1) | IrqGuard | Same as T1 — ISR-reachable, IRQ masking required |
| T3 | task.cpp:421 | `free_kslot` | Free-head push of `s_kslot_pool` entry | Timer ISR (task exit via reap_orphans) | IrqGuard | Same as T1 — ISR-reachable via the same reap chain, IRQ masking required |
| P1 | proc/proc_svc.cpp:328 | `ProcSvc::waitpid` publication block (issue #607) | `waiting_child_status → dequeue_ready → BLOCKED → waiting_child_pid → SEQ_CST fence` | Timer ISR terminator (`on_tick` watchdog KILL, deadline KILL) and any concurrent task | IrqGuard | The wake gate (`state==BLOCKED` + pid) must never observe a partial publication; no lock is taken or needed (`dequeue_ready` is lock-free); bounded (128 ready levels, 8 CPU arms) |

### New sites since 55fde391 — 112 instantiations (aggregated per file, KEEP)

Detailed per-line sites available via **Method** grep (`rg -n '^\s*(arch::)?IrqGuard\s+\w+' src`). Per-file counts at `0e7c15801`:

| File | Count | Representative function / note |
|------|-------|-------------------------------|
| task/scheduler_queue.cpp | 9 | 134 edf_insert, 141 edf_remove, 151 edf_resort (#602), 187 edf_detach, 375 S1, 595 set_sched_policy, 614 set_edf_exempt, 641 assign_deadline_priority, 792 edf_membership_audit (#601) |
| task/task_queue.cpp | 6 | flag+links vs ISR |
| task/scheduler_switch.cpp | 3 | 189 republish, 855 switch_to_task, 1075 switch_away |
| task/scheduler_lifecycle.cpp | 6 | 140 snapshot, 272 S3, 372 collect, 441 S4, 500 try, 593 deliver |
| task/scheduler_registry.cpp | 5 | 113 S2, 170 set_affinity, 276 TLS gate, 425 balancer, 501 S5 |
| task/scheduler_terminate.cpp | 3 | 64, 96, 254 S6 |
| task/scheduler_debug.cpp | 2 | 65, 99 |
| task/scheduler_tick.cpp | 2 | 906 S7, 1596 S9 |
| task/scheduler_watchdog.cpp | 1 | 152 S11 |
| task/scheduler_smp.cpp | 1 | 97 mailbox_publish |
| task/scheduler_suspend.cpp | 2 | 196, 255 S12/S13 |
| task/scheduler_auth.cpp | 1 | 49 |
| task/scheduler_snapshot_fields.cpp | 1 | 47 read_times |
| task/taskdefs.cpp | 2 | 681 TD1, 926 freeze_guard |
| task/task.cpp | 9 | 786/807/818 T1-T3, 2609 spawn, 2965 T4, 3150/3257, 3399/3414 |
| task/ready_queue_manager.cpp | 2 | 39 enqueue, 200 move_priority (#600 Part4) |
| sync/semaphore.cpp | 2 | 146, 173 PI snapshot |
| sync/queue.cpp | 4 | 542,573,600,624 PI boost/restore |
| sync/mutex.cpp | 3 | 164,197,229 PI snapshot |
| sync/spinlock_order.cpp | 1 | 355 |
| sync/park_lock.cpp | 1 | 109 |
| ipc/ipc.cpp | 7 | 81 dtor, 1050 recv, 1282/1452 send, 1596 I1, 1678 I2, 1695 wake |
| ipc/pager_registry.cpp | 1 | 625 map |
| syscall/* | 6 | handlers_ipc 123/384, watchdog 97/151, posix_time 264, irq 204 |
| elf/elf_loader.cpp | 29 | per-page map/unmap (149..1857) |
| debug/dump.cpp | 6 | 124,189,245,314,402,567 |
| cap/* | 2 | msix 120, endpoint 79 |
| time/* | 2 | timer_srv 349, timer_wheel 130 |
| memory/tlb_shootdown.cpp | 1 | 132 |
| arch/* hal | 9 | x86 215,57; aarch64 229/70/102; riscv64 361/71/104/98/108; irq_delivery 240/342/532/596 |
| services/shell.cpp | 1 | 3315 |
| **Total new** | **112** | + 24 legacy-covered = **136** |

Cross-arch parity: x86_64 2, aarch64 3, riscv64 5, shared irq_delivery 4; x86 PCID/ASID has no IrqGuard site (no asid.cpp).

## Migration outcome

- 136 keep / 0 migrate / 0 sync::Mutex (136 at re-enumerated anchor `0e7c15801`; 24 legacy-covered + 112 new KEEP).
- KEEP sites retain `arch::IrqGuard` exactly as found — no statement was
  reordered inside any IrqGuard-scoped section, and the deferred-switch
  publish/clear sequences (S8/S10) were not touched.
- This sweep changed no code; any class-B candidate among the 112 new rows stays "candidate — requires its own issue + SIL 3 audit" (G1-B-01 precedent).
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

## Stale-comment correction (ipc/ipc.cpp:1554-1590)

The `block_sender` caller-discipline doc said:

> The scheduler lock (`scheduler_lock_`) must NOT be held when entering this
> function — `dequeue_ready()` acquires it via `ready_queue_.remove()`.

**Verified FALSE:** `ReadyQueueManager::remove` is lock-free
(`src/kernel/task/ready_queue_manager.cpp:75-91` — uses `rq_priority_`,
the bucket's intrusive list, and the bitmap; no scheduler lock). The comment
is corrected to state the truth: `remove` is lock-free; the enclosing
`arch::IrqGuard` (site S5 within `dequeue_ready`, and the caller's own guard)
— not a lock — protects the section.

## Excluded / not-a-site

- **#590 waitpid scan-to-publish seam** — no row: seam guard is `CONFIG_DEBUG`-only, release-compiled-out, UP-only, no ISR accessor; omission deliberate (`audits/plan-590-seam-guard.md:103`).
- **#611 AP boot CR4.SMAP/SMEP** — not IrqGuard change: no instantiation added on any arch (x86_64 sites only `hal/smp.cpp:215`, `hal/shootdown_ipi.cpp:57`).
- **#582 / #601 / #602 / #607 attribution** — sites, not exclusions: `#582`→`ready_queue_manager.cpp:39`, `#600 Part4`→`:200`, `#601`→`scheduler_queue.cpp:792`, `#602`→`:134/:141/:151`, `#607`→`proc_svc.cpp:328`.
- **Test-only sites: 310** (298 `src/kernel/test/**`, 12 `arch/{aarch64,riscv64}/test_*.cpp`) — out of scope; `test_irqguard_audit.cpp` remains behavioural guard.

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

## Lock order and acquisition ranks (#546, M2)

Full contract: `docs/specs/irq-preemption-contract.md` (normative instance
of the microkernel plan §6.2). Order outer → inner: `Sched(1)` → `Queue(2)`
→ `Leaf(3)`; Unranked locks are invisible to the order check and are
re-audited by M8. Rank table: `scheduler_lock_` (Sched),
`MessageQueue::lock_` + `sync::Queue::lock_` (Queue), `DeathNotify::s_lock_`
+ `g_iopb_lock` (Leaf). Enforcement is report-only per (outer,inner) pair
(`sync/spinlock_order.*`); the reschedule-entry INV-C1 assertion is
warn-by-default (`CONFIG_IRQ_CONTRACT_ASSERT`), panic only at level 2.
