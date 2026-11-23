# PLAN: #610 — READY task invisible to dispatcher under TCG dilation (test-8 waitpid racer stranded)

## HYPOTHESES (two co-equal — r1 demotion; no single leading candidate)

**H1 (flag-off-chain, needs a timing-only producer): the EDF chain is a
flag-gated second discipline that #582's "links are truth" rule never reached, so a READY,
EDF-eligible task can hold `in_edf_queue_==1` while being unreachable from `edf_ready_[0].head_`;
`edf_peek_valid_own` cannot splice such a node (flag-gated `edf_remove`) and the #600 P2 sweep
explicitly `continue`s on `in_edf_queue_` truth — so nothing ever rescues it, and the task starves
forever while `in_ready_queue_==0` (which for an EDF task is NORMAL, not the anomaly the FAIL line
suggests).** PRIOR ART — open issue **#601** ("[#600 S3 r1] EDF-eligible + in_edf_queue_ +
unreachable-from-head stays invisible to sweep and peek", filed by design-review-600-r1 as
**corruption-premise-only**: all EDF writers are typed IRQ-atomic units). This plan adopts the
#601 shape for a pure-timing TCG flake, so H1 stays convicted ONLY IF validation names a
timing-only producer that orphans a chain under dilation; without one, H1 is not distinguishable
from #601's residue and the fix target becomes H2. Trigger legs that can produce the orphaned
chain: the `edf_peek_valid_own` magic-bad-head fail-closed `l.head_=nullptr`
(`scheduler_queue.cpp:201-207`), the INV-RQ-5 bound-hit nuke leaving nodes past the bound flagged
(`:222-241`). (`edf_detach` itself is NOT a trigger: it clears flag+links as one IRQ-guarded
unit (`scheduler_queue.cpp:140-149`), and the three wholesale head-null sites
(`rebuild_ready_queue`, snapshot restore, lifecycle teardown) each pair the null with per-node
clearing — verified in-tree. An earlier draft misdescribed this; corrected per r1.)

**H2 (co-equal, has the direct dilation story): sweep-gate miss under TCG-stretched contention** —
`preempt_enabled_==false`, tick `try_lock` miss, stale `current_task()`, or
`queue_target(task)!=sched_cpu()` — any of which skips the rescue without touching membership.
Dilation stretches the contention window, so H2 predicts exactly the observed signature
(station-only, Mac-green, 2/5) with no corruption premise at all.

