# Design re-review (rev 5) — issue #582

Scope: plan only (no code exists). Read-only on main. Audited rev 5 deltas
against main: `scheduler_tick.cpp` (gates `:1255-1329`, park `:1336-1372`,
`reschedule` `:1375-1490`, RMS `:1041-1230` with CLR `:1142-1162`,
on_tick `:165-816` held region / tail lock-free before `:1030`),
`scheduler_lock_guard.hpp:81-99`, `scheduler_switch.cpp`
(cancel `:88-112` clears force `:102`; terminate `:946-956,1094-1096`
sets force), `scheduler_smp.cpp` (AP reset clears force `:54`),
`global_state.cpp` (drop_arm `:783-820`, AP skip `:792-793`, all
clearers clear force), `park_lock.cpp` (deny reasons `:145-198`,
BLOCKED publish `:107-109`), `scheduler_registry.cpp`
(`set_task_ready:457-516`, BLOCKED-free READY sites `:465,:500`),
`scheduler_queue.cpp` (`enqueue_ready:264-325`, READY site `:313`),
`irq_guard.hpp` (depth/owner semantics), x86 `isr_stubs.asm`
(`.depth_ok:236`, gen `:271-298`, validate `:322-340`, `.restore:497-499`
no-clear), aa64 `vectors.S:302-380`, rv64 `syscall_entry.S:244-248`
force-consume, `:328` gate, `gen_skip:389`, `7f:420` TLS-consume,
`8f:451` depth-dec.

## FINDINGS

- [S2] Per-context guarantee table overclaims; the depth==0
  (ExternalDenial) class has no hook and its tick bound is
  single-holder-only. WHY: (1) hooks exist only on
  `SchedulerLockGuard` dtor/`unlock()` (`scheduler_lock_guard.hpp:87-88,97-98`).
  A task denied with `scheduler_lock_depth==0` holds no scheduler lock,
  so no hook ever fires for it — "plain-task→S1 hook guaranteed" is
  false as stated; IrqGuard/order-table releases are not hooked.
  Actual service for the whole extended class is tick-only.
  (2) "first tick after IF=1 / after release" is a single-holder
  bound: chained holders covering consecutive tick instants starve it
  unboundedly with only `covered_max` as witness, and the design's
  answer is panic-on-healthy (availability fail-safe). That acceptance
  is legitimate ONLY with the per-site holder-bounds premise, but rev 5
  states the conclusion ("holders ≤sub-100µs or clock-frozen") without
  the in-plan 26-site boundedness list r3/r4 required (adopted scopes
  `tick.cpp:86,105,167,1054`, `smp.cpp:117`, `registry.cpp:389`
  included) — assertion, not evidence. (3) "Trap-IF=0→epilogue
  apply" conflates mechanisms: the latch arms when
  `candidate==null`, i.e. NO deferred-switch arm is published, so the
  trap epilogue has nothing to apply; service is next-tick-only, and
  the bound runs from IF=1 restoration, not from arming — unstated.
  (4) `covered_ticks` accounting across the two try_locks is unpinned:
  ticks where on_tick `:165` fails never enter the held region, so no
  escalation-site count accrues; ticks where RMS `:1052` skips after a
  clear are handled by re-arm+covered++ (stated), but which counter
  drives "armed≥8" (wall `since_tick` vs monotonic `covered_ticks`)
  and whether rearm's `since_tick` reset restarts the FailClosed clock
  under chronic contention (threshold livelock: rearm-forever with no
  panic) is not pinned. Self-hold class is unaffected (tick try_lock
  fails ⟹ no concurrent mutation); ExternalDenial arming is exposed.
- [S3] Class-tag wet-read needs an explicit no-branch rule. WHY: the
  arming test runs at IF=1 after the arm-scope guard closes, so a tick
  can preempt between scope-close and the tag/depth sample; for
  ExternalDenial the tick CAN acquire the lock and wake the candidate
  (BLOCKED→READY) before the latch stores. Harmless ONLY if (i) `cls`
  gates no service/clear/panic decision (diagnostic-only, grep-able)
  and (ii) woke silent-clear (state!=BLOCKED) runs BEFORE Warn/hook at
  the hook AND before count/Warn/FailClosed at the tick. The priority
  table lists the order but does not state hook-side placement (no hook
  fires for ExternalDenial anyway) or that `cls` is unread by every
  decision predicate — state it verbatim.
- [S3] Escalation clock/reset semantics under-pinned. WHY: rearm resets
  `since_tick` — state whether Warn (`≥1`, one-shot latched?) is reset
  by rearm (storm re-warns: log-spam only, acceptable if bounded by the
  +19 count; or one-shot: state it), whether FailClosed keys on
  monotonic `covered_ticks` (rearm-safe) vs resettable `since_tick`
  (livelock, ⟹ S2), and that stale/id-gen silent-clear precedes
  threshold tests at BOTH sites (hook: id/gen + BLOCKED re-check under
  the adopted lock; tick: same before count). Also pin "held" in
  "rearm(free)/FailClosed(held)": full 5-term discriminator state, not
  scheduler_lock_ alone (order-table-held-forever must panic, not
  rearm-loop).
