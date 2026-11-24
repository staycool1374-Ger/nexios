# DESIGN REVIEW R1 — issue #608 (S2: lock-park FailClosed false positive on legal wheel sleeps)

Scope: PLAN ONLY (`audits/plan-608.md`), no code exists. Read-only on `main @ a1e84e694`
verified via `git log --oneline -1` (branch `main`; uncommitted `audits/pending_patch.diff`
scratch + `?? audits/plan-608.md` only — touched nothing, committed nothing).
Normative issue: `gh issue view 608` (OPEN, S2, witness `tick=48 streak=8 is_locked=0`,
40 PANIC rows / 17 classes). PROMPT-audit applied as design review: no `pending_patch.diff`
exists, so there is no PATCH section and no `rejected_patch.diff` — findings are plan
amendments the developer folds into plan-608 r2 before implementation. Severity scale
per PROMPT-audit (S1 blocker / S2 major-must-address / S3 note); decision rule unchanged.

## VERIFICATION (the 8 load-bearing checks)

1. **H1 exhaustion — CONFIRMED.** `s582_cover_instant` (`global_state.cpp:1370`) has exactly
   three callers in-tree: pre `:1659`, post `:1695`, miss `:1738`. Pre `:1659` is reachable
   only when `deliverable == false` (`:1642-1644` = `word_held || own_depth != 0`), i.e. lock
   evidence present by construction. Miss `:1738` runs only after the `:165` try_lock failed
   (`scheduler_tick.cpp:886-894`), i.e. a holder existed at try time. Post `:1695` needs no
   predicate: pre `:1642-1657` already proved the word free, RMS's adopted guard destructs at
   `scheduler_tick.cpp:1139` scope exit before post runs at `:1115`, so post normally observes
   a free word and counts +1/tick. The witness (`is_locked=0 holder=0x0 slock_depth=0
   streak=8`) is the predicted signature. Uniqueness claim holds.
2. **Secondary defects — both CONFIRMED.** (a) Unit inflation: a `:165`-fail tick counts miss
   (`:893`) + post (`:1695`) = 2/tick, so the "8-tick" bound is 4–8 real ticks. (b) False FATAL:
   `s582_failclosed` (`:889-989`) prints "scheduler_lock_ unreleased" on a path where the
   dump itself shows nothing held. Both fixed by the same predicate change.
3. **R4' dedupe (`now != last_cover_tick`) — SOUND.** `Timer::ticks_` advances by ISR
   `fetch_add` (`arch/x86_64/hal/timer.cpp:192`) — monotonic non-decreasing values; TCG
   dilation changes rate, never monotonicity. Same-tick-double-service (pre+post inside one
   tick, miss+pre/post inside one `on_tick`) is exactly what the latch suppresses; tickless
   stretches issue no service calls (service points live in `on_tick`/RMS paths). Tick-0 edge
   (first cover suppressed if `last_cover_tick` inits 0 and `now == 0`) is unreachable in
   practice (latches arm on live tasks, ticks > 0) — specified as S3 below, not a defect.
4. **F1 E-predicate vs F2/F3 — direction right, atomicity UNARGUED (see S2-1).**
   F2 rejection sound (any finite bound still panics on arbitrarily long legal blocks; needs
   a new panic path + audit). F3 rejection sound (pure postponement). F1 does not reintroduce
   either: warn is one-shot per latch lifetime (S3-2 precision gap aside), bound stays 8.
