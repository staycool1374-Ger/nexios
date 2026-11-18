# Design re-review (rev 4) — issue #582

Scope: plan only (no code exists). Read-only on main. Audited rev 4 shape
against main: `scheduler_tick.cpp` (gates `:1255-1329`, park `:1336-1372`,
`reschedule` `:1375-1490`, RMS `:1041-1230`, on_tick→RMS order `:1030`,
sweep `:562-620`), `scheduler_lock_guard.hpp:81-99`,
`scheduler_registry.cpp` (balancer `:383-454`, set_task_ready `:493-515`),
`scheduler_smp.cpp` (`mailbox_drain:108-149`, `ap_tick:156-163`),
`scheduler_watchdog.cpp:137-157`, `scheduler_queue.cpp:264-330`,
`core/global_state.cpp:768-913` (validate), `sync/spinlock.hpp:83-107`,
`sync/spinlock_order.cpp:128,208`, `arch/hal/irq_guard.hpp`,
`x86_64/isr_stubs.asm` (`:244-248` force n/a, `:300-340` gen+validate,
`:339-340` test/jz, `.restore:497-499` no-clear, `.abort_switch:455`),
`aarch64/vectors.S` (`:303-319` cbz w0, `.Lswitch_abort:371`, `.restore:379`),
`riscv64/syscall_entry.S` (`:244-248` force consume, `:323-328` beqz a0,
`gen_skip:389`, `abort_switch:406`, `7f:420` TLS-consume, `8f:451` depth-dec).

## FINDINGS

