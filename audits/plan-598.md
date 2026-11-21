# PLAN: breaker observation keyed to the victim band, not the top level (issue #598)

Tip at planning time: `main @ 8e7e41807`. Scope: scheduler policy only. No config,
no new lock, no new state, no test edits, no arch-conditional code.

## RETRIEVAL (audit check-7)

- `graphify query_graph "starvation breaker consecutive quiet ticks scheduler lowest priority progress"`
  (BFS d=3, 373 nodes) → USED: anchored `Scheduler`, `current_task`, `reschedule`,
  `ReadyQueueManager`, `PriorityMap`, `dequeue_ready` and the
  `reset_starvation_breaker` seed, which confirmed the observation state is a pair of
  Scheduler statics (policy-runtime), not a TCB field — so the fix cannot touch the
  TCB/snapshot byte contract. IGNORED: `idle_monitor`/`IdleScanProgress` and every
  `statemachine/*` node (workflow tooling, no bearing on dispatch policy).
- `graphify query_graph "next_task_below highest_starvable_below PriorityMap find_lowest_bit"`
  (DFS d=3, 62 nodes) → USED, decisive: `find_lowest_bit()` already exists in
  `src/kernel/arch/hal/bits.hpp:67-75` (`__builtin_ctzll` with a portable fallback),
  so the lowest-band query is O(1) with no new asm; and `highest_starvable_below` has
  exactly two callers (`scheduler_dispatch.cpp:105`, `scheduler_tick.cpp:1243`) and zero
  test references → a rename leaves no dead code and breaks no fixture. IGNORED:
  `test_sched_helpers.hpp`/`UniquePtr` nodes (fixture scaffolding).
- `graphify shortest_path "starvation-breaker" "on_tick reschedule next_task"` → 6 hops
  with an ambiguity warning (it matched a Python `starvation_gap` symbol).
  DISPOSITION: ignored as evidence, used as a warning — "starvation" is overloaded
  across `statemachine/` and `src/kernel/`; every step below is anchored to
  `file:line`, never to a graph label.
- `graphify get_neighbors("Scheduler")` → USED to confirm nothing outside the
  scheduler (admission `adm_*`, balancer, park, IPC) reads `starve_level_`/
  `starve_ticks_`; IGNORED the ~200-entry define list.
- `obsidian_search_vault "starvation breaker"` (vault `jarvis`) → 2 hits, both
  `specs/microkernel-transition.md` (:519 "the starvation breaker excludes level 0",
  :780 "Idle and the priority-0 reaper are both outside the starvation breaker") →
  USED as a normative contract: the level-0 exclusion must survive the target change
  (INV-STARVE-5, test T3).