- [S3] IrqGuard-self-hold freeze has no witness. WHY: latched task holds
  the outer guard with IF=0 ⟹ tick frozen ⟹ neither `since_tick` nor
  `covered_ticks` accrues; release promptness of the abnormal
  hold-across-reschedule path is unproven (normal ops release before
  `reschedule()` per `monitor_task_entry:143-152`; this path is
  abnormal by construction). Accepted as watchdog territory, but name
  it: latch escalation provably cannot fire here; state the covering
  detector (watchdog/`g_depth_sat`/IF=0-hold bound) or bound the
  hold-across-`reschedule()` window for the denied class.

## Verified sound (no finding)

- Depth-slot pinning: tail reads `scheduler_lock_depth` only; the 5-term
  discriminator (self-hold/irq-depth/spin-depth/isr-nesting/IF +
  tick/idle/BLOCKED/id-gen) gates servicing only — resolves the r4
  ambiguity as written.
- Asm widening targets: x86 `cmp eax,2→.restore` (preserves outer-ISR
  arm per `:497-499` no-clear contract); aa64 `cmp w0,#2→.restore`
  (`:379` leaves arm pending); rv64 `li/beq→8f` ONLY — `gen_skip`
  clobbers `t3`/`a0` across the C call (`:293-304` setup) and `7f`
  steals pending TLS (`:420-450`), both verified against main text.
  rv64 force-peek deletion (`:247` `sd zero`) + C-side clears
  co-located with every arm-clearer (cancel `:102`, RMS-CLR
  `tick.cpp:1159`, AP-reset `smp.cpp:54`, validator-abort via
  `drop_arm→cancel`) is coherent; "defer keeps, arm-gone needs no
  action" holds because every clearer already clears force (new
  apply-clear is added C code — diff re-verify at implementation).
  Rev-4 clear-at-arm-gone asm withdrawal accepted (C `cancel_` covers).
- Arming-exactness core: only IF=1 lock-free READY writers are
  `registry.cpp:465` and `queue.cpp:313` (both set READY, never
  BLOCKED — spot-verified); `depth` written only by guard
  ctor/dtor/`unlock()`; SelfLock state read frozen (tick try_lock
  fails). Disclosure of remaining BLOCKED writers
  (`park_lock.cpp:109`, `ipc.cpp:1606`, `pager_registry.cpp:595`,
  `tick.cpp:402` monitor wake under lock, `debug.cpp:78` under lock)
  belongs in-plan as the 4-bucket table, not as re-verification here.
- `reschedule()`-entry service removal stays correct; sole production
  arm site + zero-arm-in-health GATE 1(v)
  (`arm==0, covered_max==0, escalated==0`; `serviced_tick>0` only in
  injection) is the right shape; bit-6 deletion is a clean QE-visible
  contract change (no bit-6 exists in the main gate bitmap
  `0-5,9-13`).

## Disposition per r4 item

1. S2 latch coverage (depth==0-denied): OPEN, narrowed — coverage
   extended (all BLOCKED denials arm; no silent class remains) but the
   BOUND is unproven: no hook exists for ExternalDenial (S2 above),
   single-holder "first tick" does not survive chained holders, and the
   aggregate panic-acceptance lacks the in-plan 26-site holder-bounds
   list. Provide the list + pin the escalation clock (`covered_ticks`
   monotonic driving FailClosed; rearm preserves it) or bound
   aggregate coverage.
2. S3 depth ambiguity: CLOSED (slot pinned to `scheduler_lock_depth`;
   discriminator gates servicing only).
3. S3 asm mislabel: CLOSED, proviso carried (widening diffs re-verified
   at implementation; targets above).
4. S3 escalation table: PARTLY OPEN, carried as S3 (priority order
   accepted; pin hook-side silent-clear placement, Warn one-shot vs
   rearm reset, FailClosed clock monotonicity, "held" = full
   discriminator).
5. S3 arming-exactness: CLOSED on mechanism, proviso carried (publish
   the 30-writer/4-bucket table in-plan; the two READY-site claim and
   depth-writer claim verified here).
6. S3 aggregate coverage: OPEN, carried (verbatim fail-safe acceptance
   stated, but without the holder-bounds list it does not discharge
   the S2 bound; `covered_max` witness + counter resets via
   `lockpark_reset_for_test()` accepted as QE criteria).

## Required changes before implementation

1. Correct the service table (no S1 hook for depth==0; tick-only with
   single-holder bound), publish the 26-site holder-bounds list, and
   pin the escalation clock (monotonic `covered_ticks` drives
   FailClosed; rearm never resets it; `:165`-fail ticks counted or
   proved impossible while armed).
2. State verbatim: `cls` is diagnostic-only (no decision predicate
   reads it); stale/woke silent-clear precedes thresholds at both
   sites; Warn one-shot semantics across rearm; "held" = full
   discriminator; IrqGuard-self-hold freeze covering detector.

`DECISION: REJECTED`
