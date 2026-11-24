# DESIGN REVIEW R2 — issue #608 (S2: lock-park FailClosed false positive on legal wheel sleeps)

Scope: PLAN ONLY (`audits/plan-608.md` r2, as amended post-r1), no code exists.
Read-only on `main @ a1e84e694fa4b4c4260d55c43cd9ce15fc21eb9e` verified via
`git rev-parse HEAD` (branch `main`; `dev/604-c6-gates` present but untouched;
uncommitted `M audits/pending_patch.diff` scratch + `?? audits/plan-608.md` /
`?? audits/design-review-608-r1.md` only — touched nothing, committed nothing).
Normative issue: `gh issue view 608` (OPEN, S2, witness `tick=48 streak=8
is_locked=0`, 40 PANIC rows / 17 classes). PROMPT-audit applied as design
review: no `pending_patch.diff` exists, so no PATCH section and no
`rejected_patch.diff` — r1 findings were plan amendments, now verified folded
in. Severity scale per PROMPT-audit; decision rule unchanged (any S1/S2 ⇒
REJECTED, S3-only ⇒ APPROVED).

## R1-FINDING CLOSURE (the 7 load-bearing re-checks)

1. **S2 E-atomicity — FIXED.** Plan INVARIANTS ¶2 (line 43) now carries the full
   transition-order argument: mark-AFTER-take / unmark-BEFORE-release
   (verified: `scheduler_lock_guard.hpp:74-75` take-then-`++depth`,
   `:93-94` `--depth`-then-`unlock`), so E is true throughout both transitions;
   only a *post-clear remote release* inside the load-accrue window miscounts
   (over-count, one instant); a remote take between read and reset drops one
   held instant (under-count, one instant of tripwire delay); local same-CPU
   IF=0 transitions cannot interleave; panic needs 8 *consecutive
   same-direction* races in nanosecond windows. Directions correct
   (release→over-count, take→under-count — verified against guard order).
   Applies-identically-at-pre/miss/post stated explicitly. Covers the core
   (`s582_cover_instant :1370`, verified 3 callers pre `:1659` / post `:1695` /
   miss `:1738`) and all three service sites, not just the core.
2. **S3 free_warned — FIXED.** Step 4 (line 62): separate `free_warned` bool
   (rule-6 one-shot untouched), **preserved on same-`{task,generation}` re-arm**,
   **starting false on fresh arms**. Both halves stated.
3. **S3 fresh-arm last_cover_tick — FIXED.** Step 3 (line 61): fresh lifetime
   zeroes `last_cover_tick` (= struct default, step 1 line 59) **with the tick-0
   rationale** (monotonic ticks never re-issue tick 0, so a `==0` first-cover
   skip is a harmless one-time event, never a systematic blind spot). Clear
   (step 2, line 60) also zeroes — sentinel consistent 0 across
   struct-init / clear / fresh-arm (see §4 below).
4. **S3 fence-3 — FIXED.** Fence 3 (line 48) recorded as explicit **AMENDED
   (not unchanged)** with the livelock argument (true wedge holds word
   continuously ⇒ no !E ⇒ fires; free gaps mean dispatch was possible ⇒ reset
   honest); no-weakening contract (line 49) carries
   chained-holders-with-free-gaps (`scheduler.md:250-251`: fires iff gaps
   absent, resets iff a gap proved dispatch possible).
5. **S3 #604 proviso — FIXED.** Step 11 (line 69): "If #604 lands first, rebase
   this hunk onto that tip (its holder-bounds rewrite sits textually adjacent)
   and re-run recon 0.2 before committing." Hunk-scoped, on the step-11 doc
   hunk.
6. **S3 QE test — FIXED.** Must-still-fire (line 76): tick-advance via **spin on
   `arch::Timer::ticks()`** (ISR counter advances regardless of held lock) +
   **combined miss/pre accounting** (interim `:165` try_lock misses contribute
   miss-path covers alongside pre-path covers) with **`covered == 8` exact**.
7. **S3 retrieval placement — FIXED.** `gh issue view 608 --comments` shows the
   check-7 block thread-visible (4 queries + dispositions, verbatim from plan).
   Substance was verified real in r1 check 8; placement now passes a
   thread-only check-7 read.

## NEW-CONTRADICTION SWEEP (amendments vs plan body)

- Amendment-vs-preservation language consistent: fence 3 alone marked AMENDED;
  fences 1/2/4/5/6/7 remain "unchanged" — correct, R4' adds the only second
  reset path and fences 1/2/4-7 are untouched in mechanism.
- Step 5 "no behaviour change" (stash of `last_cover_tick`) vs step 6 witness
  fix: consistent — the stash itself is inert (pre-cover branch already had E
  true); the behavior change lives in the R4' core that post now calls.
- Step 7 "unchanged call" for miss: consistent — call line unchanged, gating
  moves into the core re-read of E.
- Sentinel consistent: struct default 0 / clear→0 / fresh→0 / stamp-both-
  branches (step 4, line 62 `last_cover_tick = now` on both E/!E arms).
- No new panic/never-panic path introduced; bound stays 8; class stays
  diagnostic-only (H5 forbidden, line 32); TEST STRATEGY still dev-lands-none
  (step 12).

## CARRIED IMPLEMENTATION NOTES (binding on the dev lane)

- E must be read inside `s582_cover_instant`, after RMS's adopted guard has
  destructed (`scheduler_tick.cpp:1139`), never from a pre-RMS snapshot; if
  recon 0.3 shows E true at a legal post instant, fall back to passing the
  pre-time `deliverable` bit into `LockParkWatch` (SIL 3 risk S1, line 90).
- `last_cover_tick` RELAXED atomics like `covered_ticks` (per-CPU single-writer:
  tick ISR + same-CPU task-context `on_tick` reentrancy).
- `SCHED+24` collision check at implementation (no static_assert covers the
  4000-window) + severity mirror in `lookup_severity`.
- TEMP-DIAG verbatim revert + `grep -n "TEMP-DIAG-608" src/` empty before any
  gate run.
- QE `covered_ticks_max` / `SCHED+16..19` expected-count rows shift with the
  unit migration — update in the same QE commit.
- Doc drift: hpp doxygen + `scheduler.md:232-240` must state "distinct ticks
  with the lock held/owned".
- #564 interim discipline (line 71): tripwire PANIC during flip gates ⇒
  identical-binary retry, never flip fallout; do not base on `dev/604-c6-gates`.

## FINDINGS

(none — all r1 findings closed, no new contradictions)

DECISION: APPROVED
