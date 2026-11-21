# DESIGN REVIEW 2026-11-21 — issue #598 (r1)

PLAN: audits/plan-598.md (stated tip `main @ 8e7e41807`; verified against actual tip `6dac02fd0`)
SCOPE: design-only review per PROMPT-audit evidentiary standard. No code changed, nothing committed.

## VERIFICATION (each load-bearing claim re-checked in-tree at 6dac02fd0)

1. Top-identity keying — CONFIRMED. `scheduler_tick.cpp:1247-1252`: `++` only when
   `top == starve_level_`, re-seed to 1 on any change, reset to 0 when nothing starvable
   below (`:1242-1246`). A periodic waker with READY window < 20 ticks makes 20 identical
   tops unreachable by construction; the reported 2→5 sawtooth with ~6-tick reset cadence
   is the expected signature. `harness_nonpreempt` early return (`:1189-1196`) freezes the
   count (no inc, no reset) — fail direction is earlier relief, correctly left untouched.
2. Diversion — CONFIRMED. `scheduler_dispatch.cpp:95-126` forces
   `bitmap().highest_starvable_below(top)`; with transient top 20 over backlogged 11 over
   waiting 10, the forced slot lands on 11 (not starved). No starvation-ordering invariant
   requires highest-first: the only such rule is the §3.3 caveat sentence itself, which the
   plan deletes with owner sign-off. `dequeue_level` (`ready_queue_manager.cpp:86-99`)
   serves any level including the lowest; level 0 is refused there, preserving INV-STARVE-5.
3. Option A soundness — SOUND. Victim-keyed reset (reset only when
   `lowest_starvable_below(top) == STARVABLE_NONE`, else `starve_level_ = victim`,
   `++starve_ticks_`, unconditional reset on fire incl. the EDF stand-down branch) has no
   late-relief hole: any continuously-READY victim accumulates to 20 regardless of activity
   above it. Ceiling unchanged (every fire path resets ⇒ `starve_ticks_ <= 20`, 19+1 steady
   state). Zero new state/locks/config: the new helper is a pure bitmap function
   (`lo_mask`/`hi_mask` mirrored, low-word-first `find_lowest_bit`, zero-word guarded —
   S3 from #242 carried: the call must test the word non-zero first since
   `find_lowest_bit(0) == 0` per `bits.hpp:70-72`). Fairness caveat (middle band loses the
   forced slot) is real, disclosed as S2-risk with Option B escalation and owner block —
   see S3-2 below for the one overstated sentence.
4. LTO-only explanation — ACCEPTED as honest. Mechanism is plain shared C++ (no `#ifdef`,
   no arch dependency); LTO shifts only activation length/phase of the prio-20 sporadic
   band against the 1 kHz tick, i.e. whether the 20-tick hole is exercised. The plan does
   not dismiss the flake: it mandates a stash control reproducing the stall plus structural
   fixtures (period < N by construction, stacked bands with zero timing dependence) and
   explicitly forbids "off-arch green" as proof.
5. R4 framing — ACCURATE. `docs/specs/scheduler.md:64-67` promises no lower level denied
   forever; the prio-20 daemon population is real (`taskdefs.cpp:328-377` vfsd/iocd/
   watchdogd/ramdiskd/`drv-*` SS periods 10 at prio 20; `daemon_mgr.cpp:255-257,379-381`
   restart path hardcodes 20/10). Every image running that band satisfies "periodic waker
   above the victim with period < N". §3.3 caveat deletion is flagged as blocking on owner
   sign-off before steps 3-5 — correct gating; the rewrite keeps counter-only,
   EDF-precedence, policy-runtime-reset bullets verbatim.
6. Rejections B-F — ALL SOUND. C: counter-only still spends the slot on 11 under stacked
   bands (T2 stash control proves zero progress) — masking, correctly rejected. D: cause is
   periodicity, and N ≤ 5 breaks the 1-in-20 budget and the LL re-characterization.
   E: waker classification couples dispatch to the daemon subsystem and leaves F2 intact —
   wrong layer. F: bounding interference in the harness masks a production hole and violates
   test sanctity. B deferred (not rejected) as the per-band-fairness escalation — legitimate
   scope control given the S1 surface of new per-CPU state + snapshot-byte-layout risk.