- `obsidian_search_vault "starvation"` → 50+ hits; READ
  `audits/report-2026-09-29T153000Z.md` (the #242 audit) → USED: its three S3s are
  carried forward so this cycle cannot re-earn them — (a) the interference bound must
  read "one per N", not "(N+1)/per-21"; (b) `sched_cpu()` needs
  `% CONFIG_MAX_CPUS` defense-in-depth; (c) `next_task_below` must keep the
  `nullptr` sentinel (already fixed at `scheduler_dispatch.cpp:125`). IGNORED:
  `report-2026-09-14T06-59-02Z.md`/`07-03-37Z` "R4" findings — that `R4` is a
  per-CPU-SMP lock-order label, a different invariant with the same name.
- `obsidian_search_vault "242"` → no additional #242 artifact beyond the above.
- Check-7 anchors verified in-tree this session:
  `scheduler_tick.cpp:1086` (`rate_monotonic_schedule`), `:1097` (try_lock skip),
  `:1176-1196` (harness_nonpreempt early return), `:1204-1224` (H2 arm CLR),
  `:1226-1265` (the breaker block), `:1266-1289` (pick/switch);
  `scheduler_dispatch.cpp:63-126` (`next_task`/`next_task_below`);
  `scheduler.hpp:1626-1652` (fields + contract); `priority_map.hpp:44-84`
  (`IDLE_BAND`/`STARVABLE_NONE`/window helper); `arch/hal/bits.hpp:67-75`;
  `scheduler_registry.cpp:76-79,334-340,614` (defs / reset / init);
  `scheduler_snapshot.cpp:231` (restore reset); `test_scheduler.cpp:755-820,844`
  (the sacred #242 test); `test_registry.cpp:385,411,413,741,748,750` (classes
  `scheduler_core`, `scheduler_budget`, `scheduler_starvation` all exist);
  `taskdefs.cpp:318-377` (prio-20 daemon band), `daemon_mgr.cpp:255-257,379-381`
  (restart path hardcodes 20 / period 10); `docs/specs/scheduler.md:64-67,287-312`
  (R4 + §3.3).

## MEASURED FINDINGS (establish, do not assume)

**F1 — the counter measures top-level stability, not victim waiting.** In
`scheduler_tick.cpp:1238-1265`: reset when
`top == 0 || highest_starvable_below(top) == STARVABLE_NONE`; increment only when
`top == starve_level_`; any other top change re-seeds to 1 (`:1247-1252`). So
`kStarvationBreakerTicks` really means "20 consecutive ticks with an *identical*
top level". A periodic higher-priority waker whose READY window is shorter than 20
ticks makes that unreachable by construction — exactly the GDB sawtooth (2→5,
reset cadence ~6 ticks).

**F2 — a second, independent defect: the forced pick is diverted.** The forced
target is the *highest* level below `top` (`next_task_below`,
`scheduler_dispatch.cpp:95-126` via `highest_starvable_below`). When the transient
top is the prio-20 daemon, the highest level below it is the continuously-backlogged
prio-11 band — a band that is *not starved at all* — so even a firing spends the one
reserved slot on the de-facto top and band 10 stays denied. Fixing only the counter
therefore yields probabilistic progress (the harness advances only on rounds whose
firing tick lands while the daemon is blocked) and zero progress under a stacked
backlogged band. Both defects must be fixed together.

**F3 — the prio-20 population is daemons, and the exact one does not matter.**
`taskdefs.cpp:328-377` boots vfsd `SS(2,10)`, iocd `SS(3,10)`, watchdogd
`SS(1,10)`, ramdiskd `SS(2,10)` and the `drv-*` PDs `SS(1,10)` at
priority 20 / period 10; `daemon_mgr.cpp:255-257,379-381` hardcodes 20 / 10 on every
restart. The observed ~6-tick READY cadence is the sporadic-server
activation/replenishment interleave of that band (`sporadic_server.cpp:143-180`),
not a 6-tick task. Step 2 pins the identity as *evidence*; no step may depend on it —
naming a daemon is the failure mode this change removes.

**F4 — the observed shape is exactly "transient 20 over backlogged 11 over
waiting 10".** Harness band 10 while the suite is active
(`taskdefs.cpp:319-326`); the failing test creates its two forever-spinners at
`me->priority + 1 = 11` (`test_scheduler.cpp:774-782`).

**F5 — ticks that take the harness_nonpreempt early return do not touch the
counter** (`scheduler_tick.cpp:1189-1196` returns before `:1238`). Pre-existing:
the count freezes (no increment, no reset) across such ticks. The fail direction is
safe in the only way that matters — a frozen count can make relief *earlier*, never
later. Not changed here; documented as S2.

**F6 — two prior-audit S3s are still live in the touched lines:** the raw
`sched_cpu()` index (`:1240`) and the interference-bound wording duty in the
spec. Both are discharged because the same lines move.

**F7 — LTO-only is a manifestation, not a codegen defect (established, not
assumed).** The identity-keyed counter can only fire if the top stays identical for
20 consecutive evaluations. Whether a prio-20 activation lands inside each 20-tick
window is a function of (a) how long that activation computes and (b) the phase of
the harness's 5 ms sleep wakeups against the 1 kHz tick — both shifted by inlining
and code layout, i.e. by LTO. LTO therefore decides *whether the hole is exercised*,
not *whether it exists*: the mechanism is plain C++ shared by all three arches with
no `#ifdef` and no arch dependency. Consequence: parity must be proven by a fixture
that reproduces the shape *structurally* (period < N by construction, stacked bands
by construction), never by "off-arch happened to be green". This is why the QE
fixtures below use a periodic waker and a stacked pair instead of relying on daemon
timing.

**F8 — R4 is currently violated in every shipped image.**
`docs/specs/scheduler.md:64-67` promises "Strict RMS must not deny every lower level
forever". Every image that runs the standard prio-20 daemon band satisfies "a
periodic waker above the victim with period < N". That is the S1 framing: a
liveness hole in the policy, not a flaky test.


## DECISION BRIEF (owner options — shape per PROMPT-dev.md)

**Option A — victim-keyed observation + lowest-band forced pick (RECOMMENDED).**
How: reset only when *no* ready level exists strictly below the current highest
ready level (nothing can be denied); otherwise increment, independent of top
identity. Forced pick = the *lowest* eligible ready non-idle band below top (new
O(1) `lowest_starvable_below`, an exact mirror of the existing helper).
Guarantee: a continuously-READY band is dispatched within N ticks of becoming ready,
regardless of what happens above it; ceiling unchanged at ≤ 1 forced tick per 20.
Pros: two-condition predicate + one mirrored helper; no new state, lock or config;
deletes the timing sensitivity instead of compensating for it. Cons: documented
semantics change — the §3.3 caveat "levels below the highest starved level keep
classic fixed-priority semantics" must go, and a *middle* continuously-backlogged
band no longer receives the forced slot (it keeps strict service whenever the top
drains). Reversibility: single commit; the helpers are exact mirrors, so a revert
restores byte-identical behaviour. Audit/S1: touches only the relief slot of the
strict path — no priority write, no IRQ window, no lock, no snapshot field; the S1
surface is "the forced dispatch may now target a lower band", mitigated by the
verbatim eligibility/dequeue mirror and the unchanged 1-in-N ceiling.

**Option B — round-robin across ready starved bands (strongest fairness).**
How: keep the highest-band helper, add a per-CPU cursor advanced on every firing,
force the next ready starvable band in rotation. Guarantee: every starved band
served within k·N. Pros: no caveat lost, middle bands served. Cons: new per-CPU
cursor (still static policy-runtime, still lock-free), larger audit surface, the
bottom band waits k·N, and the spec's flat "20 ms ceiling" becomes k·20 ms (must be
re-worded per band). Reversibility: additive, independent commit. Audit/S1: higher
(new state that must be reset on init + snapshot restore and must never enter the
snapshot byte layout). Verdict: not chosen now; documented escalation path if the
owner wants per-band guarantees — QE test T2 passes under either.

