# PLAN: RUNQ flag/link desync — repair the reporter, close the EDF sweep blind spot, bound the EDF list walk (issue #600)

> Lane: dev, branch main @ 1f76c839d. Production code only. **This plan is uncommitted scratch
> evidence** (audits/plan-600.md) — commit nothing until the SIL 3 audit returns APPROVED.
> Phase order is mandatory: **Part 0 (evidence) → Part 1 (truth) → Part 2 (sweep coverage) →
> Part 3 (bounded EDF walk) → Part 4 (window closure)**, each part independently revertible.

RE-READ OF THE FILED NARRATIVE (correction the implementer must carry):

1. The line QE counted — "[RUNQ] already-queued but nowhere linked id=N" — is produced by a
   **broken diagnostic** (`scheduler_queue.cpp:607-611`), not by a proven desync: it tests
   `rq_for(q).queue(q)` with ONE index used as CPU *and* priority. `rq_for(q)` masks
   `q % CONFIG_MAX_CPUS` (8), so the pairing only matches when `level % 8 == cpu`. Every
   duplicate-wake refusal on a legitimately queued task at any other level prints the ERROR.
   Hand check: harness id=1 at level 10 on CPU0 → judged "nowhere linked" although physically
   linked; task id=10 at level 12 → same. That is the "hundreds of lines on ids 1/4/9/10".
2. Consistent consequence: the sweep **never** rescues these tasks (`ZERO sched rescued` in the
   QE log) because its own scan is correct (`rq_task(*task).queue(p)`, `scheduler_tick.cpp:600-604`)
   and finds `phys=true`. "No rescue" is therefore **not** evidence of a strand in tests 16-18.
3. The only always-correct desync detector (tick orphan scan, `scheduler_tick.cpp:290-321`) is
   compiled out — `CONFIG_DEBUG_IPC_SCHED` is commented out (`src/kernel/debug/ipc_sched_trace.hpp:34`).
   Debug runs therefore have **zero trustworthy** link/flag observability.
4. Two structural blind spots remain real and are fixed here: the #249/#582 sweep skips every
   **EDF-eligible** task (`scheduler_tick.cpp:593`) although `edf_peek_valid_own` can only see the
   EDF chain, and the snapshot restore rebuild repeats the same skip
   (`scheduler_snapshot.cpp:96-97`).
5. #597 assessment (df5172beb): the `is_user_` gate touches only `wake_waiting_parent`'s RAX store
   (`syscall_handlers_misc.cpp:207`, `syscall_handlers_thread.cpp:252`,
   `scheduler_lifecycle.cpp:690`) — the waitpid/exit-record path, **not** the IPC block/wake-sender
   or ready-queue paths. It can only shift timing/stack contents, never link/flag ordering.
   Do **not** revert it to recover a test: it is the fix for a corrupted parent return slot.

## AFFECTED FILES
- src/kernel/task/scheduler_queue.cpp — `Scheduler::note_runq_enqueue_outcome` correct membership
  scan + throttled counted violation; `edf_peek_valid_own` bounded, self-repairing head walk
  (Part 1, Part 3).
- src/kernel/task/scheduler_tick.cpp — #249 sweep: EDF-eligible stranded-READY rescue, state gate
  moved above the EDF gate, new counter + throttled warn (Part 2).
- src/kernel/task/scheduler_snapshot.cpp — `restore_rqpod` rebuild: same EDF-eligible rescue
  instead of the blind `continue` (Part 2).
- src/kernel/task/ready_queue_manager.cpp — `move_priority`: remove+enqueue become one
  `arch::IrqGuard` unit (Part 4).
- src/kernel/task/scheduler.hpp — 3 new counter declarations + doc block (oracles, must-read-0,
  never rewound), accessors/resets declared (Part 1-3).
- src/kernel/task/scheduler_registry.cpp — counter definitions + accessor/reset bodies next to
  `stuck_rescue_count_` / `orphan_rescue_count_` (`:88-89`, `:346-358`).
