# AUDIT REPORT 2026-11-21 — issue #600 (design review r1, plan-600.md)
PATCH: n/a — design review of audits/plan-600.md (no pending_patch.diff; plan is uncommitted scratch)
FILES: audits/plan-600.md (reviewed against src/kernel/task/scheduler_queue.cpp, scheduler_tick.cpp, scheduler_snapshot.cpp, ready_queue_manager.cpp, scheduler_registry.cpp, scheduler.hpp, task_queue.cpp, src/kernel/ipc/ipc.cpp, src/kernel/syscall/syscall_handlers_misc.cpp, syscall_handlers_thread.cpp, src/kernel/task/scheduler_lifecycle.cpp, src/kernel/debug/ipc_sched_trace.hpp, audits/plan-582-final.md)
BASE: 1f76c839d verified via git log --oneline -1 (matches plan header).

## FINDINGS
- [S3] src/kernel/task/scheduler_queue.cpp:175-190 (Part 3 force-drop) — drop path adopts c->edf_next_ as the new head without a magic/validity check on c, but the predicate-fail case includes magic-bad heads.
  WHY: Reading edf_next_ of a magic-bad node installs a possibly-wild pointer as list head; the bounded guard + next-iteration magic check make it non-fatal, but fail-closed prefers head=nullptr (or skip adoption) when magic is bad.
- [S3] src/kernel/task/scheduler_tick.cpp:593-598 (Part 2 EDF branch) — EDF-eligible + in_edf_queue_==true + unreachable-from-head stays invisible to both the sweep (flag skip) and edf_peek_valid_own (chain-only).
  WHY: Corruption-premise only (all EDF writers are typed IRQ-atomic units), and Parts 2+3 strictly shrink the blind spot, but the residual has no oracle; record a follow-up (e.g. debug-only full-registry EDF membership audit) so the family stays closed.
- [S3] src/kernel/task/scheduler_queue.cpp:469-479 (Part 4 scope) — Scheduler::move_priority's EDF branch (edf_resort: edf_remove+edf_insert composite) keeps the same remove-then-publish window shape that Part 4 closes on the bitmap side.
  WHY: Self-healing (≤1 tick missed preemption, re-sort on next move) and out of the evidenced hang shape, but by the plan's own step-18 precedent it should be a recorded follow-up, not silently open.

## VERIFICATION SUMMARY (the 9 reframing questions)
1. Broken diagnostic — CONFIRMED: scheduler_queue.cpp:607-611 single index q as CPU+prio; rq_for masks c % CONFIG_MAX_CPUS (scheduler.hpp:1344-1346); CONFIG_MAX_CPUS=8, CONFIG_PRIORITY_CEILING=127 so pairing matches iff level%8==cpu. Sweep scan (scheduler_tick.cpp:600-604) correctly uses rq_task(*task) (affinity-derived CPU) and finds phys=true — ZERO-rescue consistency explained. Plan honestly lists the masking risk (genuine strand indistinguishable from noise) as S1 — no overclaim.
2. EDF skip — CONFIRMED: sweep skip at :593, restore-rebuild skip at scheduler_snapshot.cpp:96-97, peek chain-only at :175-190. Fix sound: state-gate move only reorders two skip gates with identical outcomes; rescued EDF tasks route via enqueue_ready_inner :426-432 to edf_insert (never bitmap); stale-flagged case converges through typed RQM::remove forced-clear (counted).
3. Unbounded EDF walk — CONFIRMED §6 violation: while(l.head_) at :178 + edf_remove flag-gate no-op at :109-111 = non-advancing loop when head fails predicate with in_edf_queue_==false. Bounded guard + force-advance + fail-closed cycle path is sound; no new locks; cycle-path chain nuke re-converges via Part-2 sweep + re-insert on next ready transition.
4. move_priority window — CONFIRMED real and harmful: RQM::move_priority :191-203 and Scheduler::move_priority :469-479 carry no guard; set_priority calls move BEFORE writing the field (scheduler_registry.cpp:116-118), so an ISR interleave rescues at the pre-move level and the resumed enqueue refuses AlreadyQueued = stale bucket + noise. One outer IrqGuard closes it; nesting legal (irq_guard_depth per-CPU counter); no scheduler_lock_ take added, no lock-order change.
5. Option (c) rejection — SOUND: reuses plan-582-final.md §12 verbatim precedent ("Scanning for membership instead of the flag on every enqueue — O(n) on the dispatch hot path (WCET)", verified present). Escape hatch exists: Part-0 table last row (stranded, no rescue) stops and files separately instead of guessing — the hang is not assumed sweep-covered.
6. Part 0 gate — CAPABLE: stash control at df5172beb^ isolates #597; GDB census captures flag/link/chain/bitmap/tick-advance; 5-row table maps each outcome to an owning part or a fail-safe (follow-up issue / escalate / stop). Proceed-regardless is justified since Parts 1-4 are defects by code-read.
7. #597 verdict — CONFIRMED: df5172beb touches exactly 3 sites, each adding only p->is_user_ && around the RAX/a0 frame store; set_task_ready calls precede the store unchanged; waitpid/exit path only, no IPC/runq/lock effect. Never-revert directive sound.
8. Scope — CLEAN: no new locks, no asm, generic task code (arch-neutral); test sanctity holds (no test files in affected list; QE owns testbed sketches + test_isolate resets); gates cover ipc/ipc_core/ipc_blocking/scheduler_core/core_sync × LTO+NOLTO + off-x86 + 3-arch + style + history + stash control.
9. Check-7 — SATISFIED: appendix carries 4 graphify queries + 1 shortest_path + 2 get_node with per-item dispositions, honest vault-search negative, and applied prior art (plan-582-final §5/§6.3/§12, design-review-582*.md, report-20261118-111146-582.md — all verified present; LEARNINGS #249/#250/#345 entries verified). Plan-600 is the first #600 artifact (no vault note exists yet) — correctly stated.

False-close guard + open item: plan requires the Part-0 decision table to fire before the hang-fixed claim/close (step 20 + OPEN QUESTIONS) — correctly gated as DONE-blocker, not implementation-blocker.

## PATCH
Omitted (APPROVED; S3 notes are advisory for the implementer, no corrective diff required).

DECISION: APPROVED