Why these two and not the others: `in_ready_queue_==0` is the ONLY diagnostically meaningful number
available in the FAIL line, and for all three test tasks (periods 10/9/10 → all EDF-eligible) it is
the expected value, so the QE reading "none queued anywhere" is not yet evidence. Remaining
candidates stay live until the decision table in Phase A step 6 rules them out: **C3**
dispatch-side drop loop or deferred-arm discard without `restore_preempted_current` (#590/#593
fence — out of this lane's scope), **C4** `enqueue_ready` H2-ownership silent refusal, **C5** pure
test-seam artifact (test 8 uses `seam_noop`, so only its snapshots are racy).

BASE: HEAD `92378b81781e744805f2a1fe66ee1e5c23b36501` ("merge origin/testbed", verified via `.git/logs/HEAD`;
reflog is the only git surface available to this planner lane). Branch `main`. The tree carries STAGED
uncommitted #604 work — this plan changes NOTHING in it and lands NO commit.

## RETRIEVAL (mandatory audit check-7 — queries + dispositions)
- graphify `query_graph` "set_task_ready ready queue membership flag" (dfs, depth2) → **used**: located the
  membership surface (`set_task_ready` sched.hpp, `enqueue_ready`/`dequeue_ready`/`move_priority`,
  `ReadyQueueManager`, `TaskQueue`) and the "6. Task Lifecycle — Ready-Queue Membership" spec section.
- graphify `query_graph` "RMS dispatch pick next task" (bfs) → **used**: only the dispatch file
  (`scheduler_dispatch.cpp`) and `docs/specs/rms-rework.md` entry points; **ignored** the debugd/
  state-machine noise it also returned (unrelated community).
- graphify `query_graph` "EDF insert remove ready queue single discipline" (bfs) → **used**:
  `edf_remove()` (scheduler_queue.cpp) + `TaskQueue`/`ReadyQueueManager` = the two-discipline split;
  **ignored** `sync/queue.hpp` fan-out (IPC, not dispatch).
- graphify `shortest_path` add_task → dequeue_ready (2 hops) and edf_insert → edf_peek_valid_own (2 hops)
  → **used**: both live in the same TU pair, confirming the bounded change surface.
- Obsidian `obsidian_search_vault` "scheduler" / "in_ready_queue" / "582" → **used**:
  `audits/audit-task-sync-v0.4.2.md` (in_ready_queue_ is intended authoritative; double-enqueue refusal),
  `audits/deep-analysis-h2-ssdeadline-v0.3.9.md` (the INV-4 precedent: a preempted harness reading
  `state=READY, in_ready_queue_=0` and never redispatched — the closest prior art to this signature),
  `audits/design-review-593-r1.md` (`dequeue_ready(task)` is the corpse funnel; arm/INV-593-4 fences).
- Obsidian searches "ready queue membership invariant", "deny-absolute single discipline runqueue 582",
  "audit scheduler comovement breaker sweep", "edf_orphan_rescue INV-RQ-4 stranded sweep" → **ignored,
  empty result set**: the vault has no #610-specific note; the normative text lives in
  `docs/specs/scheduler.md` (INV-RQ-1..5, read directly).
- Code read for the plan (files-as-truth, graph may lag): `task_queue.cpp` (all), `ready_queue_manager.cpp`
  (all), `scheduler_dispatch.cpp` (all), `scheduler_queue.cpp:1-660`, `scheduler_tick.cpp:30-600`,
  `scheduler_registry.cpp:540-780`, `scheduler_switch.cpp:190-356/670-840`,
  `scheduler_debug_internal.hpp:30-95`, `test_waitpid.cpp:1100-1260` (read-only).

## AFFECTED FILES
- `src/kernel/task/scheduler_queue.cpp` — add `sched_queue::edf_contains()` (bounded, `is_valid`-guarded
  reachability proof over `edf_ready_[cpu]`, mirror of `TaskQueue::contains`); reuse it in
  `move_priority`/`migrate_dispatch_queue` only if it is the convicted site; add the orphaned-chain
  diagnostic on the `edf_peek_valid_own` magic-bad-head fail-closed leg (report, do not re-adopt links).
- `src/kernel/task/scheduler_queue_internal.hpp` — declare `edf_contains()` next to `edf_detach`.
- `src/kernel/task/scheduler_tick.cpp` — the #600 P2 rescue block (currently lines 319-347): stop
  trusting `in_edf_queue_` as chain membership; prove reachability and rescue the
  flagged-but-unreachable (and unreachable-anywhere) READY case through `edf_detach` + `enqueue_ready`,
  counting on `edf_orphan_rescue_count_` (INV-RQ-4's named oracle — no new counter).
- `src/kernel/task/scheduler.hpp` — doc-only: `edf_orphan_rescue_count_` / `SchedOracles` comment gains
  the "flag is not membership" contract line. NO field added (staged #604 touches this struct; additive
  changes are #604's call, not ours).
- `docs/specs/scheduler.md` — INV-RQ-4 wording: "member of neither the bitmap level … nor the EDF chain
  (proved by physical chain reachability, never by `in_edf_queue_`)".
- `prompts/LEARNINGS.md` — one compressed entry after the fix is green (PROMPT-dev §5).
- Validation-only scratch (NOT committed): `tools/gdb/610-wedge-capture.gdb`, `/tmp/610-*.log`,
  `audits/plan-610.md` (this file).
- Test lane (QE-only, dev must not touch): `src/kernel/test/test_waitpid.cpp` (2 partial shapes),
  `test_expected_counts` rows, `test-history.txt` rows.

## INVARIANTS & CONCURRENCY BOUNDARIES
- INV-RQ-1: every ready-queue mutation body is ONE `arch::IrqGuard` unit (`task_queue.cpp:13-185`,
  `ready_queue_manager.cpp:33-111`, `scheduler_queue.cpp:80-123`). Any new EDF helper follows the same shape.
- INV-RQ-2: a typed refusal leaves the TCB byte-identical (no defensive flag writes on the false path).
- INV-RQ-4 (the invariant this bug violates): "every READY task owned by this CPU and member of neither
  the bitmap level for its effective priority nor the EDF chain is rescued within one tick, irrespective
  of `edf_eligible`" — spec `docs/specs/scheduler.md:87`. The implementation at
  `scheduler_tick.cpp:323-325` tests the FLAG (`in_edf_queue_`) instead of chain MEMBERSHIP, so a
  flag-true / chain-absent task is never rescued: the spec deviation is the plan's target.
- Membership-authority doctrine (#582 "deny-absolute"): intrusive LINKS are truth for the bitmap runq
  (`task_queue.cpp:124-141`), while `EdfReadyList` is still flag-gated (`scheduler_queue.cpp:107-123`,
  documented asymmetry at `:174-184`). This issue lives exactly in that asymmetry; the fix must extend
  the link-authoritative rule to the EDF list, not weaken the runq rule.
- Exactly-one-dispatch-structure per READY task; RUNNING never queued (current-never-queued); the
  `ready_deferred_` latch is only legitimate while the task IS `current_task()`
  (`scheduler_queue.cpp:396-416`; sweep mirror at `scheduler_tick.cpp:352-361`).
- Deferred-switch ownership (#590/#593, rev-6 fences): `switch_to_task` publishes the arm AFTER dequeuing
  the target and setting it RUNNING (`scheduler_switch.cpp:810-838`); every arm-discard leg MUST undo via
  `restore_preempted_current` (`scheduler_switch.cpp:316-356`). No step in this plan touches that path.
- Lock scope/order: the #600/#582/#249 sweeps run inside the adopted `scheduler_lock_`
  (`scheduler_tick.cpp:102-104`, T2b no-reentrant-take). `edf_contains()` is a pure read of links
  and runs ONLY under the caller's held `scheduler_lock_` (or IF=0); it is NOT independently
  lock-free/ISR-safe — the "lock-free like `TaskQueue::contains`" phrasing is withdrawn per r1.
  New call sites add no lock, hold no leaf lock, allocate nothing.
- Bounded loops only (CODING_STYLE §6): `edf_contains()` walks at most `STRANDED_RESCUE_SCAN_MAX`
  (= `CONFIG_MAX_TASKS`), `is_valid`-guarded break, exactly like `edf_peek_valid_own` INV-RQ-5.
- `Scheduler::preempt_enabled_` and the tick `try_lock` gate must be true/free for the sweep to run at
  all — the validation plan reads both before concluding anything.

## STEP-BY-STEP CHANGES

### Phase A — validation FIRST (no production logic change; TEMP-DIAG is reverted verbatim)
1. `git branch --show-current` must print `main`; `git log --oneline -1` must be `92378b81`; capture
   `git status --porcelain` so the staged #604 set is provably untouched at the end (diff the two lists).
2. Retrieve-before-implement artifacts: paste the graphify/vault queries + dispositions from the
   RETRIEVAL block above into the #610 work-begun comment (audit check-7).
3. Reproduce on the TCG station (nexios-qe container, never the Mac as the sole witness):
   `python3 tools/meson/qemu_test.py --arch x86_64 --build debug --class process_waitpid` x5 with
   `TEST_SERIAL_LOG=/tmp/610-run<N>.log`, `uptime` recorded per run; expect ~2/5 red at
   `test_waitpid.cpp:1235`. Append one `test-history.txt` row per run (AGENTS.md Test Result History).
4. TEMP-DIAG (revert after conviction; CONFIG_DEBUG only, no alloc, bounded; GDB capture in
   step 5 is PRIMARY for TIMEOUT legs — serial-in-ISR under TCG can mask the flake or manufacture
   stalls, so a TIMEOUT leg is never convicted on DIAG alone):
   - `scheduler_tick.cpp` sweep: for each `edf_eligible` READY non-current own-CPU task print
     `ticks id state in_edf in_rq chain_head_id chain_has_task rescue_branch`;
     throttled: at most once per 100 ticks per id, only for the ids under test (>=3 ids), and
     rescue/drop branches always print (unthrottled — rare by construction).
   - `scheduler_queue.cpp`: print at `edf_insert`, at `edf_remove` ONLY on the flag-gated
     no-op case (skip the healthy removal fast path), at `edf_peek_valid_own` drop/nuke legs, and
     at `edf_preempt_candidate` return ONLY when it returns nullptr for an eligible task;
     `next_task()` (dispatch unit) prints tag+id of the pick ONLY when the pick differs from the
     previous tick's pick (suppress the steady-state spam that perturbs dilation timing).
   - Print `preempt_enabled_`, the tick `try_lock` result, and `arch::Timer::ticks()` in the same line.
   - CONTROL RULE (anti-self-defeat): every DIAG red must be followed by one clean (DIAG-free)
     rerun of the same class; if the DIAG run greens while clean runs red at the base rate, the
     DIAG is perturbing the timing — discard the DIAG transcript and convict on GDB only.
5. Static wedge-state capture recipe (use when the class TIMEOUTs instead of failing; store the transcript):
   `python3 tools/meson/gdb_run.py debug-test --arch x86_64 --build debug --class process_waitpid --gdb-script tools/gdb/hang-capture.gdb`;
   confirm symbols first (`info variables edf_ready_`), then read, in one batch:
   `tasks` (plugin) + `kernel::sched_queue::edf_ready_[0].head_` + walk `edf_next_`/`edf_prev_` +
   per-node `state`/`in_edf_queue_`/`in_ready_queue_`/`deadline_ticks`/`id` +
   `Scheduler::current_task()` and the live RSP/RIP (`regs`) + `preempt_enabled_` +
   `read_sched_oracles()` (11 counters) + the `[S582-LOCK]`/`lockpark` serial line.
6. CANDIDATE DECISION TABLE (run before writing any fix; each row is a conviction criterion):
   - C1 (convicted by design reading + a timing-only producer from validation): task READY + `in_edf_queue_==1` + NOT reachable from
     `edf_ready_[0].head_` ("flagged-but-off-chain") → INV-RQ-4 is violated by construction
     (`scheduler_tick.cpp:324-325` `continue`). Also expect `edf_chain_drop_count_>0` if the
     magic-bad-head/null-head leg, or the INV-RQ-5 nuke, orphaned the chain, and
     `edf_orphan_rescue_count_==0` (the sweep never fires). WITHOUT a named timing-only producer this row is #601's residue, not a #610 conviction — H1 then demotes and H2 becomes the fix target. FIX = Phase B.
   - C2/H2: task READY + `in_edf_queue_==0` + `in_ready_queue_==0` + not current → the #600 P2 sweep
     SHOULD have rescued; then the sweep never ran: check `preempt_enabled_`, the `try_lock` miss
     counters, `current_task()==task` (stale-current cache) and `queue_target(task)!=sched_cpu()`.
     If any of those fired → the strand is in the sweep's GATES, not its membership proof; fix that gate
     only (no EDF change). If none fired and `edf_orphan_rescue_count_>0`, the task is rescued and
     re-stranded every tick → conviction moves to C3.
   - C3: `stuck_rescue_count_`/`orphan_rescue_count_`/`edf_orphan_rescue_count_` climbing once per
     tick while the task never runs → dispatch-side drop loop: instrument `next_task()`
     (`scheduler_dispatch.cpp:63-93`) pick vs. returned id, and the arm atoms
     (`save_rsp_to`/`next_task_id`) around the `switch_to_task` publish and the epilogue apply. If the
     arm is dropped without `restore_preempted_current`, STOP: that is #590/#593 fence territory → close
     this lane, re-plan with the auditor (out of scope for #610).
   - C4: racer NOT in `all_tasks_` / `find_task(id)!=&task` → enqueue H2 guard refusal
     (`scheduler_queue.cpp:379-387`) strands READY silently; if so the fix is a counted+traceable refusal,
     still inside the bounded surface.
   - C5 (only if every counter is 0 and the chain IS reachable+eligible with the task dispatched every
     tick yet never executing): the residual is the test seam (observe-only) + `set_task_ready` of a
     BLOCKED parent — hand back to QE as a test artifact; dev lands no code. Note test 8 uses
     `seam_noop` (`test_waitpid.cpp:1151`), so the seam cannot mutate task state by construction; only
     the snapshots are racy. (C5 inherits the step-4 DIAG-perturbation caveat: a seam verdict is valid only on DIAG-free runs — serial prints inside the observe window can stretch the very race the snapshots sample.)
7. Revert the TEMP-DIAG with `git checkout -- <files>` and prove it with `git diff` (LEARNINGS: no
   TEMP-DIAG survives a session).

### Phase B — fix, ONLY for the C1-with-producer convicted candidate
1. `scheduler_queue_internal.hpp`: add
   `bool edf_contains(const TaskControlBlock &t, uint64_t cpu) noexcept;` with the same "reachability,
   not flags" contract wording as `TaskQueue::contains`.
2. `scheduler_queue.cpp`: implement `edf_contains()` immediately after `edf_insert` — bounded walk
    (`STRANDED_RESCUE_SCAN_MAX` = `CONFIG_MAX_TASKS`), `t.magic == TCB_MAGIC` break, `&t` identity match; no writes, no lock
    (pure read of links; caller holds `scheduler_lock_` — see INVARIANTS correction per r1).
3. `scheduler_queue.cpp` `edf_peek_valid_own` magic-bad-head leg (`:201-207`): keep the fail-closed
   `l.head_ = nullptr` (INV-RQ-5) but state in the comment that the stranded remainder is now recovered by
   the reachability sweep; no link adoption.
4. `scheduler_tick.cpp` #600 P2 block (`:319-347`): for `edf_eligible(*task)` replace
   `if (task->in_edf_queue_) continue;` with
   `if (task->in_edf_queue_ && edf_contains(*task, sched_cpu())) continue;` and, before the rescue, on the
   flagged-but-unreachable shape call `edf_detach(*task)` (typed, flag+links as one unit) exactly as
   the runq side calls `rq_task(*task).detach(*task)` at `:369`; then fall through to the existing
   `enqueue_ready(*task)` + `need_resched` store, counting on `edf_orphan_rescue_count_` (same message
   and `STRANDED_RESCUE_WARN_EVERY` throttle, op-ring dump on the first).
5. `scheduler_tick.cpp`: clear `task->ready_deferred_` for the rescued non-current task (mirror of
   `:330`), never clobber a non-READY state (H2 guard unchanged).
6. `scheduler.hpp`: extend the `edf_orphan_rescue_count_` doc comment with the flag-vs-chain contract.
   Do NOT add a counter field (staged #604 owns `SchedOracles`).
7. `docs/specs/scheduler.md`: INV-RQ-4 row wording as above; add the flag/link asymmetry pointer in §"EDF".
8. Guard rails for this phase: no edit to `task_queue.cpp`, `ready_queue_manager.cpp` enqueue/dispatch
   semantics, `scheduler_dispatch.cpp`, `scheduler_switch.cpp`, any `isr_stubs.asm`/arch entry, or the
   #598 breaker block; the new rescue keeps its current lock position and must not move the breaker's
   victim selection (rescue happens in the same adopted-lock block, before accounting).
9. Fidelity checks to state in the commit/issue comment: #582 (typed refusals + one IrqGuard + link
   authority preserved), #600 (P2 sweep now matches its own INV-RQ-4 text; P3 chain-drop fail-closed
   untouched; P1 nowhere-linked scan untouched), #598 (breaker path untouched; rescue only adds READY
   members the breaker may then serve).

### Phase C — post-fix obligations (dev lane)
1. `tools/meson/build.sh` x86_64 debug + `--arch aarch64` + `--arch riscv64` (3-arch build parity).
2. Gates in TEST STRATEGY; then `graphify update .` and the Obsidian mirror note (PROMPT-dev §5).
3. Auditor call (Phase 3, production diff): `git diff main -- <files> > audits/pending_patch.diff`.
   No commit, no push, no `fixes #610` from this lane.

### Phase D — revert discipline & STALL-retry budget (host-load aware)
1. Every experiment is one variable: TEMP-DIAG only, or one fix hunk only. Any run that does not move
   the signature is REVERTED (`git checkout -- <file>`) before the next hypothesis — never stacked
   (AGENTS.md Revert on Wrong Hypothesis, max 3 attempts then HALT with the evidence).
2. STALL budget: a TIMEOUT/PANIC leg (not a FAIL line) is a stall, not a regression signal — record it as
   `STATUS: TIMEOUT|PANIC` with `uptime`/load average in the history row, and only re-run once the host
   load is comparable (this lane's prior evidence: nexios-qe is the DILATED station, the Mac is the fast
   control; a green Mac run never exonerates a red TCG run, and a red Mac run under load>2 is not
   evidence). Identical-binary rerun before any code change when a stall appears (LEARNINGS: TCG stall).
3. After 3 consecutive same-signature reds on the STATION with a verified-in-tree fix: HALT, post the
   serial transcripts + oracle counters to #610, do not widen the diff.
4. Never "fix" by lengthening the test bounds (test files are QE-owned and read-only for dev) and never
   dismiss the failure as pre-existing (AGENTS.md Pre-Existing Failures).

## TEST STRATEGY
- Test sanctity: **dev lands ZERO test edits.** `test_waitpid.cpp`, `test_expected_counts`, and any new
  test are QE-owned on `testbed`. Dev may only *read* them (done) and request shapes.
- New tests (QE owns; stub-first per PROMPT-dev §3 — two partial shapes):
  1. `sched_edf_chain_flag_desync_rescued` — build a READY EDF-eligible task, force the
     flagged-but-off-chain shape (`edf_detach` free of links / head-null), tick once, assert the task is
     reachable in `edf_ready_[cpu]` again, `edf_orphan_rescue_count_==1`, and no double-link/cycle
     (walk bounded, assert counts).
  2. `sched_edf_orphan_sweep_idempotent` — run the tick against a HEALTHY chain (all flagged+reachable)
     and assert the rescue counter stays 0 and the chain order (deadline/prio/id) is byte-identical, i.e.
     the new reachability proof never reshuffles a healthy list.
  3. QE hardening request for the existing FAIL line: print `in_edf_queue_` alongside `in_ready_queue_`
     in `test_waitpid.cpp:1236-1248`, because for EDF tasks `in_rq==0` is EXPECTED — the current
     diagnostic conflates bitmap membership with dispatch membership (this is the tractability fix for the
     next flake, not a kernel change).
- Flake green bar (QE, nexios-qe TCG station, host load recorded per run):
  `--class process_waitpid` x10 consecutive → 10/10 with 11/11 PASSED (0.6^10 ≈ 0.6% false-pass
  against the 2/5 base rate; 5/5's 7.8% is insufficient per r1), plus one
  `--class proc_elf` and one `--class core_sched` for cascade evidence (test-9 `g_pidok` must not fire).
- Existing classes to run (x86_64 debug, per-class via qemu_test.py only):
  `process_waitpid` (primary), `proc_elf` (waitpid/elf shadow), `core_sched`, `core_task`,
  `core_syscall`, `deadline` (EDF chains), `smp_waitpid` (x86-only policy row), `smp` (affinity/
  `queue_target`), `selftest` (CI gate). Rationale: the touched surface is EDF membership + the tick
  sweep, so every class that asserts chain order or READY rescue must be re-proven.
- Off-arch legs: `task_core`/`process_lifecycle` on aarch64 debug and
  `scheduler_zombie`/`task_core` on riscv64 debug (aarch64 `core_sched` dropped — known STALL,
  gate-excluded per r1; class names from `tools/meson/list_tests.py:24-43`);
  3-arch builds mandatory even though the bug was only seen on x86_64 TCG (AGENTS.md Cross-Arch Parity).
- Release leg: `python3 tools/meson/qemu_test.py --arch x86_64 --build release --class selftest` (the
  sweep code compiles into release; `sched_runq_trace` is type-only there — verify no behavior divergence,
  CODING_STYLE §6).
- History rows: exactly one `test-history.txt` row per invocation, both lanes.

## SIL 3 / SAFETY RISKS
- [S1] The fix changes a *liveness rescue inside the timer ISR*: an over-eager rescue could enqueue a
  RUNNING/current task or a corpse, breaking current-never-queued / INV-2 — mitigation: keep the existing
  magic/idle/current/state/queue_target gates untouched, only replace the membership test; `edf_contains`
  is read-only; `edf_detach` is the typed flag+links unit already used at `rebuild_ready_queue`.
- [S1] Adding a walk inside the tick enlarges ISR work under `scheduler_lock_` (WCET): mitigation —
  bounded by `STRANDED_RESCUE_SCAN_MAX`, `is_valid` early-break, one walk per edf-eligible READY task,
  no logging on the healthy path (warn only on rescue, throttled) — do not add a second nested walk.
- [S1] Mistaking the diagnosis and "hardening" the wrong structure risks regressing the #590/#593 deferred
  switch fences (arm-drop/orphan re-enqueue). Mitigation: candidate C3 halts the lane and re-plans with the
  auditor; Phase B is scoped to `scheduler_queue.cpp` EDF helpers + `scheduler_tick.cpp` sweep only.
- [S2] A stale `in_edf_queue_==1` cleared by `edf_detach` while the node is actually reachable would
  orphan a healthy node: mitigation — `edf_contains()` is the gate for the detach (detach ONLY after
  proven absence), identical to the runq side's `sweep_bitmap_absent` ordering.
- [S2] Reusing `edf_orphan_rescue_count_` for both the old and new rescue shapes could hide which one
  fired: mitigation — the warn line distinguishes "EDF-eligible stranded READY" vs
  "EDF flagged-but-off-chain" and the first firing dumps the op-ring.
- [S2] Counter semantics are test-visible (must-read-0 discipline): a new firing class must never appear in
  a healthy run — QE's idempotence test (new test 2) is the guard.
- [S3] If the true mechanism is C2 (sweep gate), the EDF change is unnecessary churn: mitigated by the
  decision table ordering (C1 conviction requires the off-chain evidence, not just `in_rq==0`).
- [S3] QE's current FAIL line will keep misleading future triage until it prints `in_edf_queue_`
  (test-lane request, not a kernel risk).

## ORDERING vs STAGED #604 (r1 rule — binding)
- This plan's Phase B touches `scheduler_queue.cpp`, `scheduler_tick.cpp`, `scheduler_registry.cpp`
  (read-only: doc comment), `scheduler.hpp` (doc comment) — the same four files #604 C1c/C4 have
  STAGED uncommitted. No new oracle field is added here precisely to avoid a `SchedOracles`
  conflict (verified: accessors only, no new storage — r1 confirms).
- Rule: Phase A (validation, no production change) may run any time. Phase B (fix) lands ONLY
  AFTER #604 is committed on main, with anchors re-verified (the deletions shift tick/registry
  lines); if #604's C4 changes an accessor body the family composes from, re-audit the composition.
  If #604 slips, Phase B waits — no parallel production hunks in the same files.
- Staged #604 work currently occupies `Scheduler::SchedOracles`: confirm the auditor accepts landing this
  fix BEFORE #604 (plan adds no oracle field to avoid a conflict), or sequence #604 first.
- If validation convicts C3 (deferred-arm drop without restore), #610 is out of this lane's fence —
  confirm it becomes a #590/#593 follow-up issue rather than an in-place fix here.
