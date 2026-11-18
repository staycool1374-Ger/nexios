# Design review (rev 6) — issue #582

Scope: plan only (no code exists). Read-only on main (rev `7381dde7b`).
Audited rev-6 deltas against main: `scheduler_tick.cpp` (`:84-157` diagnostic
DEBUG-gated, `:165-167` adopt, `:193-237` STALL DEBUG-gated, `:1030` RMS call,
`:1041-1054` RMS try-skip+adopt, `:1032-1034` note_tick), `scheduler_lock_guard.hpp:63-99`,
`core/global_state.cpp:88,94-95`, `scheduler_registry.cpp` (set_task_ready
`:457-516`, adopts `:389,:727`, setters, registration), `scheduler_queue.cpp`
(`:264-325` incl. `:313`, setters `:415,:434,:462`), `scheduler_smp.cpp:108-149`,
`arch/hal/irq_guard.hpp:113-167`, `sync/park_lock.cpp:99-134,145-198`,
`sync/park_lock.hpp:38-49`, `scheduler_switch.cpp:940-959,1120-1139`,
`scheduler_watchdog.cpp:92-185`, `scheduler_terminate.cpp`,
`scheduler_auth.cpp:36-153`, `scheduler_debug.cpp`, `task/tick_watchdog.hpp`,
asm (x86 `isr_stubs.asm:340,497`; rv64 `syscall_entry.S:328,389,406`;
aa64 `vectors.S` cbz sites), `nexios_config.h:70-71` (`CONFIG_TICK_HZ 1000`).

## FINDINGS

