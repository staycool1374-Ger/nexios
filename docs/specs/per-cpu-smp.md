# Per-CPU Foundation & SMP Bringup Skeleton

**Doc ID:** NEX-SPEC-2026-08-23-002
**Status:** IMPLEMENTED (issue #25 Phase C1, commit abd52d85; FPU migration
split to #151; TPR follow-up in #26; per-CPU asm proof in #27)
**Milestone target:** v0.4.4 (design now, land with Phase 5 SMP)
**Inspiration:** Cyjon `kernel/task.asm` (`KERNEL.task_cpu_address[lapic_id]`)
and `kernel/init/ap.asm` (uniform AP init path); noted caveat: Cyjon's own
source marks its AP task registration as racy — we adopt the *structure*, not
the synchronization.
**Related:** `docs/specs/scheduler.md` §8 ("isr_nesting_depth → per-CPU asm —
deferred to SMP"), `docs/specs/memory.md`, `docs/specs/drivers.md` §6.

## 1. Current State (verified)

NexIOS is single-core with two gs-relative slots already in place:

```asm
; syscall_entry.asm
mov [gs:0x00], rsp        ; user RSP saved
mov rsp, [gs:0x08]        ; kernel stack loaded
```

Global state that must become per-CPU under SMP (audit findings):
`isr_nesting_depth`, `irq_entry_tsc` (both plain `[rel …]` in
isr_stubs.asm:91ff), `fpu_owner` (global_state.cpp:67), the scheduler
run-queue lock, and `hhdm_modified_` (VAR-17, flagged "re-audit under SMP").

## 2. Cyjon's Model and What We Take From It

Cyjon reads the LAPIC ID, indexes `KERNEL.task_cpu_address[]`, and gets the
current-CPU context in three instructions — no segment-register gymnastics.
The idea generalizes: **one flat array indexed by LAPIC ID, filled during BSP
+ AP bringup, read-only afterwards.**

What we deliberately do differently:

| Aspect | Cyjon | NexIOS |
|---|---|---|
| Index source | rdmsr-free MMIO read each time | GS_BASE MSR set once per CPU at init; struct cached in gs |
| AP init sync | spinlock byte + known race ("-_-" comment) | INIT-SIPI with documented rendezvous + timeout panic |
| Task registry | global array, unlocked | existing Scheduler TCB lists behind scheduler_lock_ |

Reason to prefer GS_BASE over repeated LAPIC-ID reads: our syscall entry
*already* depends on gs (user-RSP/kernel-stack swap), so the per-CPU block
costs nothing extra; a MMIO LAPIC read per ISR would add ~100+ cycles to
every interrupt — unacceptable for RT latency budgets.

## 3. Design

### 3.1 Per-CPU block layout

```
struct PerCpu {                 // 4 KiB-aligned, one page per CPU
    uint64_t user_rsp;          // gs:0x00  (existing — ABI frozen)
    uint64_t kernel_rsp;        // gs:0x08  (existing — ABI frozen)
    uint64_t cpu_id;            // gs:0x10  logical id == index
    uint64_t lapic_id;          // gs:0x18
    uint64_t isr_nesting_depth; // gs:0x20  moved from .bss
    uint64_t irq_entry_tsc;     // gs:0x28
    void *   fpu_owner;         // gs:0x30  (TaskControlBlock*)
    void *   current_task;      // gs:0x38
    ... reserved to page end
};
PerCpu per_cpu[CONFIG_MAX_CPUS];   // CONFIG_MAX_CPUS default 4
```

Migration is mechanical: every `qword [rel isr_nesting_depth]` becomes
`qword [gs:0x20]`; C accesses go through `arch::per_cpu()->field`. A
single-core build keeps `CONFIG_MAX_CPUS == 1` so semantics are provably
unchanged (the gs base points at `per_cpu[0]`, set during boot before the
first `cli` region ends).

### 3.2 AP bringup skeleton (Phase 5)

1. BSP parses MADT, allocates per-Cpu pages, fills `lapic_id` per entry.
2. For each AP: send INIT, SIPI (vector of `ap_trampoline`), SIPI again after
   200 µs if the CPU's `started` flag (in its PerCpu page, written by the AP
   itself) is still clear. Second SIPI failure ⇒ controlled panic naming the
   LAPIC id — fail-closed, never half-alive cores (Cyjon's lesson inverted).
3. Trampoline (identity-mapped, < 4 KiB): load the page's own GDTR, set
   GS_BASE = this PerCpu page (wr{fs,gs}base MSRs if available, else WRMSR
   path), load CR3 (shared kernel PML4 — see memory.md kernel-half rules),
   far-jump to `ap_main` (high half), unmap trampoline after all APs are up.
4. `ap_main`: GDT/TSS per CPU (TSS registered in its own GDT slot), IDT is
   shared/read-only, enable LAPIC, mark started, `sti`, enter idle loop that
   pulls work via the (Phase 5) scheduler hook.

### 3.3 Sequencing rule

No AP enters the scheduler until the ready-queue generation counter scheme
(scheduler.md §7 snapshot machinery) is extended with a per-CPU run queue;
until that lands, APs park in idle and only service IPI-directed IRQ work.
This gives us bringup testing *before* scheduling correctness work.

## 3.4 Phase C1 design (AP runs pinned kernel tasks — approved scope)

C1 brings one AP into the scheduler under tight bounds. Single-CPU runs
must behave identically to Phase B (every decision indexes CPU 0).

### 3.4.1 Per-CPU state (what moves, what stays global)

| State | Placement | Notes |
|---|---|---|
| `current_task()` | `CpuContext cpu_ctx[8]` (.bss); `current_cpu()` indexes by true CPU | Zero call-site changes (88 sites keep working) |
| Deferred-switch atoms (`save_rsp_to`, `load_rsp_from`, `load_cr3_from`, `next_task_id`, `kstack_base/top`, `generation`, `need_resched`) | `PerCpu` page slots `gs:0x40–0x78` | asm `[rel X]` → `[gs:0xNX]` (16 sites); C++ `per_cpu[cpu].sw_*`; INV-PC4 checker forbids `[rel scheduler_*]` |
| `scheduler_kernel_cr3` | Stays GLOBAL (read-only after boot) | asm/C++ unchanged |
| `scheduler_corruption_count`, `deadline_detection_integrity`, `fpu_nm_depth_max` | Stay GLOBAL (already `__atomic_`) | unchanged |
| Ready queues | `ReadyQueueManager ready_queues_[8]` (Scheduler statics) | Single GLOBAL `scheduler_lock_` retained — no new lock discipline |
| `idle_task_` | `idle_tasks_[8]`; `get_idle_task()` keeps `[0]`; `is_idle_task(t)` helper | reap/cleanup/task_at sites updated (~10) |
| `isr_nesting_depth`, `irq_entry_tsc` (C++ readers) | Own slot `per_cpu[cpu]` (asm already gs) | x86 linker aliases removed if zero C++ readers remain (link error = proof) |
| `fpu_owner` | STAYS global in C1 (deferred to #151) | AP tasks must avoid FPU (C1 constraint) |
| `gs:0x38 current_task` slot | RESERVED (superseded by CpuContext array) | documented, not wired |

`cpu_index()`: x86_64 reads `GS_BASE` via RDMSR, range-checks against
`per_cpu[]`, returns 0 outside (early-boot safety); other archs return 0.
Cost (~20 cycles) is negligible against per-tick work.

### 3.4.2 Locking contract (asymmetric — v2 after design audit)

The v1 "queue[C] written only by CPU C" rule is WITHDRAWN (unenforceable:
~12 BSP-side writer families — terminate/remove/unregister/add/
move-priority/IPC-inheritance/on_tick-sporadic/zombie-drain/reap/
cleanup-test/snapshot-restore/boot-spawns — write the VICTIM's queue from
the CALLER's CPU). The enforceable rule is asymmetric:

- Task-context queue writes (both CPUs) hold the global `scheduler_lock_`
  AND run with IF=0. New C1 task-context sites take both explicitly
  (never relying on caller context); existing sites are unchanged.
- ISR queue writes are own-queue-only AND `try_lock`-gated (`drop_arm`,
  mailbox drain, IPI handler): on contention they SKIP, and the pending
  arm/mailbox is retried by the next tick. No ISR path blocks; no ISR
  path touches a remote queue, in either direction.
- There is NO AP `reschedule()` equivalent (no AP lock-free peeks at
  all): the AP dispatches only from its tick. BSP `reschedule()` keeps
  IrqGuard-only peek of queue[0] — sound because AP→BSP wakes are
  mailboxed symmetrically and IF=0 excludes own ISR.
- Cross-CPU wakes in BOTH directions go through mailbox + IPI (never
  direct remote enqueue — including terminate/`wake_waiting_parent`
  paths, which hold the lock and therefore publish safely).
- Producer lock order: `scheduler_lock_` → `mailbox_lock[cpu]` (leaf);
  ISR producers holding slot locks (`irq_delivery` `r->lock_`) order
  slot → mailbox (both leaves, documented acyclic — mailbox never leads
  to slot or global). Consumers hold mailbox alone.
- Full order (v3 closes the R1 grandfather gap): IPC/slot locks →
  `scheduler_lock_` → `mailbox_lock` → (APIC ICR poll, lock-free HW).
  The v2 text grandfathered "existing sites unchanged" while
  `set_priority()` (IrqGuard-only) and the IPC-inheritance
  `move_priority` families (`sync/queue.cpp`, `ipc/ipc.cpp`,
  `cap/endpoint.cpp`) write remote queues lock-free. CLOSED by
  conversion: `move_priority`/`enqueue`/`dequeue`/`remove` take
  `scheduler_lock_` (blocking) with a documented task-context-only
  contract; the IPC-inheritance call sites are audited to task context
  at implementation (any ISR-context caller found uses try_lock +
  defer-to-tick instead). ISR-adjacent users (`drop_arm` etc.) keep
  try_lock + skip-and-retry — never blocking, never remote.
- Zombie list: dedicated `zombie_lock_` leaf with a caller-holds
  protocol (non-recursive SpinLock — no re-take anywhere):
  `release_zombie`-push and `flush_zombies` take the leaf ONLY and
  require the caller to hold the global (`terminate`, `on_tick`
  already do — verified at implementation); `cleanup_step`-pop and
  `drain_zombie_list` take global→leaf themselves and require
  callers to hold NO lock (`snapshot_restore`, `cleanup_test_tasks`,
  both idles — verified at implementation). Two-phase rule, stated
  explicitly: pop/flush/drain do list surgery under the locks;
  `cleanup()` + `MemPool::free()` always run AFTER release (this
  also answers the starvation point — no free under the global).
  Queue-`remove()` branches inside flush/drain/cleanup inherit the
  caller's global coverage.
- `set_affinity()` takes `scheduler_lock_` + IrqGuard (task-context
  only, documented NOT-ISR-safe); dequeues + re-enqueues across the
  mask change.

### 3.4.3 AP scheduler entry (mirrors the BSP's HLT-driven first dispatch
+ start-gate — v2 after design audit)

BSP precedent (`taskdefs.cpp` reboot path): `RSP = idle stack top`,
`sti`, `hlt` loop — the first tick applies the deferred switch via the
ISR epilogue. AP does the same via shared `arch::enter_idle_context(top)`,
with three added boot-contract items (all missing in v1):

- AP loads the shared IDT (`IDT::load()` on the AP — descriptor is
  global read-only) BEFORE `sti`; without it the first AP timer IRQ
  triple-faults (Phase-B `ap_main` explicitly had "no IDT").
- Start-gate: the AP spins pre-scheduler parked (Phase-B behavior) until
  the BSP publishes `smp::scheduler_ready` AFTER `reboot_from_table()`
  finishes spawning (otherwise the AP's tick reads `all_tasks_` mid-
  rebuild). The flag is `__atomic` bool: BSP publishes with RELEASE,
  AP spins with ACQUIRE + `arch::pause()` (no hoisting, no early
  observation). Only then: create AP idle (normal locked `add_task`-family
  path — safe: tables quiescent, BSP past teardown), set own current,
  load the shared IDT FIRST (a firing timer IRQ with no IDT
  triple-faults: `timer_init` programs INITCNT, live immediately in
  bus-periodic mode, and calibration itself arms the timer), then
  program APIC timer and enter idle context.
- AP idle entry is `ap_idle_main` (`cleanup_step()` + `hlt` ONLY — never
  CRC/markers: `crc_owner_lock_` ENSURE forbids concurrent CRC), built
  via `TaskControlBlock::create()` (valid created-order iret frames,
  satisfying the `switch_to_task` dispatch guard).
- FPU tripwire (v1 had hope, not enforcement): `ap_main` sets
  `CR0.TS=1` + `CR4.OSFXSR|OSXMMEXCPT`; the #NM handler fail-stops on
  `cpu != 0` (panic "FPU on AP" — C1 forbids; #151 will allow). With
  TS=1 any AP x87/SSE faults loudly instead of corrupting BSP lazy
  state silently. (Trampoline leaves TS=0/EM=0, which would let x87
  run silently — hence setting it here, not there.)

No manual first-switch is needed on either CPU.

### 3.4.4 Tick split (blast-radius containment)

- Shared timer ISR lambda branches by CPU: BSP → `handle_irq()` +
  full `on_tick()` (600-line body UNTOUCHED); AP → TSC re-arm +
  dispatch-only `ap_tick()` = `rate_monotonic_schedule()` on own state.
- AP skips: accounting, deadlines, watchdogs, zombies, sporadic,
  `handle_irq` ticks (global `ticks_` stays 1 KHz on both variants —
  no 2× drift), profiling sampler.
- `rate_monotonic_schedule` runs on AP via the global `try_lock`;
  skip-and-retry is bounded by the C1 liveness argument: EVERY
  global-lock critical section is O(MAX_TASKS)-bounded with no
  blocking syscalls/IO (verified at implementation; the longest
  holders are `reap_orphans` MAX_REAP=64 and `drain_zombie_list`
  max_flush-bounded frees). The `smp_sched` rendezvous timeout is 1 s
  (≈1000 ticks) — orders of magnitude above measured worst-case hold;
  sustained starvation fails the test loudly instead of hanging
  (fail-closed liveness, documented as the C1 watchdog). Implementation
  hoists logging/allocation out of the reap locked region (or quantifies
  worst-case hold with them inside); TCB generation is uint32_t
  monotonic never reset (wrap past 2^32 creations assumed unreachable).
- AP quiesce flag (covers teardown + snapshot windows): teardown
  (`cleanup_test_tasks`) and `snapshot_create`/`snapshot_restore`
  set an atomic quiesce flag FIRST, then cancel ALL per-CPU armed
  deferred switches (per-CPU `cancel_pending_switch(cpu)`: clear
  atoms + bump generation so an in-flight epilogue fails its
  re-check), then take the global lock, then run, then unlock, then
  clear. Exact order (load-bearing): flag → cancel-arms → lock →
  work → unlock → clear. An epilogue that already passed its
  generation check applies onto still-live targets (nothing is freed
  before the lock is held); one that runs after cancel sees the
  bumped generation and skips. The AP tick checks the flag before
  `try_lock` and parks (no dispatch, no queue touch) while set.
  Deadlock-freedom: the AP never blocks on the BSP (try-only), and
  its in-flight tick body is bounded, so the BSP's blocking take
  always completes. Teardown additionally spares tasks
  current-on-ANY-CPU (scan of all 8 currents — the AP-running
  non-idle case warns loudly as a test bug instead of freeing a live
  stack).
- Its `idle_task_`/`harness` references become own-idle/`is_idle_task()`;
  H2 ring index goes atomic (`__atomic_fetch_add`; torn debug-only
  entries documented; `h2_record` reads the CALLER's CPU generation).
- ALL `on_tick()` global loops gain affinity skips (exact families:
  sporadic replenish + `move_priority` call sites, deadline scan +
  expiry, accounting/WCET/alarm loop, orphan/blocked wedge-repair
  scans): tasks not affine to the ticking CPU are neither mutated nor
  re-queued by that tick. AP-affine sporadic tasks are NOT serviced in
  C1 (documented starvation rule — AP tasks are non-real-time).
- `scheduler_abort_switch_fixup` (shared-TSS rebind) is CPU0-gated:
  on AP it is a no-op (AP runs kernel tasks only; rsp0 irrelevant).
  This covers the writer the affinity clamp misses.

### 3.4.5 Affinity + mailbox semantics (v2 after design audit)

- `TCB.cpu_affinity` bitmask, default `0x1` (CPU0 — every existing task
  behaves identically; AP queues stay empty unless opted in). Children
  inherit the creator's mask (user clamp below applies at inherit time).
- Target = lowest set bit (deterministic; no balancing in C1).
- `set_affinity(task, mask)`: runs inside the quiesce window (flag →
  cancel-all-arms → lock → check+move → unlock → clear), which fences
  the lock-free AP current-apply path the global lock cannot cover:
  with arms cancelled and AP dispatch parked, the "running on another
  CPU" check is stable across the dequeue+enqueue. Empty mask →
  clamp CPU0 + warn; user task (`is_user_`) → force CPU0 + one-time
  warn (shared-TSS limitation); mask bits beyond up-CPUs
  (`1 + ap_count`) → clamp + warn; refuses tasks running on another
  CPU (warn); re-queues if queued (dequeue old target + enqueue new
  target under lock).
- Not-up-CPU rule (v1 left this undefined → own test plan would panic):
  wake targeting a CPU that is NOT up (`target > ap_count`) does NOT
  use the mailbox — it direct-enqueues to the target queue under the
  global lock (no IPI; the task waits like single-core parked work) +
  one-time warn. Mailbox + IPI are used ONLY for up CPUs, so a slot is
  always drained and the overflow panic is unreachable in legitimate
  flows (4-ring bound holds: C1 producers are test code + ISR wakes at
  tick granularity; drain is per-IPI).
- Mailbox entries carry task-ID + generation (irq_delivery `waiter_gen`
  pattern): the consumer validates against `id_table_` and drops stale
  entries instead of dispatching UAF.
- `next_task()` skips candidates not affine to own CPU (defense-in-depth
  alongside enqueue-time targeting).
- Snapshot: `TaskFields` gains `cpu_affinity`; `capture_rqpod`/
  `restore_rqpod`/`rebuild_ready_queue` loop all 8 queues; per-CPU
  currents + per-CPU atoms + mailbox rings are CAPTURED (mailbox
  entries validated against `id_table_` id+magic+generation on
  restore-drain, stale dropped — never dispatched). Mailbox is
  CLEARED on restore (no cross-test wakes). AP current restores from
  capture (validated like mailbox entries); BSP keeps the existing
  RSP-match re-identification. Synchronization half: create/restore
  hold the global lock across the window (they run in BSP task
  context — blocking take is safe) + set the quiesce flag first, so
  the AP cannot try_lock-succeed mid-capture/mid-restore (the v2 race
  is closed by construction, not by luck).
  `clear_switch_globals` clears all CPUs.
  `g_aps_up`/`boot_madt`/`per_cpu[]` are boot-stable plain statics
  outside the snapshot driver (verified: test_isolate rewinds
  PMM/MemPool/tasks/misc/rqpod/user-pages only) — no action.
  Snapshot buffer growth (~25 KB: 8 PODs + currents + atoms + mailbox)
  is verified against the allocator at implementation.

### 3.4.6 Test plan

- `sched_affinity` (single-CPU safe): default mask, lowest-bit
  targeting, empty-mask clamp, user-task clamp, re-queue on change,
  `is_idle_task`, per-CPU queue isolation (enqueue-to-1 while BSP
  runs from 0). Cross-CPU wakes on 0-AP configs exercise the
  not-up-CPU direct-enqueue rule (no mailbox use, no panic).
- `smp_sched` (smp2 variant; 0-AP paths pass on default like
  `smp_bringup`): pin task→CPU1, completion-flag rendezvous,
  `per_cpu[1]` current observes the task, BSP task unaffected,
  re-park + destroy leaves AP idle; cross-CPU BLOCKED move
  (deterministic placement, nothing dispatchable); semaphore IPI wake. At most 1–2 tasks target the AP
  per test (mailbox 4-ring never near full by construction).
- Gates: all targeted + `safe`/`selftest`/`all` (debug) + `all`
  (release) + SIL 3 audit.

### 3.4.7 Explicitly out of scope (later phases)

Load balancing/migration, per-CPU TSS + AP user tasks, AP-side
accounting/deadlines, TLB shootdown, per-CPU deadline monitor,
scheduler statistics per CPU, FPU migration (#151).

### 3.4.8 Audit-driven errata (exact sites — design audit 2026-09-14)

- Reboot/teardown must spare ALL idles AND quiesce the AP (S1: the kill
  loop would terminate the live AP idle every SMP boot; v2: per-test
  teardown vs AP-running tasks): `taskdefs.cpp` reboot kill loop +
  `cleanup_test_tasks` collect loop use `is_idle_task()` (not
  `== idle_task_`) AND spare tasks current-on-any-CPU (loud warn on
  AP-running non-idle = test bug); both run under the quiesce flag
  (AP parks, no dispatch/queue touch during the window).
  `reap_orphans` is safe via TERMINATED-only matching + the index-0
  recreate branch (AP idle never terminates); `task_at(0)` keeps
  returning `idle_task_[0]`.
- `per_cpu_current()` (hardcoded `[0]`) is unified with `cpu_index()`
  (GS_BASE + range check) so `cpu_id()`/`lapic_id()` become truly
  per-CPU; the S3 fallback (0 outside the range) covers early boot.
- `switch_to_task`'s `set_tss_rsp0` call is CPU0-gated alongside the
  fixup gate (same shared-TSS reason).
- `validate_switch` never validates the CURRENT frame (only next) —
  first-dispatch relies on `create()`-built frames (verified); the AP
  idle MUST be built via `create()` (no hand-rolled TCB).

### 3.4.9 Issue #61 delta (RT load balancer + affinity ABI)

§3.4.7's "load balancing/migration" carve-out lands here. Policy:
- `Scheduler::balancer_tick()` (BSP-only, quiesce + try_lock, never
  blocking) migrates queued aperiodic kernel tasks from the busiest
  up-CPU to the idlest while depths differ beyond
  `BALANCER_THRESHOLD` (2), at most
  `BALANCER_MAX_MIGRATIONS_PER_TICK` (2) moves per tick, every
  `BALANCER_TICK_PERIOD` (10) BSP ticks, production runs only
  (`!is_test_active()` — tests drive it directly under IrqGuard).
- Hard exclusions (never migrate): periodic tasks
  (`period_ticks != 0 && != NO_PERIOD`), user tasks (shared-TSS:
  CPU0), idle tasks, tasks current on any CPU, unowned/terminated
  tasks, tasks whose mask lacks the target bit.
- Move = remove + re-pin `cpu_affinity` to the singleton target +
  enqueue (queue==target invariant preserved, same as
  `set_affinity`); only up-CPUs are targets (no stranding on
  never-booted CPUs).
- Affinity ABI: `SYS_SET_AFFINITY` (77) / `SYS_GET_AFFINITY` (78);
  mask is a constraint, placement follows the lowest set bit;
  empty mask clamps to CPU0, user tasks clamp to CPU0, beyond-up
  bits clamp; unknown pid returns a specific error (never panic).
- Still out of scope: per-CPU TSS + AP user tasks, AP-side
  accounting/deadlines, TLB shootdown, per-CPU deadline monitor.

## 4. Invariants

- INV-PC1: gs:0x00/0x08 semantics unchanged — existing syscall_entry/isr
  epilogues need no edits beyond nesting-depth relocation.
- INV-PC2: `isr_nesting_depth` is always accessed relative to gs; a
  grep-level CI check forbids `rel isr_nesting_depth`.
- INV-PC3: an AP that fails to start within its deadline causes panic, not
  silent degradation.

## 5. Test Plan

Single-core first (no QEMU -smp change): class `per_cpu` verifies gs-slot
round-trip and nesting-depth relocation against existing IRQ tests (must pass
unmodified — proves ABI stability). SMP stage (QEMU `-smp 2`):
`ap_bringup_two_cpus` (both report started), `ipi_ping_pong`,
`ap_failure_panic` (block one AP's SIPI vector, expect panic string).
Deadline-monitor histograms recorded per-CPU.

## 6. Open Questions

- x2APIC vs xAPIC mode (MADT flags decide; x2APIC removes the MMIO read
  entirely, strengthening §2's argument for GS_BASE indexing).
- Whether `current_task` migration out of Scheduler globals happens here or
  with the Phase 5 scheduler split — proposed: here, mechanically.