- **QE-owned, dev must not touch:** src/kernel/test/** (incl. test_isolate.cpp reset wiring),
  test_expected_counts rows, initrd test config, test-history.txt rows appended by the harness.

## INVARIANTS & CONCURRENCY BOUNDARIES
- INV-RQ-1 (unchanged, extended): `in_ready_queue_`/`runq_next_`/`runq_prev_`/`rq_priority_`
  are written only inside `TaskQueue`, one IRQ-atomic unit each. `move_priority`'s two-step
  remove→enqueue is a **composite** outside that rule today; Part 4 makes it one `IrqGuard` unit
  (nested guards are legal, T2b constrains `scheduler_lock_` only).
- INV-RQ-4 (new): every READY task that is owned by this CPU and is a member of **neither** the
  bitmap level for its effective priority **nor** the EDF chain is rescued within one tick —
  irrespective of `edf_eligible`. The rescue is the only legal writer of membership from the sweep.
- INV-RQ-5 (new): `edf_peek_valid_own` terminates in ≤ `STRANDED_RESCUE_SCAN_MAX` iterations
  (CODING_STYLE §6 bounded-loop rule). A head whose `in_edf_queue_` is false cannot be spliced by
  `edf_remove` (`scheduler_queue.cpp:109-110`, flag-gated — the exact asymmetry `try_unlink`
  documents and rejects for the runq, `task_queue.cpp:126-141`); it is force-dropped and counted.
- IRQ-off windows: sweep and `edf_peek_valid_own` run under `scheduler_lock_` (`on_tick` held
  region / dispatch) — no new lock, no new take, no change to the lock-order table.
- No allocation, no logging on hot allow paths; warnings throttled first/64th
  (`STRANDED_RESCUE_WARN_EVERY` precedent) so no detector can feed back into the race.
- Counters: tick/ISR-owned writes under `scheduler_lock_`, readers atomic, monotonic (never
  rewound by snapshot restore) — identical discipline to `stuck_rescue_count_` (`scheduler.hpp:1569-1580`).
- Debug/release parity: rescues and counters are **always on** in both builds; only the
  `[RUNQ]` text is `#if defined(CONFIG_DEBUG)` (logging-only divergence, CODING_STYLE §6).

## STEP-BY-STEP CHANGES

Option decision (recorded, not re-litigated):

| Option | Verdict |
|---|---|
| (a) link/flag atomicity at the desync site | **Partially adopted.** Every runq writer is already one `TaskQueue` IRQ-atomic unit post-#582; the only remaining composite is `RQM::move_priority` (Part 4). No writer can be "fixed" further without inventing one. |
| (b) sweep coverage extension | **Adopted (primary).** Closes the EDF blind spot in both the tick sweep and the restore rebuild; tick context, bounded, no hot-path cost. |
| (c) wake-path re-queue hardening | **Rejected for the hot path.** "Detach + re-link on AlreadyQueued" needs a membership scan on every duplicate wake — rejected in #582 §12 for WCET; the level-triggered sweep is the cheaper structural net. |
| (d) reporter truth + bounded EDF walk | **Adopted (mandatory precondition).** Without (d) the other two cannot be verified: the only visited desync signal is a false positive, and the EDF walk can freeze the kernel instead of reporting. |

**Part 0 — evidence gate (no code; satisfies Hypothesis-First before any edit)**
1. Post the code-read artifacts of this plan (the four defects above, each with file:line) on
   issue #600 as the `[HYPOTHESIS]`/`[EVIDENCE]` record; QE's serial log is the reproduction half.
2. Local reproduce attempt: `python3 tools/meson/qemu_test.py --arch x86_64 --build debug --class ipc`
   on the unmodified tree. Expected: either the QE stall or `94/2`-class completion.
3. Stash control (answers the #597 question, ~1 boot): same class on a scratch worktree at
   `df5172beb^` (#597 unmerged). Record both rows. If the pre-#597 tree also stalls near test 19,
   #597 is exonerated and the hang is a pre-existing defect surfaced by ordering — record it and
   proceed; **never** revert #597.
4. If the wedge reproduces, capture with
   `python3 tools/meson/gdb_run.py debug-test --arch x86_64 --build debug --class ipc --gdb-script <script>`
   in one stop: harness RIP + `sp`; `current_task()` {id,state,inrq,rq,eff}; census of every TCB
   {id,state,in_ready_queue_,rq_priority_,runq_next_,runq_prev_,in_edf_queue_,edf_next_,edf_prev_,eff};
   `rq_own().bitmap().raw_hi()/raw_lo()`; per-level head/empty; `edf_ready_[0].head_` chain;
   two samples of `arch::Timer::ticks()` to prove whether ticks advance.
5. Decision table from that capture (records which part owns the hang; Parts 1-4 are implemented
   regardless, they are defects by code-read):
   - bitmap bit clear for a non-empty level → add a Part-4b follow-up issue (bitmap/queue desync),
     do not widen this change.
   - `edf_ready_[0].head_` non-null and non-advancing / head with `in_edf_queue_==0` → Part 3 is
     the hang fix.
   - ticks frozen → the freeze is inside the ISR/dispatch walk (Part 3 territory); escalate.
   - a READY task with no membership and `edf_eligible` true → Part 2 is the hang fix.
   - a READY task with no membership, `edf_eligible` false, no rescue → sweep did not run
     (`scheduler_lock_` held across `on_tick`) → stop and file separately; do not guess.

**Part 1 — repair the reporter (scheduler_queue.cpp, `Scheduler::note_runq_enqueue_outcome`)**
6. Replace the `for (uint64_t q = 0; q <= CONFIG_PRIORITY_CEILING; ++q) rq_for(q).queue(q)` block
   (`:607-611`) with two explicit nested loops that never reuse an index:
   `for (uint64_t cpu = 0; cpu < CONFIG_MAX_CPUS; ++cpu) for (uint64_t lvl = 0; lvl <= CONFIG_PRIORITY_CEILING && !phys; ++lvl) if (rq_for(cpu).queue(lvl).contains(tcb)) phys = true;`
   — the same form the tick detector (`:307-311`) and the sweep (`:600-604`) already use.
   Keep it `#if defined(CONFIG_DEBUG)`, cold, one-shot-per-refusal semantics unchanged.
7. When the corrected scan still proves absence, count it before logging:
   `++runq_nowhere_linked_count_` and change the message to
   `"[RUNQ] already-queued but nowhere linked id=%lu (n=%lu)"`, throttled to the 1st and every
   `STRANDED_RESCUE_WARN_EVERY`th firing with an op-ring dump (`sched_debug::sched_runq_trace_dump(tcb.id, 8)`)
   on the first. Throttling the *text* must not throttle the *counter*.
8. scheduler.hpp: declare `uint64_t runq_nowhere_linked_count() noexcept;`,
   `void reset_runq_nowhere_linked_count_for_test() noexcept;`, storage
   `static uint64_t runq_nowhere_linked_count_;` with the #582 INV-RQ-3 doc shape (must-read-0,
   never rewound). scheduler_registry.cpp: define the storage beside `:88-89` and the accessor
   pair beside `:346-358` (atomic relaxed load / store).

**Part 2 — sweep + restore coverage for EDF-eligible READY tasks**
9. scheduler_tick.cpp (sweep loop `:582-650`): move the state gate up and split the EDF gate.
   Order becomes: magic → idle/current → `queue_target != sched_cpu` → **`state != READY`** →
   EDF branch → FIXED branch. EDF branch body:
   `if (edf_eligible(*task)) { if (task->in_edf_queue_) continue; /* bitmap proof below */ if (bitmap_absent) { ++edf_orphan_rescue_count_; warn(1st/64th) + op-ring dump on first; enqueue_ready(*task); need_resched; } continue; }`
   The bitmap proof reuses the existing level scan (extract it into a file-local helper
   `static bool bitmap_absent(const TaskControlBlock&)` used by both branches — one implementation,
   no duplicated loop). The FIXED branch keeps today's exact behaviour (including the
   `in_edf_queue_` skip at `:597-598`).
   Rationale for `enqueue_ready`: it routes EDF-eligible tasks to `edf_insert` via
   `enqueue_ready_inner` (`:426-432`), so one call site covers both lists.
10. scheduler_tick.cpp: the rescue counter for this branch is `edf_orphan_rescue_count_`; the
    existing `orphan_rescue_count_`/`stuck_rescue_count_` semantics stay byte-identical.
11. scheduler_snapshot.cpp (`restore_rqpod` rebuild, `:89-107`): replace the
    `if (edf_eligible(*t)) continue;` skip with the same rescue — if `in_edf_queue_` is set the
    EDF rebuild at `restore_task_fields` (`:198-206`) owns it, else prove bitmap absence and
    `enqueue_ready(*t)` + `++edf_orphan_rescue_count_`. No warn on the restore path (already one
    ERROR there); counter only.
12. scheduler.hpp/scheduler_registry.cpp: `edf_orphan_rescue_count()` /
    `reset_edf_orphan_rescue_count_for_test()` + storage, same discipline.

**Part 3 — bound and self-repair the EDF head walk (scheduler_queue.cpp `edf_peek_valid_own`)**
13. Rewrite `:175-190` as one bounded loop (`while (l.head_ != nullptr && guard < STRANDED_RESCUE_SCAN_MAX) { ++guard; ... }`).
    Keep the existing eligibility predicate verbatim. Split the drop decision:
    - `in_edf_queue_` true → `edf_remove(*c, cpu)` (unchanged).
    - `in_edf_queue_` false → **force the head forward** without following the node's stale links:
      `l.head_ = c->edf_next_; if (l.head_) l.head_->edf_prev_ = nullptr; c->edf_next_ = nullptr; c->edf_prev_ = nullptr;`
      then `++edf_chain_drop_count_` + warn on the 1st/every 64th (message
      `"sched: EDF head not membership-marked, dropped id=%lx"`) + one op-ring dump on the first.
14. Bound hit (chain longer than `STRANDED_RESCUE_SCAN_MAX` ⇒ a cycle): fail closed but not dead —
    bounded second walk (same bound) clearing each reachable node's `edf_next_/edf_prev_/in_edf_queue_`,
    then `l.head_ = nullptr`; `++edf_chain_drop_count_`, one `Logger::error` with the op-ring dump.
    No panic (§5.1: reachable-by-corruption), and READY tasks are re-established by the Part-2
    sweep on the next tick.
15. scheduler.hpp/scheduler_registry.cpp: `edf_chain_drop_count()` /
    `reset_edf_chain_drop_count_for_test()` + storage.
16. Comment block above the function states INV-RQ-5 and the `edf_remove` flag-gate asymmetry
    (cite `task_queue.cpp:126-141` as the runq precedent) so the next reader does not re-derive it.

**Part 4 — close the one remaining composite window (`ReadyQueueManager::move_priority`)**
17. ready_queue_manager.cpp `:191-203`: open the body with `arch::IrqGuard irq_guard{};` before the
    `old_prio == new_prio` test, so remove→enqueue is unobservable by the tick sweep. No other
    change: the typed refusal handling and the trace stay as-is. (Nesting inside callers that
    already hold `IrqGuard`/`scheduler_lock_` is legal.)
18. OPTIONAL, same file/shape (only if the Part-0 capture shows a sweep rescue of a task that a
    concurrent `dequeue_ready` was moving): add the same `IrqGuard` to `Scheduler::dequeue_ready`
    (`scheduler_queue.cpp:442-460`) so `edf_remove`+`remove` are one unit. Default: **not** in
    this change (blast radius); record it as a follow-up issue instead.

**Part 5 — evidence + docs**
19. Append one row per test invocation to `test-history.txt` (never rewrite existing rows).
20. Post on #600: Part-0 capture, per-part gate table, counter readings (all must-read-0 oracles),
    and the `audits/pending_patch.diff` path for the SIL 3 audit. No commit before APPROVED.
21. `docs/specs/scheduler.md`: one paragraph under the ready-queue section naming INV-RQ-4/INV-RQ-5
    and the three new oracles. No CHANGE to the existing settings/park contracts.

## TEST STRATEGY
- Dev lands **no** tests (test sanctity: src/kernel/test/** is read-only for this lane; counters are
  the dev-side contract, QE wires asserts/resets on testbed).
- QE sketch (testbed, KERNEL debug registration):
  - `sched_runq_nowhere_linked_no_false_positive` — a double wake on a task legitimately queued at a
    level with `level % CONFIG_MAX_CPUS != 0` (e.g. prio 12 on CPU0); asserts
    `runq_nowhere_linked_count() == 0`. This is the exact negation of today's defect and fails on
    the current tree.
  - `sched_edf_orphan_rescue_heals` — build an EDF-eligible (AUTO, period>0) READY task with no
    membership (detach + clear flags), run a tick, assert `edf_orphan_rescue_count() == 1`,
    `in_edf_queue_ == true`, and dispatchability. Fails today (sweep skip at `scheduler_tick.cpp:593`).
  - `sched_edf_head_force_drop` — inject a stale EDF head (head_ points at a task with
    `in_edf_queue_ == false`), call `Scheduler::needs_switch()`; asserts bounded return, head
    advanced, `edf_chain_drop_count() == 1`. Fails today (non-advancing `while`).
  - `ipc_wake_sender_restores_priority_runq_oracle` — the test-16..19 sequence with a teardown assert
    that `orphan_rescue_count|stuck_rescue_count|edf_orphan_rescue_count|edf_chain_drop_count|runq_nowhere_linked_count`
    are all 0. The QE regression bar for the #594/#600 signature.
  - `sched_move_priority_atomic` — move_priority hammered with tick interleave; counters 0 and the
    task ends at the new level (bijection, mirrors #582 GATE 2.3).
  - Reset wiring in `test_isolate.cpp` for the three new counters.
- Existing classes to run (dev gates, x86_64 **LTO and NOLTO**): `ipc`, `ipc_core`,
  `ipc_blocking`, `scheduler_core`, `core_sync`; then `aarch64` and `riscv64` legs of the same
  five classes (arch-neutral code, but the epilogue/dispatch paths differ) and a 3-arch link.
  Style: `make check-style` Errors 0. Revert criterion: any counter non-zero in a clean run, or
  any class regression → revert the offending part only (Parts 1-4 are independent).

## SIL 3 / SAFETY RISKS
- [S1] The only visited desync reporter is wrong (`scheduler_queue.cpp:607-611`), so a genuine
  strand is indistinguishable from noise and the only correct detector is compiled out
  (`ipc_sched_trace.hpp:34`) — mitigation: Part 1 (corrected scan + counted oracle), which also
  re-attributes the #594/#600 investigation on evidence.
- [S1] EDF-eligible READY tasks are invisible to both the sweep (`scheduler_tick.cpp:593`) and
  `edf_peek_valid_own` (chain-only) and to the restore rebuild (`scheduler_snapshot.cpp:96-97`) —
  a stranded EDF task stalls unwaived forever, no counter — mitigation: Part 2 (+ must-read-0
  `edf_orphan_rescue_count_`).
- [S1] `edf_peek_valid_own`'s `while (l.head_ != nullptr)` cannot advance when the head is not
  membership-marked (`edf_remove` is flag-gated, `scheduler_queue.cpp:109-110`), i.e. an unbounded
  loop in the dispatch/tick path — a freeze with no diagnostic and a CODING_STYLE §6 violation —
  mitigation: Part 3 (bounded walk + force-drop + counter).
- [S2] `RQM::move_priority` publishes READY + unlinked for one window
  (`ready_queue_manager.cpp:191-203`); the tick sweep may re-enqueue at the pre-move level, leaving
  a stale bucket (missed preemption) — mitigation: Part 4 (one `IrqGuard` unit).
- [S2] The sweep's absence proof is O(READY tasks × 128 levels × chain walk) inside the timer ISR
  every tick — a WCET concern for an RTOS, worsened by Part 2 adding one more branch — mitigation:
  unchanged bound (`STRANDED_RESCUE_SCAN_MAX`), one shared helper (no duplicated loops); the
  census-gated optimisation (one O(128) count comparison per tick, per-task proof only on
  disagreement) is **out of scope here** and filed as a follow-up issue.
- [S2] TCBs are recycled; Part-3's force-drop must never follow a stale `edf_next_` of a
  not-membership-marked node — mitigation: the head advance reads the pointer but never splices
  through it, and the cycle path is bounded.
- [S3] New counters need QE reset wiring in `test_isolate.cpp` and must-read-0 assert rows; dev
  ships accessors only (test sanctity).
- [S3] Do not "fix" the hang by reverting #597 (`is_user_` gate) — it is a correctness fix on a
  different path; the stash control only *attributes*, it never authorises a revert.

## OPEN QUESTIONS
- Is the QE serial capture of the wedged `ipc`/`ipc_core` run (testbed 48ef49199) attachable to
  #600? Parts 1-4 are implementable without it, but the "hang fixed" claim and issue close
  require the Part-0 decision table to fire — this is the only blocker on the DONE definition.

---

## APPENDIX — RETRIEVAL & DISPOSITIONS (audit check-7)
- `graphify query_graph "RUNQ already-queued-but-nowhere-linked in_ready_queue_ desync enqueue_ready set_task_ready"`
  → **used**: `set_task_ready` (`scheduler.hpp:590`), `enqueue_ready` (:1010), `RunqTraceEntry`
  op-ring, `audits/design-review-582.md`; confirmed the wake→enqueue routing has one funnel.
- `graphify query_graph "stranded READY rescue sweep orphan_rescue_count_ #249 scheduler_tick"`
  → **used**: `Scheduler::on_tick` (`scheduler_tick.cpp:70`), `ReadyQueueManager`, `TaskQueue`,
  `edf_peek_valid_own`, `reschedule`; **ignored** the logger/checked_ptr/timer nodes (no bearing).
- `graphify query_graph "reschedule park latch deferred switch apply gate scheduler_lock_"`
  → **used** for the park/apply boundary: it confirms no ParkLock/scheduler_lock_ take is inside the
  paths this plan edits; **ignored** `statemachine/*`, `elf_shared` nodes.
- `graphify query_graph "IPC send_sync reply_recv wake sender block priority restore"`
  → **used**: `IPC::wake_sender` (`ipc.hpp:268`), `block_sender` (:264), `unblock_sender_rollback`
  (`ipc.cpp:1771`); **ignored** `userspace/block-srv.c` (no kernel-side bearing).
- `graphify shortest_path set_task_ready -> enqueue_ready` → 2 hops via `Scheduler::defines`
  → **used**: single wake funnel confirmed.
- `graphify get_node` on `wake_sender` / `set_task_ready` → **used** for degree/location only.
- Obsidian vault search (`jarvis` vault): "stranded READY in_ready_queue_ orphan rescue scheduler
  strand" → 0 direct hits; "582"/"strand"/"scheduler" → `audits/design-review-582-r2.md`,
  `audits/audit-scheduler-tasks-v0.4.2.md` → **used** (prior #582 family coverage: the sweep
  direction table and the INV-RQ-1/2 discipline). No vault note exists for #600; this plan is the
  first #600 artifact.
- Repo prior art read and applied: `audits/plan-582-final.md` (§5 sweep, §6.3 the 17-writer
  window table, §12 rejected alternatives — the hot-path membership scan rejection is reused
  verbatim), `audits/design-review-582.md`, `audits/report-20261118-111146-582.md`,
  `prompts/LEARNINGS.md` #249/#250/#345/#582-active-cautions (`inrq==1` orphan detector blind to
  the wake-while-current shape; detach-without-flag clears strands; assert dispatchability, never
  exact flags).