**Option C — counter fix only, keep the highest-band target. REJECTED.** Not
sufficient (F2): progress becomes phase-dependent and is deterministically absent
under a stacked backlogged band (T2's stash control proves it). Trading a
deterministic hang for a probabilistic one is not a liveness guarantee.

**Option D — retune N. REJECTED.** The cause is periodicity, not magnitude: N > 5
never fires (max observed stable run 5); N ≤ 5 degenerates into "force every 5
ticks", breaking the 1-in-20 budget and the LL re-characterization. The measured
property is wrong, not its threshold.

**Option E — exempt periodic/daemon wakes from the reset. REJECTED.** Requires
waker classification (period / SS-mode introspection) on the dispatch path, couples
the scheduler to the daemon subsystem, and still leaves target diversion (F2)
intact. Wrong layer: the breaker should observe the victim, not learn about daemons.

**Option F — bound daemon interference in the harness (test-side). REJECTED.**
Masks a production hole inside the fixture; test sanctity forbids weakening the
observation, and the defect reaches every shipped image.

## AFFECTED FILES

- `src/kernel/task/priority_map.hpp` — rename `highest_starvable_below` →
  `lowest_starvable_below`; same window/masks, low-word-first scan with
  `find_lowest_bit`; doc block carries the lowest-victim rule and the level-0
  exclusion contract.
- `src/kernel/task/scheduler_dispatch.cpp` — `next_task_below()` queries the lowest
  band; eligibility mirror, drop loop, `current_task()` exclusion and `nullptr`
  sentinel unchanged; comment rewritten.
- `src/kernel/task/scheduler.hpp` — doc blocks for `kStarvationBreakerTicks`/
  `starve_level_`/`starve_ticks_` (`:1626-1640`) and `next_task_below`
  (`:1641-1650`) restated as "victim band + consecutive denied ticks". No signature,
  no new member.
- `src/kernel/task/scheduler_tick.cpp` — the breaker block (`:1226-1265`):
  victim-keyed accumulation, `% CONFIG_MAX_CPUS` clamp, EDF stand-down and fire-reset
  preserved; the `top == starve_level_` branch is deleted.
- `src/kernel/task/scheduler_registry.cpp` — comment only at `:76-79` (field
  meaning); `reset_starvation_breaker()` logic unchanged.
- `docs/specs/scheduler.md` — §3.3 (`:287-312`) and R4 (`:64-67`) rewritten to
  the victim rule; interference bound restated "one forced tick per N (= 20) denied
  ticks (20 ms ceiling at 1 kHz)".
- `test-history.txt` — one row per gate run, including the stash control.
- NOT touched: `src/kernel/test/**` (test sanctity — QE owns new fixtures),
  `scheduler_snapshot.cpp` (no new state to reset), `nexios_config.h` (no new
  knob), any arch file, `ready_queue_manager.*` (its `:88-92` idle-band note stays
  true).

## INVARIANTS & CONCURRENCY BOUNDARIES

- **INV-STARVE-1 (interference ceiling).** The breaker fires at most once per
  `kStarvationBreakerTicks` evaluations: the increment happens once per
  `rate_monotonic_schedule()` pass and *every* firing path resets both fields
  unconditionally — including the EDF stand-down branch. Therefore
  `starve_ticks_ <= 20` always (no wrap, no saturation needed) and steady state
  stays 19 strict + 1 forced.
- **INV-STARVE-2 (state ownership).** `starve_level_`/`starve_ticks_` are
  written only inside `rate_monotonic_schedule()` under `scheduler_lock_`
  (`:1097` try_lock, skip on contention) and by `reset_starvation_breaker()`
  (`scheduler_registry.cpp:334-340`) at init (`:614`) and snapshot restore
  (`scheduler_snapshot.cpp:231`). One owner per CPU index; no atomics. **No new
  field** — adding one would enter the snapshot byte contract and ResourceTracker
  accounting, which this change must not do.
- **INV-STARVE-3 (counter-only).** No `task->priority`/`base_priority` write: PI
  (§11.5), admission bases and WCET accounting untouched (§11.4 respected by
  construction — the breaker never mutates a queue order or a priority).
- **INV-STARVE-4 (INV-2 / INV-RQ-1).** The forced pick uses `dequeue_level` exactly
  like the strict pick, so the returned TCB is already unlinked before
  `switch_to_task`; the eligibility predicate is mirrored verbatim (READY/RUNNING,
  affine to this CPU, never current); the drop loop is bounded (each iteration
  unlinks ≥ 1 node or clears ≥ 1 stale bit).
- **INV-STARVE-5 (level-0 exclusion, vault contract).** The mask window remains
  `[1, prio)`; idle (band 0) and the prio-0 reaper are never forced targets
  (`specs/microkernel-transition.md:519,780`).
- **INV-STARVE-6 (deferred-switch discipline).** The breaker only chooses `next`;
  the arm/apply path, INV-APPLY-1/3 and the H2 CLR block above it (`:1198-1224`)
  are untouched — no lock across a switch (§11.1), no new IRQ-off window, no change
  to `switch_to_task`.
- **INV-STARVE-7 (debug/release parity, §6).** No `#ifdef` added or removed; control
  flow identical in both builds.
- **Cross-arch.** No arch-conditional code; `find_lowest_bit` is already provided
  on all three arches.


## STEP-BY-STEP CHANGES

1. **Baseline.** `git branch --show-current` must be `main`; record
   `git log --oneline -1` and `git status --porcelain` (clean besides
   `audits/plan-598.md`).
2. **Pin the waker identity (evidence only).** Run the GDB batch recipe
   (`tools/meson/gdb_run.py debug-test --arch x86_64 --build debug --class scheduler_core --gdb-script <script>`)
   with a synchronous breakpoint at `kernel::Scheduler::switch_to_task` printing the
   target's `priority`/`id`/`name` via raw memory reads (never `call` on the
   TCG stub, #242 learning), plus a breakpoint after `scheduler_tick.cpp:1248`
   printing `top`/`starve_level_`/`starve_ticks_`. Record over ~100 ticks which
   prio-20 task tops the queue at the ~6-tick cadence, and the counter sawtooth. Post
   the tally as a comment on #598. If a TEMP-DEBUG print is used instead of GDB, it
   must be grep-verified reverted before the diff is generated.
3. **`priority_map.hpp`.** Replace `highest_starvable_below` with
   `lowest_starvable_below(uint64_t prio)`: same `prio <= 1` early-out, same
   `lo_mask`/`hi_mask` expressions, but test `bitmap_lo_ & lo_mask` first with
   `find_lowest_bit` (word tested non-zero before the call), then
   `bitmap_hi_ & hi_mask` with `64 + find_lowest_bit`. Update the doc block:
   window `[1, prio)`, level 0 excluded (cite the vault contract), return = the
   lowest ready band (the maximally-waiting victim), `STARVABLE_NONE` when none.
4. **`scheduler_dispatch.cpp::next_task_below()`.** Swap the level query to
   `lowest_starvable_below`; keep the `while (rq_own().has_ready())` shape, the
   `dequeue_level` call, the eligibility predicate, the `current_task()`
   exclusion, the `nullptr` sentinel and the finite-progress argument in the
   comment. Restate the contract: highest → lowest victim, same eligibility as
   `next_task()`.
5. **`scheduler_tick.cpp` breaker block.** Replace `:1238-1265` with:
   `uint64_t cpu = sched_cpu() % CONFIG_MAX_CPUS;`
   `uint64_t top = rq_own().highest_ready_priority();`
   `uint64_t victim = rq_own().bitmap().lowest_starvable_below(top);`
   if `victim == PriorityMap::STARVABLE_NONE` → set `starve_level_ =
   STARVABLE_NONE` and `starve_ticks_ = 0`; else set `starve_level_ = victim`,
   `++starve_ticks_`, and at `>= kStarvationBreakerTicks` keep the EDF stand-down
   (`if (edf_preempt_candidate() == nullptr) breaker_next = next_task_below(top);`)
   then reset both fields unconditionally. Delete the `top == starve_level_` branch
   and the old reset form. Rewrite the leading comment: victim-keyed so a transient
   upper-level waker can neither reset the count nor divert the forced pick; ceiling
   unchanged; counter provably ≤ N because every fire resets.
6. **`scheduler.hpp` doc blocks.** `:1626-1632`: "consecutive *denied* ticks
   while lower non-idle READY levels wait" → "consecutive ticks the victim band was
   denied". `:1633-1640`: the pair is "victim band under watch (diagnostic
   mirror) + consecutive denied ticks"; keep the reset-owner and counter-only
   statements. `:1641-1650`: lowest-band rule.
7. **`scheduler_registry.cpp:76-79`.** Comment only: the pair is victim band +
   consecutive denied ticks; `.bss` zero + reset owner unchanged.
8. **`docs/specs/scheduler.md`.** §3.3 first bullet → "When the strict pick leaves a
   lower non-idle READY band waiting for `kStarvationBreakerTicks` (= 20) consecutive
   ticks, exactly one dispatch of the *lowest* eligible READY non-idle band is
   forced, then strict RMS resumes; the observation is keyed to that victim band, so
   a transient higher-priority waker can neither reset it nor divert the forced
   dispatch." Delete "Levels below the highest starved level keep classic
   fixed-priority semantics ..." and replace with "Bands above the victim keep strict
   fixed-priority service; the breaker invests its single forced slot in the lowest
   ready band, so the bottom-most waiter is never denied forever." Interference
   bullet → "one forced tick per N (= 20) denied ticks (20 ms ceiling at 1 kHz)"
   (keeps the 2026-09-29 S3 closed). Keep the counter-only / EDF-precedence /
   policy-runtime-reset bullets verbatim; tighten R4 (`:64-67`) to the same
   statement.
9. **Builds.** `tools/meson/build.sh --arch x86_64|aarch64|riscv64` (LTO) and
   `tools/meson/build.sh --arch x86_64 --nolto`: link green everywhere, no new
   warnings from the touched files.
10. **Gates, one class at a time, each appending a `test-history.txt` row:**
    `scheduler_core` x86 LTO → x86 NOLTO → `scheduler_starvation` →
    `scheduler_budget` → `scheduler_core` aarch64 → `scheduler_core` riscv64.
    Class timeout = last measured value × 1.5 (`scheduler_core` history rows range
    460-2180 ms; 60 s is the safe class budget).
11. **Stash control (mandatory).** `git stash` the source+doc diff, rebuild the LTO
    tree, run `scheduler_core` → the stall at test 21 must reproduce;
    `git stash pop`, rebuild, rerun → must pass. Both rows recorded; the pair is
    the fix's proof, and the unmodified-`main` run doubles as the flake control.
12. **Close-out.** `grep -n CONFIG_DEBUG_IPC_SCHED src/kernel/debug/ipc_sched_trace.hpp`
    still commented out; `make check-style` → Errors 0; `graphify update .`;
    mirror the spec change into the Obsidian vault (preserve frontmatter/wikilinks);
    generate `audits/pending_patch.diff` and post report path + DECISION on #598.

## TEST STRATEGY

Dev lands **no tests**. QE sketch, stub-first (`JARVIS_TEST_PASS()` first, then real
assertions; `// Runmode/Testidea/Input/Expect/Depends` doc block with a pseudocode
block; `JARVIS_REGISTER_TEST` debug-only; teardown via `terminate_and_drain`
before the final assert):

- **T1 `scheduler_breaker_periodic_upper_waker`** — harness (10) + two
  forever-spinners at 11 + one periodic waker at 12 that loops `NANOSLEEP(4 ms)`
  (period < N *by construction*, so the top identity changes every ~4 ticks
  regardless of codegen). Asserts the harness completes 10 × `NANOSLEEP(5 ms)` with
  `elapsed <= 1200` ticks. Pre-fix: TIMEOUT (stash control). This is the
  F1/daemon-shape fixture, arch-neutral.
- **T2 `scheduler_breaker_stacked_bands`** — harness + forever-spinners at 11 **and**
  12, no periodic task. Pre-fix the top is stable at 12, the count reaches 20, and
  the forced pick is 11 → the harness never runs (TIMEOUT); post-fix the forced pick
  is 10 → progress. Proves the target-diversion half (F2) with zero timing
  dependence, on every arch; passes under Option A or B.
- **T3 `scheduler_breaker_idle_band_excluded`** — a prio-1 resident and the idle
  band must never receive the forced slot: assert the prio-1 execution counter gains
  no forced tick and the prio-2 probe keeps running (guards INV-STARVE-5 after the
  target change).
- **Existing classes to run:** `scheduler_core` (x86 LTO + NOLTO, aarch64, riscv64),
  `scheduler_starvation` (3 tests — `SchedulerStarvation`'s doc-block documents
  pre-#242 behaviour and must NOT be edited), `scheduler_budget` (6 tests,
  sporadic-server interaction with the prio-20 band), plus `timing` if budget allows.
- **Test sanctity:** `scheduler_breaker_lower_progress`
  (`test_scheduler.cpp:769-820`) stays byte-identical — it remains valid and must
  still pass (the 2-level shape is identical under the new rule). If a
  strict-priority test fails because it asserts "the lower band never runs across
  > 20 backlogged ticks", that is the documented semantics change surfacing: escalate
  to owner/QE, never weaken the assertion.
- **Rows:** one `test-history.txt` row per run, including the stash control and any
  TIMEOUT/STALL marker, measured times only.

## SIL 3 / SAFETY RISKS

- **[S1] The forced dispatch can now target a lower band than before** — with
  several continuously-backlogged bands the relief slot moves from the
  second-highest to the lowest. A slipped eligibility filter or a trusted stale
  bitmap bit would double-dispatch or dispatch a dead task. *Mitigation:*
  `dequeue_level` reuse (INV-RQ-1 unchanged), verbatim eligibility mirror
  (READY/RUNNING, affine, never current), `% CONFIG_MAX_CPUS` clamp, and the
  existing oracles (`runq_link_flag_desync_count_`, `orphan_rescue_count_`)
  asserted 0 in T1-T3.
- **[S1] Victim-keyed accumulation can fire when nothing is genuinely starving**
  (the lowest ready band may be a task that would have run anyway), spending the
  slot on a no-op relief. *Mitigation:* INV-STARVE-1 keeps the ceiling at ≤ 1 per 20,
  no priority/order mutation, spec re-characterization in the same commit, and a
  comment stating the no-op is harmless.
- **[S2] Documented semantics change** — the §3.3 caveat "levels below the highest
  starved level keep classic fixed-priority semantics" is deleted; a middle
  backlogged band loses the forced slot. *Mitigation:* R4/§3.3 rewritten in the same
  diff (the auditor checks doc/code agreement); Option B recorded as the escalation
  path if the owner rejects the caveat loss.
- **[S2] Stale counter across `harness_nonpreempt` early returns (F5)** can produce
  an early fire (up to 19 ticks early). Fail direction is "relief sooner", never
  later; documented, not changed here.
- **[S2] A strict-priority regression test may legitimately break** (it encoded the
  removed caveat). *Mitigation:* run `scheduler_core`, `scheduler_starvation`,
  `scheduler_budget` before the audit; escalate any such failure instead of
  patching it by weakening the test.
- **[S3] Dead code:** `highest_starvable_below` must not survive unused.
  *Mitigation:* single rename, `grep -n "starvable_below"` before/after.
- **[S3] `find_lowest_bit` returns 0 for a zero mask** — the word must be tested
  non-zero before the call, or band 0 becomes a lookalike victim.
- **[S3] Prior-audit wording duty** (`audits/report-2026-09-29T153000Z.md`: "one per
  N", not "(N+1)/per-21") must stay closed after the §3.3 rewrite; the
  `sched_cpu()` clamp S3 is discharged in the same lines.

## OPEN QUESTIONS

- **Blocks steps 3-5 (policy choice): confirm Option A (lowest-band target) before
  implementation.** It changes documented behaviour (the §3.3 caveat loss), which the
  owner must accept; Option B (round-robin over starved bands) is the alternative if
  per-band fairness is required. Recommendation: A — it restores R4 with one
  mirrored helper and zero new state, and QE test T2 passes under either. Nothing
  else in this plan is blocked: the waker identity from step 2 is evidence, not a
  dependency.
