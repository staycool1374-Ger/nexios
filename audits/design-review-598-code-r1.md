# CODE REVIEW 2026-11-21 — issue #598 (code r1, design-review-598-r1 follow-up)

PATCH: audits/pending_patch.diff (byte-identical to fresh
`git diff -- <6 files>`; hunk counts self-consistent, e.g. tick hunk
32/41 old/new. Artifact is COMPLETE — an apparent mid-hunk ending at
"EDF keeps precedence…-strict" is the legitimate close: 3 trailing
context lines `}`/`if (starve_ticks_…`/`// EDF…`, new range ends 1263.)
Base verified: main @ 6dac02fd0. No test files in scope (verified via
`git status --porcelain`); test-history.txt excluded per pipeline scope.
Graphify-out churn is tooling noise, out of scope.

## VERIFICATION (each brief item re-checked in-tree/worktree)

1. Helper mirror — EXACT. `lowest_starvable_below` keeps the identical
   `prio <= 1` early-out, identical `lo_mask`/`hi_mask` expressions
   (`0xFF…FE` / `((1ULL<<prio)-1) & ~1`; hi `(1ULL<<(prio-64))-1`),
   level-0 exclusion preserved (`& ~1ULL`), `STARVABLE_NONE` preserved,
   zero other callers left (`highest_starvable_below` grep: NONE;
   `lowest_…` refs: exactly 2 callers). Scan order lo-word-first with
   `find_lowest_bit`, each word zero-guarded before the call
   (`find_lowest_bit(0)==0` alias hazard closed, doc states it).
   Any lo-word bit (levels 1..63) is below any hi-word bit — lowest-first
   order is correct for "lowest in [1,prio)".
2. Dispatch query — EXACT MIRROR. Only the level query + comment changed;
   `while (has_ready())` shape, `dequeue_level`, `nullptr` sentinel +
   unreachable-contract comment, `current_task()` exclusion and
   READY/RUNNING + `queue_target()==sched_cpu()` predicate are untouched
   context, byte-identical to `next_task()`'s predicate
   (dispatch.cpp:78-81 vs 114-117).
3. Counter block — CORRECT. Reset iff `victim==STARVABLE_NONE`
   (subsumes old `top==0` via helper early-out; field now NONE+0, cleaner,
   no behavior delta since field is never compared). Else
   `starve_level_=victim; ++starve_ticks_`. `top==starve_level_` branch
   fully deleted (grep: NONE). `% CONFIG_MAX_CPUS` clamp present
   (tick.cpp:1252). EDF stand-down + unconditional fire-reset preserved
   (1262-1273: null-gate, then both fields reset outside the inner `if`).
   Victim-carryover fail-safe comment present (1238-1243, explicit
   do-NOT-reset-on-change warning).
4. No new state/locks/config/snapshot/arch; zero priority writes —
   CONFIRMED. Diff `+` lines matching
   `mutex|IrqGuard|snapshot|new |malloc` : none (only the clamp line
   mentions CONFIG_MAX_CPUS, pre-existing macro). `priority =` writes in
   diff: none (sole hit is the "no `task->priority` write" comment).
   `reset_starvation_breaker` logic untouched; no snapshot/config/arch
   file in scope; zero `#if` lines added/removed; debug/release flow
   identical (the one `#if CONFIG_DEBUG_IPC_SCHED` nearby is
   pre-existing context).
5. Docs — AGREE WITH CODE. §3.3 retitled "(issues #242, #598)", victim
   rule + transient-waker sentence + L0 exclusion + `lowest_…` op name;
   caveat deleted, replaced by "Bands above the victim receive no breaker
   relief and remain denied while the top never drains (…strict service
   only whenever the top drains; …tracked in issue #599)". R4 retitled,
   bound "one forced lower tick per 20 denied ticks". Interference "one
   forced tick per N (=20) denied ticks (20 ms ceiling)". #242 duties
   closed (no "(N+1)/per-21" wording anywhere).
6. r1 S3s — ALL APPLIED IN CODE/DOCS: tip drift noted (base 6dac02fd0);
   §3.3 honest sentence + #599 ref (#599 OPEN, Option-B escalation,
   filed 11:58); victim-carryover comment; clamp; no-call GDB rule was a
   plan-execution constraint (see S3-4 below).
7. Style/gates — Style independently re-verified: `validate_style.py
   --severity error --quiet` → Errors: 0 (Warnings: 0, 578 files).
   8 test-history rows present and sequenced: 13:01:17/13:01:33
   core 39/39 (LTO/NOLTO), 13:01:48 starvation 3/3, 13:02:03 budget 6/6,
   13:02:11/13:02:22 core 39/39 (aa64/rv64), 13:06:56 TIMEOUT,
   13:07:44 core 39/39 post-pop. Zero post-fix STALLs; trace macro
   confirmed commented out (ipc_sched_trace.hpp:34).
8. Stash control — COHERENT. RED TIMEOUT row sits between green runs and
   is followed by a full 39/39 green after pop; 0/0-with-TIMEOUT matches
   the filed defect signature (hang at test 21 → harness reports no
   summary). Full-removal provability is inherently retrospective (see
   S3-3 limitation note); code-shape argument is airtight regardless:
   old code provably cannot accumulate 20 identical tops under a
   <20-tick periodic waker, new code accumulates independent of top
   identity — the RED→green pair is over-determined, not load-bearing
   alone.

Critical checks 1-11: no heap alloc (counter + O(1) bitmap ops);
no lock change (tick-owned writes under scheduler_lock_ as before);
zero test edits; no PMM/BufferPool touch; counter-only, no
critical-section interference; 0 `#if` deltas; check-7 artifacts on the
#598 thread (work-begun comment with graphify query + path +
vault-search dispositions, expanded in plan-598.md RETRIEVAL);
arch-neutral shared C++ (`find_lowest_bit` common hal, no arch gates);
no new failure paths, sentinel preserved; scheduler-internal only, no
capability crossing; evidence rows present.

## FINDINGS

- [S3] test-history.txt rows carry no arch/build columns — the
  LTO/NOLTO/aa64/rv64/stash mapping rests on timestamps + developer
  assertion; TIMEOUT row records TIME: 0ms (unmeasured, violates the
  measured-time format).
  WHY: Traceability gap only; sequencing + counts are internally
  consistent and the code proof does not depend on row labels.
- [S3] Off-arch gates cover scheduler_core only; starvation/budget were
  not run on aa64/rv64 and no justification was posted on #598 (r1 S3-3
  asked for trio-or-justification).
  WHY: Mechanism is arch-neutral shared C++ and both new-behavior
  fixtures are structural, but the plan's own parity rule cuts both
  ways — record the justification on #598 before landing.
- [S3] Plan step-2 GDB waker tally was not posted on #598 (single
  work-begun comment only); waker identity rests on the code-shape
  argument + prio-20 daemon-band in-tree evidence.
  WHY: Stash RED→green pair + F1/F2 code proof carry causality; GDB
  tally would be corroboration, not the load-bearing evidence.
- [S3] Stash-true-removal is asserted, not independently re-provable
  post-pop (no pre-stash `git stash list`/hash captured on the issue).
  WHY: Process hygiene for future controls; does not weaken this
  verdict given the deterministic code-shape proof + coherent row
  sequence. Capture `git stash show` output in the issue comment next
  time.

DECISION: APPROVED
