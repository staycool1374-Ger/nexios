# Design re-review (rev 3) — issue #582

Scope: plan only (no code exists). Read-only on main. Audited ONLY the rev 3
deltas against the r2 report; settled items re-checked solely for invalidation.
Grounded on: `task/scheduler_lock_guard.hpp:81-99`, `task/scheduler_tick.cpp`
(gates `:1255-1329`, arm `:1427-1483`, park `:1336-1372`, adopted guards
`:86,:105,:167,:1054`, tail guard `:828`, tick reschedule `:982`, direct-switch
gates `:1120-1138`), `task/scheduler_switch.cpp` (cancel `:88-112`,
terminate guard `:942-947`, publish guard `:1071-1072`, force sets
`:953,:1095`), `task/scheduler_smp.cpp:39-54,115-118`,
`task/scheduler_registry.cpp:385-453,459`, `core/global_state.cpp:768-820`
(drop_arm AP skip `:792-793`), `sync/spinlock_order.cpp:19-28,208-217`,
`kernel.cpp:1870-1888` (panic), `arch/hal/irq_guard.hpp:110-167`.

## FINDINGS

- [S1] INV-PARK-2 still open: the 5-term discriminator has no guaranteed
  service point, so the latch routinely pends to 8-tick escalation while the
  BLOCKED task runs on (the F1 window). WHY: (a) holder releases that should
  service it fail the discriminator in every normal context — ISR adopted
  scopes (`scheduler_tick.cpp:86,105,167,1054`, `isr_nesting!=0` fails term 1);
  trap-context releases (`scheduler_switch.cpp:947` terminate guard, trap
  gates enter IF=0, fails the IF==1 term); IrqGuard-nested releases (any live
  outer `IrqGuard` fails the depth term — note the sole live `unlock()` site
  `scheduler_registry.cpp:453` runs in the ISR-driven balancer path, failing
  term 1 anyway); (b) `reschedule()` entry cannot serve one-shot (non-spinning)
  BLOCKED callers — they never re-enter (`tick.cpp:1424` spinner is the
  exception, not the rule), and entry większą częścią runs under the arm's own
  `IrqGuard` (`:1427`) which fails the depth term for guarded callers;
  (c) escalation does not park: AbortRestore cancels the arm + runs drop_arm
  surgery but never deschedules the running BLOCKED task, and it can cancel a
  *healthy* pending arm (lock-FREE at sample time ≠ arm stale), delaying the
  dispatch that would have descheduled the zombie. The arming site itself
  ("bit 6 sole failing bit") is unpinned in the brief — which gate, evaluated
  where (call-time base `tick.cpp:1255-1284` has bits 0-5/9-13; lock-held at
  call time is impossible per the T2b contract check `:1387-1399`, so bit 6
  must be apply-time, but no site is stated) — so closure is unverifiable as
  written.
- [S3] Proof obligation (iii) is factually false as stated. WHY:
  `SchedulerLockGuard::unlock()` has a live production call site
  (`task/scheduler_registry.cpp:453`, guard adopted at `:389`); only
  `owns_lock()` is call-site-free in production (test-only use in
  `test_sync_err_api.cpp:343-361`). No mechanism hole — both release paths
  (`scheduler_lock_guard.hpp:87-88` dtor, `:97-98` unlock) are hooked, and no
  raw `scheduler_lock_.unlock()` exists (all 11 direct uses are
  `try_lock`/`holder` only) — but the obligation must be restated as
  "all releases funnel through the two hooked points" instead of "zero
  `unlock()` sites".
- [S3] New counters lack QE wiring. WHY: r2 item 10 closed on R7 gates naming
  resets + expected-count rows; rev 3 adds `lockpark_pending_isr_count`,
  SCHED+17, `[S582-TERM-HOLD]`, and the defer-action states with no
  corresponding reset/expected-count naming. Carry-forward, same treatment.
- [S3] `scheduler_apply_allowed()` call site unstated; r2 items 3-5 depend on
  it. WHY: if the predicate/actions live inside the existing C call
  (`scheduler_validate_pending_switch`, `core/global_state.cpp:768`), the
  x86/rv64/aa64 asm discipline closures are untouched; a new asm gate call
  would reopen all three (clobber/ordering/IF=0 rules). State "C-only, no new
  asm call" explicitly.