- [S3] Holder-table counting convention unstated (not a bound defect).
  WHY: main has 36 static `SchedulerLockGuard` constructions (34 in release:
  `:86,:105` are `CONFIG_DEBUG_IPC_SCHED`-gated), 7 adopting a preceding
  try_lock (tick `:86,:105,:167,:1054`; smp `:117`; registry `:389,:727`;
  every `scheduler_lock_` try_lock adopts). "26 rows" reconciles only via
  grouping (e.g. auth 3 sites/1 fn, on_tick 3 sites/1 fn, terminate module)
  plus exclusions (`:86,:105` debug-gated; registry `:354` test helper).
  Every site inspected holds a short critical section (list ops, bitmap
  pick, O(128)-capped scans; longest is `:1131` pick+switch, still sub-ms vs
  the 8ms/8-tick window at 1000 Hz); fail direction is safe throughout, and
  spurious-panic rate is QE-visible (`covered_ticks_max`, SCHED+16..19).
  Require at implementation: state the row convention (grouping + exclusion
  list + caller-IF per row) and re-verify against the then-current tree
  (aa64 gate lines already drifted from r5's `:319`).
- [S3] `registry.cpp:465` mislabeled "IF=1 lock-free" (harmless direction).
  WHY: the write sits under `set_task_ready`'s function-local `IrqGuard`
  (`:459`), so IF=0 — exposure is SMALLER than claimed. No safety impact;
  correct the label in-plan.
- [S3] Production detector action for the (E) freeze is log/latch-only
  (accepted; QE-owned). WHY: `[STALL] HALT` (`tick.cpp:233`) is
  `CONFIG_DEBUG_IPC_SCHED`-gated (block opens `:168`; macro OFF for release
  per procedure); `tick_watchdog` fire policy is "log/latch only ...
  panic arm ... never wires it" (`tick_watchdog.hpp:22-28`); harness
  timeouts are test-only. Adequate for an abnormal-by-construction path
  (normal ops release before `reschedule()`: monitor `:141-152`; switch
  release-at-publish `:862`; `:1131` refuses live guards `:1123-1129`) —
  record the gating in-plan so R7 does not assume a release halt.
- [S3] `:165`-fail self-induced counts under task-context `on_tick`
  reentrancy (documented `tick.cpp:94-100`). WHY: the caller's own
  `on_tick` body holds the lock, so the ISR's `:165` try_lock fails and
  double-read agreement counts a covered instant against the live latch.
  Harmless in production (task-context `on_tick` is harness/tests); R7
  resets must cover SCHED+16..19. Noted, no design change.

## Verified sound (no finding)

- Honest service table: release hooks can only live on guard dtor/`unlock()`
  (`lock_guard.hpp:81-99`); depth==0 task holds no guard so no hook fires.
  `depth` writers are guard-only (`lock_guard.hpp:69,79,87,97` + definition
  `global_state.cpp:88`; `irq_guard_depth` likewise guard/test-seam only).
  ExternalDenial/trap-IF=0 service tick-only is structurally forced
  (no other path serves a lockless task; `reschedule()`-entry removal intact).
- Aggregate clock: `covered_ticks` monotonic per latch lifetime (rearm
  preserves; reset only on clear), `since_tick` Warn-window only,
  FailClosed exclusively on `covered_ticks>=8` — the r5 rearm-livelock is
  closed by construction. `:165`/`:1030` structure supports placement
  (held region `:167-816` precedes `:1030`; RMS `:1052` skip handled by
  re-arm+covered++). Double-read sound: 64-bit aligned `{id,generation}`
  loads are single-copy-atomic on all three arches, so agreement implies no
  concurrent rewrite (64-bit generation wrap infeasible); stale-lifetime
  misattribution stays inside the explicit availability-only acceptance
  (fail-closed, `covered_ticks_max` witness). Silent-clear-before-count at
  both sites is stated (`tick: stale→woke→count`; hook: id/gen+BLOCKED
  re-check under adopted lock before Warn/park).
- Trap row: restoration guaranteed by RAII (`IrqGuard` restores `irq_was_`,
  `:155-157`; saturation floor still restores IF; `SyscallTrapScope`
  symmetric). Trap-IF=0 persists only inside the outer scope (bound = scope
  exit) or the (E) abnormal path. Bound-from-IF=1-restoration is sound.
- Bucket-1 exhaustiveness (load-bearing): full `= TaskState::READY` scan
  shows no third IF=1 lock-free READY site. `:313` genuine (IF=1-reachable
  via many lock-free callers); `:465` READY-only but IF=0 (S3 above,
  conservative); all others locked/adopted (`switch:822,838` under lock
  released `:862`; `smp:139` adopted; tick locked; `lifecycle:664` fresh
  TCB; `debug:103` locked; registry `:500` locked; `:528,:575` boot) or
  RUNNING-targeted. Wet-read safety holds via diagnostic-only `cls` +
  silent-clear precedence.
- Bucket-2 spot: BLOCKED publish `sync/park_lock.cpp:107-109` under
  `IrqGuard` as cited. `cls` verbatim rule, priority table
  (stale→woke→hook→clear+count→FailClosed→Warn one-shot), held = FULL
  5-term discriminator, `dump ≤15+≤8` / release-census-always-on shape all
  main-compatible.
- (E): tick frozen ⟹ neither clock accrues is structural (both accrue in
  `on_tick` paths); escalation provably unable to fire during freeze;
  detectors named (`tick_watchdog` checkpoint/note_tick, harness timeouts,
  `[STALL]` HALT with gating per S3 above).
- Closed items uninvalidated: asm defer targets present
  (x86 `jz:340`→`.restore:497`; rv64 `beqz:328`, `gen_skip:389`,
  `abort_switch:406`; aa64 cbz present, lines drifted — standing
  re-verify proviso); deny-absolute intact (`deny_reason` early-returns,
  `ForeignGuard` diagnostic-only); GATE/bit-6 contract change untouched.

## Disposition per r5 item

1. S2 latch coverage/bound: CLOSED. Honesty fixed; clock pinned
   (monotonic `covered_ticks`, rearm-safe); `:165`-fail counting +
   silent-clear-first stated and sound; trap row corrected with guaranteed
   restoration bound; holder table published (row-convention proviso as S3).
2. S3 class-tag wet-read: CLOSED (`cls` diagnostic-only verbatim;
   precedence at both sites; bucket-1 exhaustiveness verified).
3. S3 escalation clock/reset: CLOSED (Warn one-shot latched across rearm;
   FailClosed on monotonic `covered_ticks`; held = full discriminator;
   placement ahead of `:1030`).
4. S3 IrqGuard-freeze witness: CLOSED (detectors named; production-action
   gating recorded as S3).
5. R5 items 1-6 old business (depth slot, asm provisos, arming-exactness +
   30-writer table, reschedule-entry, GATE): stay CLOSED, spot-verified
   uninvalidated above.
6. Process: R7 QE wiring (resets/expected-counts/SCHED+16..19/latch array)
   is the acceptance criterion, not a design finding — carried to R7.

`DECISION: APPROVED`