5. **No-weakening — HOLDS for all three must-still-fire shapes.** `scheduler_lock_` is a
   SINGLE global `SpinLock` (`scheduler.hpp:1587`), so `word_held` is visible from every CPU:
   cross-CPU chained holders still yield E=true on the servicing CPU. SelfLock (own take ⇒
   word held + depth ≠ 0) and own-depth reentrancy (task-context `on_tick`, r6-accepted S3)
   both satisfy E. Witness shape and miss+post double-count stop. All 7 r6 fence properties
   preserved in mechanism — with one honesty amendment required (S3-3: r6 closed the clock as
   "reset only on clear"; R4' adds a reset, correctly argued but mislabeled "unchanged").
6. **Validation experiment — DECISIVE and bounded.** Matrix pivots on the exact H1 signature
   (8th increment `site=post, is_locked=0, own_depth=0`, latch BLOCKED, `!= cur`) with a
   genuine-held-window fallback (holder + take-site ring) on disproof — decisive both ways.
   TEMP-DIAG bounded (one line/increment, `CONFIG_DEBUG`-gated, verbatim revert + empty
   `grep TEMP-DIAG-608` gate before runs). 20× `posix_time` (firsthand witness class) is
   adequate for recon: under H1 any latched legal block > 8 ticks panics deterministically,
   so hits are not rare-per-class draws. GDB + synthetic-injection fallbacks present.
7. **Test sanctity — SPEC COMPLETE and exercises the new predicate.** Dev lands none (step 12:
   production + docs only) ✓. QE owns: must-still-fire takes the lock via `SchedulerLockGuard`
   (own depth ≠ 0 + word held ⇒ E true under R4', not just old behavior) and drives 8 distinct
   ticks to the 8th-increment panic with `covered == 8` exact (panic fires on the first
   `>= 8`, so exactly 8) ✓; must-not-fire drives 16 free-lock pre→post pairs asserting
   `covered == 0`, `free_nodeliver == 16`, latch retained, wake-clear on READY ✓; unit-is-ticks
   covers dedupe ✓. Counter/`SCHED+16..19` expected-count updates assigned to the same QE
   commit ✓. One precision gap (S3-4: miss-path contributions while the test holds the lock
   across tick-advance).
8. **Retrieval real, scope clean.** Every cited anchor verified in-tree: `global_state.cpp`
   `:402-408/:436-443/:889-989/:1152-1269/:1354-1404/:1480-1499/:1569-1739`,
   `scheduler_tick.cpp` `:886-894/:1108-1115/:1655-1668`, `park_lock.cpp:191-199`,
   `global_state.hpp:280-383`, `dmesg_catalog.hpp` SCHED `+16..+23` taken (so `+24` is the
   next free code — verify at implementation; no static_assert covers the 4000-window),
   `scheduler.md:214-251`, `test_testrunner.cpp:640-659` precedent, `test-policy.tsv`
   `expected_panic` key. `design-review-194-code-r2.md` witness line byte-matches the issue;
   `design-review-582-r6.md` APPROVED with the 7 properties the plan claims. Bound stays 8;
   class stays diagnostic-only; no fence weakened in mechanism. #604 overlap checked:
   `main...dev/604-c6-gates` touches NO `s582/lockpark/cover` line in `global_state.cpp`
   (trace-kit deletions only), no pre/post/miss line in `scheduler_tick.cpp` — functional
   disjointness verified; same-file adjacency only (`scheduler.md` holder-bounds paragraph +
   §13 append; `scheduler.hpp` guard table). Plan's "base on main tip, not `dev/604-c6-gates`"
   is therefore sound; sequencing note names #564 only — #604-land-first doc-hunk proviso
   missing (S3-5). #564 interim discipline stated (plan §13) ✓.

## FINDINGS

- [S2] E read+accrue atomicity unargued at all three cover sites — `global_state.cpp:1370-1373`
  (core), `:1642-1644` + `:1659` (pre), `:1695` (post), `:1738` (miss).
  WHY: the plan states E is "read at the instant of the increment" as if atomic, but E is two
  separate lock-free loads (`scheduler_lock_held()` word load + `scheduler_lock_depth_own()`
  per-CPU load) followed by a non-atomic accrue, evaluated under no lock. A remote-CPU
  release landing between the word-load and the accrue counts a free instant as covered
  (over-count, false-panic direction); a remote take landing between the read and a reset
  drops a held instant (under-count, tripwire-delay direction). The fix is almost certainly
  still correct — guard discipline is mark-AFTER-take / unmark-BEFORE-release
  (`scheduler_lock_guard.hpp:71-99`), so E is true throughout both transitions and only a
  post-clear remote release races, each mis-fire costs exactly one instant, and panic needs
  8 consecutive same-direction races in nanosecond windows — but NONE of this is in the plan,
  and E is the SOLE gate of a kernel FATAL. Remedy (plan-text only): add the transition-order
  argument + quantify both mis-directions (single-instant effect, 8-consecutive requirement,
  local stability under IF=0 vs remote-only race) for pre/miss/post alike.
- [S3] `free_warned` preservation across same-task re-arm unspecified (plan steps 2 vs 4).
  WHY: step 2 preserves `covered/warned/last_cover_tick` on same-`{task,generation}` re-arm;
  step 4 adds a separate `free_warned` bool without stating arm behavior — if not preserved,
  chronic free-lock conditions emit SCHED+24 every re-arm (every tick), i.e. the warn-spam
  shape check 4 asks about. State preserve-on-same-task explicitly.
- [S3] Fresh-arm initial value of `last_cover_tick` unspecified (plan steps 1-2).
  WHY: `latch_clear` zeroes it, but a fresh (new-task) arm's initial value is never stated;
  specify it (0 with the ticks>0 argument, or a `UINT64_MAX` sentinel so the first cover
  always counts) so the dedupe has no implementation-defined first-tick behavior.
- [S3] r6 frozen-property delta mislabeled "unchanged" (plan INVARIANTS fence 3 vs
  `audits/design-review-582-r6.md`: "reset only on clear", "r5 rearm-livelock closed by
  construction").
  WHY: R4' adds a second reset path (`covered := 0` on observed free lock) to a clock whose
  no-reset-except-clear property is frozen. The livelock argument itself is sound (a true
  wedge holds the word continuously ⇒ no `!E` ⇒ fires; free gaps mean dispatch was possible
  ⇒ reset is honest) — but it must be recorded as an explicit AMENDMENT to the r6 property,
  not as "unchanged", and the chained-holders-with-free-gaps case (`scheduler.md:250-251`:
  fires iff gaps absent, resets iff dispatch was possible) must be stated in the
  no-weakening contract.
- [S3] #604-land-first doc-hunk proviso missing (plan §13 names #564 only).
  WHY: verified functionally disjoint (no shared `s582/lockpark` lines), but #604 rewrites
  the `scheduler.md` holder-bounds paragraph textually adjacent to the plan's step-11
  rewrite target (`:232-240`) and appends §13 — if #604 lands first, step 11 needs a rebase
  onto that tip plus a re-run of recon 0.2. One sentence covers it.
- [S3] Must-still-fire QE test holds `scheduler_lock_` across tick-advance without a
  tick-advance mechanism or miss-path accounting (plan TEST STRATEGY).
  WHY: spinning for 8 distinct ticks while holding the lock makes every interim `:165`
  try_lock fail, so miss-path covers (E true, distinct ticks) contribute to the 8 alongside
  the pre-path covers — the panic still proves R4' escalation, and `covered == 8` stays
  exact, but the test as written is a combined-path test, not a pre-path unit. Specify the
  advance mechanism (spin on `Timer::ticks()` — ISR counter advances regardless of the
  lock) and assert combined accounting.
- [S3] Retrieval artifacts live in-plan but not on the #608 thread (PROMPT-audit check 7
  reads thread-visible).
  WHY: substance verified real (check 8 above), so this is placement only — paste the
  retrieval block + dispositions to #608 at filing alongside the step-12 evidence comment,
  so a check-7 read of the thread alone passes.

DECISION: REJECTED