- [S2] Latch arming does not cover the depth==0-denied class. WHY: the
  tail arm requires `depth!=0` (= `scheduler_lock_depth`, per the brief's
  own "SAME per-CPU slot" FailClosed walk), i.e. the BLOCKED caller itself
  holds `scheduler_lock_` — a T2b-edge shape in task context, reachable in
  practice only via re-entrant tick-context `reschedule()`
  (`scheduler_tick.cpp:982`, needs sporadic-exhaustion of a BLOCKED
  current). The broad denied classes — outer-`IrqGuard` bit-2 and
  order-table bit-3 denials with `scheduler_lock_depth==0` (e.g. any
  blocking op denied under an outer guard) — never arm, so they get no
  Warn/streak/escalation; their only stated service ("first tick after
  IF=1" via normal RMS) is best-effort: chained back-to-back holders
  covering consecutive tick instants starve it unboundedly with zero
  telemetry. The "`<=1` tick" bound is single-holder-only. Require: prove
  the depth==0-denied class empty in production, or extend arming to it
  (or bound aggregate lock coverage across tick instants).
- [S3] "depth" in the arm condition is ambiguous as written. WHY: the
  5-term hook discriminator names `irq_guard_depth==0` and
  `spinlock_held_depth==0` separately; the S2 service text treats
  IrqGuard-nested as a latched class (needs irq-depth in the arm
  disjunct), while the FailClosed walk treats it as the scheduler-lock
  slot. Pin which slot(s) the tail test reads.
- [S3] C.3 "C-only, no new asm call" mislabels mandatory asm edits.
  WHY: all three sites apply-any-nonzero today — x86 `:339-340`
  `test rax,rax / jz .abort_switch`, aa64 `:319` `cbz w0,.Lswitch_abort`,
  rv64 `:328` `beqz a0,abort_switch` — so a `2=defer` return mis-applies
  (defect B) unless each branch is widened in asm. "No new call" is true;
  "zero asm changes" is false. The widening is clobber-free (flags/`t0`
  only) so r2 items 3-5 survive, but each diff must be re-verified.
  Pin per-arch defer targets: x86→`.restore` (`:497-499` leaves arm
  pending, correct); aa64→`.restore` (`:379`); rv64→`8f` depth-dec ONLY —
  NOT `gen_skip` (`t3`/`a0` setup clobbered by the C call → garbage
  `scheduler_record_skip` args) and NOT `7f` (steals the pending arm's
  TLS slot into `OFF_TP`, cf. `:389-394`). Same proviso for the rv64
  force peek edit (delete the `:247` `sd zero`; clears move to C
  validate-apply/cancel/RMS-CLR/AP-reset). Resolve "drops clear via
  cancel/RMS-CLR/AP-reset" vs "clear-at-arm-gone" phrasing per path.
- [S3] Escalation transition table under-pinned. WHY: two rules key on
  the same threshold (`>=8` + free → clear+count AND rearm-and-continue
  SCHED+19) with no priority; stale-latch lifecycle unstated (target
  death → validator abort clears the arm, but the `{id,gen,since_tick}`
  entry lingers → spurious Warn + `>=8` clear+count, or spurious
  FailClosed if a legit holder covers that instant). Pin: rule priority,
  silent-clear (never FailClosed) on id/gen mismatch at hook and
  escalation, and `tick_service_count` semantics when RMS `:1052`
  try_lock skips after the clear (count-before-delivery). Pin placement
  inside the on_tick lock-held region (`:165-816`), ahead of the `:1030`
  RMS call — verified that order delivers same-tick dispatch; pre-lock
  placement would not.
- [S3] Arming-exactness invariants must be in-plan. WHY: the tail test is
  exact (verified: `depth!=0` ⟹ lock held on this CPU ⟹ tick try_lock
  fails ⟹ no concurrent state/depth mutation; `depth==0` ⟹ no arm
  regardless of interleaving), but it rests on (i) every `TCB.state` /
  `scheduler_lock_depth` writer requiring the lock and (ii) the
  production arming path + zero-arm-in-health expectation for GATE 1(v).
  State both with grep evidence.
- [S3] FailClosed aggregate-coverage argument missing. WHY: per-site
  C.4 bounds are shown, but 8 consecutive covered tick instants via
  chained holders panics a non-stuck system (fail-safe, availability
  only). Accept explicitly or bound aggregate coverage. C.2 (counters,
  SCHED+16..19, latch array with resets/expected-counts) and C.5
  (`<=15`/`<=8` lines, release census from always-on `holder()`+depth
  only — `spinlock_dump_held` is DEBUG-only) accepted as QE-owned
  commitments.

## Verified sound (no finding)

- Escalation→RMS order: on_tick lock-held body (`:165-816`) precedes the
  `:1030` `rate_monotonic_schedule()` call in the same tick — same-tick
  dispatch after clear holds (modulo RMS try_lock skip, S3 above).
- No false-positive FailClosed on legitimate adopted holds: tick scopes
  run IF=0 and end each tick; task adopted scopes run IF=0 (`balancer:
  383`, IPI-drain IF=0) freezing the tick-measured streak, or are C.4
  sub-tick IF=1 scopes; evaluation needs a running lock-free tick.
- `reschedule()`-entry service removal: correct (entry cannot serve
  one-shots, r3); hook-at-release + tick paths subsume it.
- Normal BLOCKED callers (`monitor_task_entry:143-152` pattern: release
  before `reschedule()`, depth==0) still take the park gate, not the
  latch — latch is residual-only, as intended.
- Obligation (iii) restatement: both release paths hooked
  (`scheduler_lock_guard.hpp:87-88,97-98`); live `unlock()` site
  `registry.cpp:453` funnels through the same hook (retained under its
  outer `IrqGuard:383` + ISR nesting — consistent with the design).
- GATE 1(v) (term_hold==0, predicate table test, one GDB-harness panic)
  matches the accepted precedent (r2 item 8 / r3 item 8).
- R5 row-2 non-discriminating + EDF separation: unchanged, accepted.

## Disposition per r3 item

1. INV-APPLY-3/force: CLOSED, stays closed (deny-absolute; peek +
   C-side clear-at-apply/clear-at-arm-gone coherent; force bypass lands
   before validate on every path).
2. INV-PARK-2 liveness: OPEN, narrowed S1→S2 (mechanism exists for the
   self-hold class; depth==0-denied class unlatched, see S2).
3. x86 discipline: CLOSED + S3 proviso (widen `test/jz` diff re-verify;
   defer→`.restore`).
4. rv64 force/ordering: CLOSED + S3 proviso (peek edit + `beqz` widening
   re-verify; defer→`8f`, not `gen_skip`/`7f`).
5. aa64 torn-publish: CLOSED + S3 proviso (widen `cbz` diff re-verify;
   defer→`.restore`).
6. Fail-closed misses rows 3-6: CLOSED, uninvalidated.
7. A-funnel: CLOSED, uninvalidated (spot-checked: stranded sweep
   `tick.cpp:562-620` and `set_task_ready:493-509` deferral intact).
8. GATE-2 evidence bar: CLOSED (1(v) accepted).
9. Streak escalation: old shape CLOSED; new Warn/FailClosed/RearmContinue
   machine carried as S3.
10. QE wiring: CLOSED for old counters; new counters/SCHED+16..19/latch
    array carried via C.2 as QE acceptance criterion for 1(v).

`DECISION: REJECTED`