7. Scope/gates/sanctity — COMPLETE. Files: scheduler `{priority_map,dispatch,hpp,tick,
   registry}.hpp/cpp` + `docs/specs/scheduler.md` + `test-history.txt`; snapshot/config/arch/
   RQ untouched; dev ships zero tests, QE owns 3 stub-first fixtures with teardown-before-assert
   and `terminate_and_drain` discipline, and the sacred `scheduler_breaker_lower_progress`
   (`test_scheduler.cpp:769-820`) stays byte-identical. Gates: `scheduler_core` (x86 LTO+NOLTO),
   `scheduler_starvation`, `scheduler_budget`, `scheduler_core` aarch64+riscv64, stash control,
   `check-style` Errors 0, `CONFIG_DEBUG_IPC_SCHED` off, `graphify update`, vault mirror.
   (Off-arch runs only `scheduler_core` — accepted with mitigation, see S3-3.)
8. Retrieval — REAL. Two `query_graph` (one decisive: `find_lowest_bit` exists at
   `arch/hal/bits.hpp:67-75`, `highest_starvable_below` has exactly 2 callers + 0 test refs),
   one `shortest_path` (correctly discarded as ambiguous), one `get_neighbors`, two vault
   searches + full read of the #242 audit. All anchors re-verified `file:line` this session.
   False-close risk closed: T1 greens only under periodic load, T2 greens only with the
   diversion fixed (passes under A or B, fails pre-fix deterministically).
9. #242 S3 carry-forward — ADDRESSED, not re-raised. Interference wording → "one forced tick
   per N (= 20) denied ticks"; `sched_cpu()` clamp → `% CONFIG_MAX_CPUS` added in the same
   moved lines (`sched_cpu()` itself is raw `arch::cpu_index()` at `scheduler.hpp:1340-1342`;
   every per-CPU access in the block must clamp); `nullptr` sentinel → already the code
   (`scheduler_dispatch.cpp:106-125`), kept verbatim.

## FINDINGS

- [S3] audits/plan-598.md:3 — stated tip `8e7e41807` is one commit stale (actual `6dac02fd0`,
  whose own message confirms the battery ran on `8e7e41807`)
  WHY: All cited line numbers re-verified identical at `6dac02fd0` (`tick:1240-1264`,
  `dispatch:95-126`, `priority_map:66-84`, `hpp:1626-1650`), so this is label drift only —
  refresh the tip line before implementation so the issue trail points at the real base.
- [S3] audits/plan-598.md:290 (§3.3 replacement) — "Bands above the victim keep strict
  fixed-priority service" overstates the guarantee
  WHY: When the top never drains, strict service for a middle band is exactly zero (the
  strict pick always takes the top), not merely "no relief" — the honest sentence is
  "bands above the victim receive no breaker relief and remain denied while the top never
  drains"; the middle-band loss is otherwise correctly disclosed (S2 risk + open question
  blocking on owner + Option B escalation), so file the Option-B follow-up issue in the
  same commit that lands the spec rewrite.
- [S3] audits/plan-598.md step 10 — off-arch gates run only `scheduler_core`, not the full
  trio (`scheduler_starvation`, `scheduler_budget`)
  WHY: Mechanism is arch-neutral shared C++ and both new fixtures are structural (no timing
  dependence), which mitigates the gap — but the plan's own parity rule ("never by off-arch
  greenness") cuts both ways: either run the full trio on aarch64/riscv64 or record the
  justification on #598; cheapest is running all three where the classes exist.
- [S3] audits/plan-598.md step 5 (`scheduler_tick.cpp` replacement) — victim-change
  carryover: a newly-appearing victim inherits `starve_ticks_` from its predecessor and can
  fire after only a few ticks of its own waiting
  WHY: Fail direction is strictly early relief (bounded by the same 1-in-20 ceiling, no
  priority/order mutation), so no liveness hole — but state the fail-safe direction in the
  rewritten leading comment so a future reader does not "fix" it into a reset-on-change
  that reintroduces top-identity-style sensitivity through the back door.
- [S3] audits/plan-598.md step 2 — GDB recipe must use raw memory reads, never `call`
  WHY: Already stated in the plan (the #242 TCG learning), restated here because a `call`
  on the stub reads garbage priorities and would misattribute the waker — keep the
  prohibition in the issue comment alongside the tally.

Checks passed without findings: no heap allocation (counter + O(1) bitmap ops + existing
per-CPU statics); lock discipline unchanged (`rate_monotonic_schedule` under
`scheduler_lock_`, no new acquisition, switch/apply paths untouched); no assertion masking
(dev ships no tests; QE fixtures assert progress with pre-fix TIMEOUT controls); INV-2/INV-RQ-1
clean (`dequeue_level` reuse, verbatim eligibility mirror, bounded drop loop); no
critical-section interference (counter-only, EDF precedence via null-gate with reset,
identical debug/release flow); no `#ifdef` added/removed; init + snapshot-restore reset
coverage preserved with no new snapshot bytes and no ResourceTracker delta.

DECISION: APPROVED