- [S3] The 26-site boundedness list must be in-plan with adopted scopes
  included. WHY: obligations (i)/(ii)/(iv) are developer-checkable (grep-able
  structure verified above), but the long-hold adopted scopes
  (`scheduler_tick.cpp:86,105,167,1054`, `scheduler_smp.cpp:117`,
  `scheduler_registry.cpp:389`) need per-scope worst-case bounds or the
  IF=0-clock-freeze argument (tick-held scopes run with interrupts masked,
  so tick-count escalation cannot accrue against them); task-context adopted
  holds need explicit bounds. FailClosed errs safe so this is plan
  completeness, not a mechanism defect. 8-tick vs sub-100µs margin is ample
  once the list is pinned.
- [S3] FailClosed dump needs a bound + release/DEBUG split. WHY: epilogue
  dumping itself is precedented (`[H2-ABORT]` unconditional at
  `core/global_state.cpp:893-907` with IF=0; depth>2 dump at `:643-666`;
  multi-line ISR dumps at `scheduler_tick.cpp:209-237,323-381`) and `panic()`
  (`kernel.cpp:1870-1888`: cli, dmesg, hlt loop) is ISR-safe, so no S1/S2 —
  but "H2 + op-ring tails" is unbounded as stated, and the held-lock census
  (`spinlock_dump_held`, `sync/spinlock_order.cpp:248-270`) reads DEBUG-only
  `g_held` (`:21-28`) while only `g_held_depth` (`:19`) is always on. State
  max lines and the release-build census source (depth + `holder()` RA only).

## Disposition per r2 finding

1. INV-APPLY-3 original defect: CLOSED, stays closed — "deny leaves arm+force
   (rearm)" preserves deny-absolute; read-only `holder()` (`spinlock.hpp:86-87,
   93-95`) keeps the predicate ISR-safe. Dead-owner recovery S1: CLOSED by
   removal (no lock-recovery, no TCB identity); dead-holder spins are argued
   away by (i)+(ii), with FailClosed as the backstop if the argument fails.
2. INV-PARK-2: OPEN (S1 above). ISR-arming half fixed (bit-6-only latch, no
   ISR-context arm); `unlock()` bypass half fixed structurally (both release
   paths hooked); liveness half fails — no guaranteed service point.
3. x86 gate discipline: CLOSED, uninvalidated — subject to the S3 call-site
   statement (no new asm call).
4. rv64 force/gate ordering: CLOSED, uninvalidated — peek/no-consume +
   deny-rearm + clear-at-apply/clear-at-arm-gone-exit retained; same S3
   proviso.
5. aa64 torn-publish: CLOSED, uninvalidated — same S3 proviso.
6. Fail-closed misses rows 3-6: CLOSED, uninvalidated. Row 7 (detector +
   deadline escalation): detector-stays-0 + Warn/AbortRestore/FailClosed is
   fail-closed, accepted, but row 8 (park service per (2)) inherits the OPEN
   S1. New-counter QE wiring folded into the S3 above.
7. A-funnel table: CLOSED, uninvalidated — row-2 explicitly non-discriminating
   and GATE 2(ii) scoped to DISCIPLINE is the honest reading the r2 report
   asked for; EDF separation (own lists/tags, no cross-list claim) is
   consistent with the sweep skipping `edf_eligible` (`scheduler_tick.cpp:573-578`).
8. GATE-2 evidence bar: CLOSED, uninvalidated — GDB/fault-harness exercise for
   the FailClosed panic matches the precedent of unexercised-in-class panic
   paths (`[STALL]` HALT, canary `panic()`); acceptable for a never-fire-in-
   health path.
9. Streak escalation: CLOSED, uninvalidated — abort reduced to cancel +
   BSP-only drop_arm matches `global_state.cpp:792-793`; streak/latch reset on
   abort/escalation stated; SCHED+17 (escalation-armed) vs SCHED+18 (Warn) split
   is coherent.
10. QE/counter wiring: CLOSED for old counters, EXTENDED by S3 for the new
    counters (see S3).

## Required changes before implementation

1. INV-PARK-2 liveness (S1): pin the arming site (which gate, which bit 6,
   evaluated where); exhibit one service point that is *guaranteed* to fire
   for a one-shot BLOCKED runner in each holder context (ISR-adopted,
   trap-IF=0, IrqGuard-nested) — e.g. defer the park to a point where task
   context with IF=1 is guaranteed rather than dropping it at release — or
   prove the 8-tick escalation itself deschedules the zombie (AbortRestore
   must park/repair the runner, and must not cancel healthy arms); re-audit.
2. Restate obligation (iii) (S3); name new-counter resets/expected counts
   (S3); state the C-only call site (S3); publish the 26-site list with
   adopted-scope bounds (S3); bound the FailClosed dump + release census
   source (S3).

`DECISION: REJECTED`
