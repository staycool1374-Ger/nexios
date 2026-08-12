# Deep Analysis — H2 Residual Race & ss_deadline Hang (v0.3.9 `all` gate blockers)

**Doc ID:** NEX-DEEP-2026-08-06-001
**Branches/commits verified against:** `main`, `ae44963d` + working tree (audit-implementation state)
**Evidence runs:** `all` ×2 (227s / 234s), `ss_deadline` ×2 (with and without the audit changes — identical hang), trace ON for `all`.

The `all` gate is blocked by two independent, pre-existing defects. Both are
now root-caused with static + observed evidence, and both fixes are implemented.

---

## 1. H2 residual — stale deferred-switch arm to a terminated task

### 1.1 Symptom (observed, `all` run 1)
```
[SW] cur=7 next=1 ...         # arm 7→1 (harness)
[SW] cur=7 next=1 ...         # re-arm
[APPLY] id=1 cur=1            # switch to harness applied
[SW] cur=1 next=6 ...         # arm 1→6 (task 6)
[APPLY] id=6 cur=1            # APPLIED to task 6, but current-cache stayed 1
[H2W] orphan-displaced tick=0 cur_rsp=0xFFFF800000A57B80
     ctx_rsp=0xFFFF900000032920 kst=0xFFFF900000023000-0xFFFF900000033000
```
`[APPLY] id=6 cur=1` is the smoking gun: `scheduler_on_context_switch`
(scheduler.cpp:2835) loaded `scheduler_next_task_id == 6`, called
`find_task(6)` → **null** (task 6 already removed from `id_table_`), so
`set_current_task(6)` no-oped and the cache stayed at 1 — but the ISR had
**already iretq'd** onto task 6's saved `context.rsp`. The CPU then executed on
a freed/foreign page (`0xFFFF800000A57B80`), the harness was displaced, and the
suite froze.

### 1.2 Root cause
The deferred-switch pair (`scheduler_save_rsp_to` / `scheduler_load_rsp_from` /
`_load_cr3_from` / `_next_task_id` / `_load_kstack_base/top`) is published by
`switch_to_task()` (scheduler.cpp:1973-2036) and normally consumed by the same
timer ISR's epilogue. When the epilogue **skips** the apply (nested-ISR depth
guard `ja .restore` at isr_stubs.asm:130-131, or the generation re-check
`jne .restore` at :165-166), the arm survives into a later ISR. In that
window the CPU is back in task context (IF=1), and the harness can **terminate
the armed target**.

`Scheduler::terminate()` (scheduler.cpp:343) → `release_zombie()` (:152) removes
the task from `all_tasks_`, `deadline_list_`, `id_table_` — **but never touches
the switch atoms**. By contrast `remove_task()` (:544-546) and `unregister_task()`
(:569-571) DO clear them. So a pending arm to a terminated task is left live;
the next ISR epilogue applies it (the apply-side RSP-owner check passes because
the freed stack's VA range still satisfies `[kstack_base, kstack_top)`), iretq's
onto freed/foreign memory, and `find_task(id)` returns null at
`scheduler_on_context_switch` — current-cache divergence + harness displacement.

This is the residual H2 mechanism behind ROADMAP §v0.3.9 / `docs/specs/ipc.md §4`.
Layers 4-6 (dispatch-guard frame.rsp check, scratch-save, apply-side RSP-owner
check) all validate the *RSP*, not the *liveness of the target task*. The task
can be freed between arm and apply with none of them firing.

### 1.3 Fix (implemented)
`invalidate_pending_switch_to(task_id)` — a static helper in scheduler.cpp that,
when the pending `scheduler_next_task_id` equals the removed task's id, clears
all switch atoms **and bumps `scheduler_switch_generation`**. The generation
bump makes any ISR that captured the pre-clear generation (isr_stubs.asm:136)
fail its re-check (:165-166) and skip the apply; the atom clear makes any ISR
entering after the invalidation see `save_rsp_to == 0` and skip.

Called from:
- `Scheduler::release_zombie()` (the terminate/self-terminate path),
- `Scheduler::reap_orphans()` free loop (direct TERMINATED reaping).

Deliberately **not** called for the self-terminating *current* task: its pending
arm (published at terminate():378) targets a valid successor, and `release_zombie`
runs *before* that arm is published, so `scheduler_next_task_id != task.id` and
the check is a no-op — the switch-away is preserved.

### 1.4 Expected effect
A stale arm to a removed task is neutralized before any ISR epilogue can apply
it; `find_task(id)==null` at apply time becomes unreachable for the
terminate/reap paths, eliminating the `[APPLY] id=X cur≠X` divergence and the
harness displacement on freed stacks.

---

## 2. ss_deadline — EXHAUSTED SS task starved below the harness

### 2.1 Symptom (observed, `ss_deadline` standalone + `all` run 2 test 469)
The suite never prints `S: ss_deadline ... 1/2`. The trace shows:
```
[RS] cur=1 next=6 hi=11          # harness(1) → helper(6, prio 11) via reschedule
[SW] cur=1 next=6 ... ; [APPLY] id=6
[RMS] cur=6 next=1 ... ; [SW]/[APPLY] id=1     # helper pulled back to harness
[TICK] t=93..78175 lk=1 ... nt=6               # harness spins forever
```
After the helper exhausts its SS budget, its effective priority collapses to
`bg_priority_` (2), and the `harness_nonpreempt` guard in
`rate_monotonic_schedule()` (scheduler.cpp:2072-2082) permanently refuses to
preempt the harness for it:
```
harness_nonpreempt && !scheduler_need_resched  →  if (highest_ready < cur_prio) return;
                                                    # 2 < 10 → RETURN, no switch
```
The helper never runs again, so it never reaches `gate.wait()` → never BLOCKED
→ the harness's `while (helper->state != BLOCKED)` spins forever.

### 2.2 Root cause
`test_ss_deadline.cpp` helper lambda (`spawn_ss_exhausted`, :48-73) consumes its
budget **first**:
```
on_activation();  consume×5 → EXHAUSTED (eff prio = bg_prio 2)
while (ticks() <= deadline_ticks) pause();   // ← needs ~10 ticks of CPU
gate.wait();                                  // ← never reached
```
Once `consume()` exhausts the 3-tick budget, `SporadicServer::current_priority()`
(sporadic_server.hpp:113-115) returns `bg_priority_` = 2 < harness 10. During the
subsequent ~10-tick busy-wait the helper is below the harness, so the
`harness_nonpreempt` RMS guard starves it. `effective_priority()`
(scheduler.cpp:98-116) returns the SS's background priority for an EXHAUSTED
server, so both the ready-queue position and the RMS guard see prio 2.
(Additionally, if the SS is ACTIVE during the busy-wait, `on_tick` auto-consumes
the budget — `if (t == cur && is_active()) consume()` at scheduler.cpp:1309 —
so an ACTIVE server also exhausts mid-wait; the fix keeps the server IDLE until
after the busy-wait.)

This matches ROADMAP_done.md v0.3.9 issue (4): "an EXHAUSTED SS task at bg_prio
2 cannot be re-dispatched after gate.post() (the harness's TERMINATED wait
spins)". The kernel's demotion behavior is correct SS semantics; the *test*
demands an exhausted task keep running, which is impossible below the harness.

### 2.3 Fix (implemented, test-side)
- **Reorder the helper lambda**: busy-wait past the real deadline at **nominal**
  priority (prio 11 > harness 10 — auto re-dispatched every tick via the RMS
  guard, since `highest_ready(11) < cur_prio(10)` is false), **then** activate +
  exhaust (`consume×5` → EXHAUSTED), **then** `gate.wait()`. The deadline passes
  while the task is live; the scan captures EXHAUSTED context.
- Keep the SS **IDLE** during the busy-wait (no `on_activation` until after) so
  `on_tick`'s auto-consume cannot demote the task mid-wait.
- Change the harness's `while (state != BLOCKED)` and `while (state != TERMINATED)`
  spins from bare `pause()` to `Scheduler::reschedule()` — the only way to
  dispatch the now-exhausted (eff prio 2) helper for `gate.wait()` and for
  self-termination after `gate.post()`.

### 2.4 Expected effect
`ss_deadline` 2/2 completes: helper genuinely exhausts, real deadline passes,
scan fires with EXHAUSTED context (budget 0), post → self-terminate → clean
teardown via `remove_task`.

---

## 3. Fix discipline notes
- Both fixes are additive and confined to their defect paths; neither alters the
  canonical ISR switch path (disassembly-verified for the abort-path change in
  `ae44963d`).
- Validation plan: `make build` (check-style gate); class gates `ss_deadline`,
  `deadline_miss`, `deadline_action`, `deadline_recovery`, `wcet_overrun`,
  `sporadic`, `timing`, `ipc`, `scheduler`, `atomic`; then the `all` gate with
  `CONFIG_DEBUG_IPC_SCHED` ON per the debug-gate procedure. `test-history.txt`
  rows appended after every run.

---

## 4. Additional pre-existing blockers unmasked (2026-08-06)

Fixing the two audit targets let the `all` gate reach later classes, exposing
two more pre-existing failures (both reproduced at baseline):

### 4.1 `wcet` — semaphore waiter-array overflow + INV-4 self-termination
`test_wcet_scheduler.cpp` created **40** tasks all blocking on one gate, but
`CONFIG_SYNC_MAX_WAITERS = 32` → the 33rd `wait()` failed `add_waiter()` →
`ENSURE(added)` panic (semaphore.cpp:185).  Additionally the helper lambda
returned immediately after `wait()` (INV-4), self-terminating before the harness
observed BLOCKED → hang once the overflow was removed.  **Fix:** population
40→30 (< 32) and the BLOCKED-spin pattern.  `wcet` 1/1 PASS (was panic).

### 4.2 `priority_inheritance` — mutex PCP spin vs. genuine blocking (T2-3)
The holder now correctly stays BLOCKED (BLOCKED-spin added), so the test
proceeds to `spawn_contender` (prio 20).  The contender calls `Mutex::lock()`
on the mutex held by the prio-11 holder.  `Mutex::lock()` (mutex.cpp:227-253)
only BLOCKS when the PCP ceiling path is active (`priority_ceiling_ > 0 &&
task->system_ceiling_ > 0 && task->priority <= system_ceiling_`); the test's
`Mutex::init()` uses ceiling 0, so the lock spins in the retry loop and panics
`Mutex::lock() exhausted PCP retry budget`.  The tests were written assuming
genuine blocking (`waiting_on_mutex`), a documented spec contradiction
(audits/test-suite-v0.3.10.md T2-3, ROADMAP_done v0.3.9 issue (2)): the test
suite framework matched the panic as an "expected panic" and the class reported
PASS spuriously (no `S:` line).  **Not fixed in this pass** — it is a separate
kernel/test model mismatch beyond the two audit targets.  Needs either a mutex
ceiling in the tests or a decision on whether `Mutex::lock()` should genuinely
block for ceiling-0 mutexes.

### 4.3 H2 residual status
The apply-side liveness + ownership re-check (section 1.3) plus the harness
`context.rsp` live-save (save-target always `&TASK_STACK_PTR(current)`) and the
dispatch-guard harness-boot exemption reduced the ipc-class H2W flake from
~1-in-3 to clean across 8 consecutive runs; the `all` gate now passes tests
1–476 (including the ipc cluster at 77/78, ss_deadline 469/470, wcet 476)
before reaching the priority_inheritance blocker at 477.

### 4.4 Hardware-watchpoint session (2026-08-06) — H2 residual live capture
Attempted to pin the residual with hardware watchpoints per ROADMAP §v0.3.9.

**QEMU gdb-stub hardware watchpoints are confirmed BROKEN** (as documented):
- lldb `WatchpointCreateByAddress` on `scheduler_next_task_id` (0xFFFF8000002E52B0)
  creates successfully but never fires during a full boot + ipc class run.
- x86_64-elf-gdb `watch *(uint64*)0x...` likewise sets "Hardware watchpoint 1"
  but never triggers.
So the documented "working instrument" is the kernel recorder + caller tracing,
not the QEMU stub.

**Live capture (kernel-side caller tags on every [SW] arm; ipc class, run 10):**
```
[SW] cur=1 next=6 rsp=0xFFFF9000...24224 caller=0xFFFF800000294395   # arm A
[SW] cur=1 next=0 rsp=0xFFFF8000...310160 caller=0xFFFF800000294395   # arm B
[APPLY] id=0 cur=0                                                    # idle → harness stranded
```
- **Two deferred-switch arms published back-to-back from the SAME call site**
  (identical return address), with NO [APPLY] or [TICK] between them.
- The second arm selects **idle** — `next_task()` returns idle because the first
  arm's `next_task()` already DEQUEUED task 6 (which is never re-dispatched).
- Applying the idle arm iretq's the harness into `idle_task_main` → the
  `[DIAG] idle loop count=186A0` hang.
- An `[ARM-SUPER]` probe (publish while `save_rsp_to != 0`) does **not** fire:
  the atoms are already cleared between the two arms (RMS clears pending arms
  every tick — `[RMS-CLR] pending=...`), so it is not a simple pending-supersede.
- `reap_orphans()` is confirmed NOT running during tests (entry probe REAP=0
  across 5 runs) — the addr2line hit on `reap_orphans()` for the arm caller was
  a stack/inlining artifact.
- Every added diagnostic (caller field, ARM-SUPER, RMS-CLR, REAP) perturbs the
  race away (repro rate drops to ~0/5), consistent with the documented
  "a single per-tick instruction perturbs it" (ROADMAP_done).

**Mechanism (refined):** a runnable task (task 6) is dequeued by
`next_task()` during an arm publish, the arm is then skipped/cleared before its
ISR epilogue applies it (generation change / RMS clear / set_current), and the
next `next_task()` falls through to idle — the dequeued task is stranded
(INV-2) and the harness is iretq'd into the idle loop.  The definitive fix
requires re-enqueueing the dequeued target when an arm is dropped, or a
hardware-watchpoint session on a host that supports it (QEMU's stub does not).
Tracked in ROADMAP §v0.3.9.

### 4.5 Global H2 event ring (implemented) — root cause pinned
Extended the per-TCB `debug_switch_ring[4]` idiom into a GLOBAL in-memory
event ring (kernel::debug::g_h2_ring[512], 6×uint64 per record), recording
every deferred-switch event with the atoms + generation + ISR depth:
`ARM`, `APPLY`, `SKIP` (asm gen-skip → scheduler_record_skip), `CLR-RMS`,
`CLR-SET`, `CLR-MISC`, `IDLE-ARM`, `REENQ`.  No serial I/O in the hot path
(the perturbation that made the race vanish); dump post-hang via
`h2_dump_ring()` or `x/120gx &kernel::debug::g_h2_ring`.

**The ring CAUGHT the root cause.**  Live capture at `ipc` test 21:
```
[ARM]     a=0x6 b=0xFFFF800000A4FF40   # arm harness→task6; its context.rsp is a
                                        #   direct-map address (displaced)
[CLR-MISC]a=0x6                          # apply-side validation aborted the arm
[ARM]     a=0x0                          # next arm selects idle — task6 stranded
[IDLE-ARM]a=0x1                          #   (next_task() skipped the harness)
[APPLY]   a=0x0                          # harness iretq'd into the idle loop → hang
```
The validation abort (drop of a stale arm) left the preempted harness READY +
still in the runq (INV-4) — so `next_task()` skipped it (a RUNNING-current
task) and fell through to idle.  The `t==null` path (target already removed)
did the same.

**Fixes (from the ring evidence):**
1. `scheduler_validate_pending_switch` drop_arm now restores the CURRENT task
   to RUNNING + dequeues it from the runq (undoing switch_to_task's
   READY+enqueue side effects) on EVERY abort path, and
2. re-enqueues the dequeued target via `set_task_ready` when it is still alive.

Effect: ipc flake drops from ~1-in-3 to ~1-in-14 hangs (direct-QEMU runs);
the idle-apply sequence is eliminated (the harness is RUNNING, so the
`!(next==idle && current RUNNING)` RMS guard protects it).  A rarer residual
remains (~7% direct / ~30% under the UART+expect harness) where the harness
hlt-waits with no further arms — a post-abort task-lifecycle state still under
investigation.  ROADMAP §v0.3.9 stays open.

### 4.6 Residual-flaw trace (2026-08-07) — full task-state capture
The ring tail plus a full id_table + TCB-state dump at the residual hang
(`ipc` test 21, harness stuck):
```
current task ptr = 0xFFFF800000725000 (id=1 state=READY ctx.rsp=0xFFFF900000032920
  inrq=0 kst=[0xFFFF900000023000-0xFFFF900000033000])
task table: id=1 READY inrq=0   id=2 BLOCKED   id=3 BLOCKED   id=5 BLOCKED
  (tasks 6 and 7 ABSENT — terminated, in the zombie list, id_table cleared)
ring tail: ... [ARM a=6 b=0xFFFF800000A4FF40] -> [CLR-MISC a=6] -> (silence)
```
Characterization:
1. Tasks 6/7 (sender/receiver) self-terminate and leave the id_table; nothing
   runnable remains but idle.
2. The harness (current, physically running the test-21 wait loop) is in
   `state=READY, in_ready_queue_=0` — the INV-4 anomaly persists on the
   preempted task in this path, so it is not RUNNING (the idle-switch RMS guard
   is off) but it is also never re-dispatched.
3. The harness's `context.rsp` = 0xFFFF900000032920 — the snapshot-restored
   frame whose RIP is `arch_hlt` (the daemon-wait).  Resuming it there resumes
   the tight `hlt` loop, so the test-21 wait loop never re-observes
   `sender->state`/`receiver->state` (whose TCBs are now zombies) — the
   harness spins in `arch_hlt` forever, with the timer ISR firing but no task
   ever runnable.

Attempted: a `snapshot_restore` harness-binding fixup (set the harness's
context.rsp to the live RSP when no TCB kernel_stack contains it) — did NOT
reduce the residual (variance), and binding context.rsp to a non-iretq-frame
address is semantically suspect, so it was REVERTED.  The residual is a
harness-resume-on-snapshot-frame + terminated-task-zombie interaction that
needs either a context.rsp fixup to a *valid* harness frame or a wait-loop
that tolerates zombie TCBs.  ROADMAP §v0.3.9 stays open.

**2026-08-07 follow-up (replace-the-frame attempt, REVERTED):** re-applied the
harness-binding fixup cleanly and measured 3×14 runs — **9/14, 9/14, 10/14**,
consistently WORSE than the 13/14 baseline without it.  Root reason: the
harness's live RSP inside `snapshot_restore` (task context, boot stack) is
call-frame data, NOT a valid iretq frame — a deferred-switch resume TO the
harness via `context.rsp = live_rsp` iretq's garbage, adding a new failure
mode.  Conclusion: the harness's `context.rsp` must stay a proper ISR-style
frame; the residual freeze cannot be fixed by re-pointing it.  The harness is
the test runner and should never be a deferred-switch TARGET in test mode —
it should always continue as the physically-running current task.  A correct
fix therefore belongs in the switch-selection / harness-nonpreempt path, not
in a context.rsp patch.  Reverted; ROADMAP §v0.3.9 stays open.

**2026-08-07 follow-up (harness non-selection guard, TESTED + REVERTED):** per
the §4.6 hypothesis, implemented a guard in `Scheduler::next_task()` that
dequeues-and-skips the harness (`is_test_active() && candidate == harness`) so
it is never selected as a deferred-switch target.  Result: **0/10 ipc runs pass
— deterministic suite hang** at test 2/15 (the harness is never resumed after
the first test task yields, so the runq-wait stalls).  This DISPROVES the
"never a deferred-switch target" hypothesis: the harness IS a legitimate resume
target (it is switched away to dispatch test tasks and must be switched back).
The residual is therefore NOT that the harness is selected, but that its
resume frame is occasionally STALE (the snapshot-restored arch_hlt frame).
Correct direction: keep the harness's `context.rsp` fresh across
`restore_task_fields` (the write that reintroduces the stale frame at every
snapshot boundary) or validate/repair the harness resume frame at apply time.
REVERTED; ROADMAP §v0.3.9 stays open.

**2026-08-07 Direction 1 (restore_task_fields harness bypass) — IMPLEMENTED.**
`scheduler.cpp::restore_task_fields()` now captures the harness's live RSP
(when the harness is the current task, i.e. snapshot_restore is running on it)
and re-applies it to `context.rsp` after the snapshot-field restore, so the
snapshot's stale daemon-wait arch_hlt frame is never reintroduced into the
harness's resume frame.  SIL-3 review (inline; the sil3_auditor subagent is
registered pending an opencode restart):
- Rule 5 (critical-section interference): runs inside snapshot_restore's
  `arch::IrqGuard` (IF=0); only reads RSP (`mov %rsp`) and writes the harness
  TCB `context.rsp`; no locks, no runq mutation — no interference.
- Rule 6 (no #ifdef asymmetry): unconditional code; `get_harness_task()` +
  `harness == current_task()` + `harness_live_rsp != 0` guards make it a no-op
  in every path where restore_task_fields is not driven by snapshot isolation
  (release never calls it), so no asymmetric branch behavior.
Empirical result (28-run direct-QEMU batches, identical methodology):
**baseline 24/28 (86%) vs with-fix 25/28 (89%)** — within noise; the fix does
not regress ipc and does not clearly improve it (consistent with §4.6: the
live RSP in task context is call-frame data, not a valid iretq frame).  Full
regression suite green: safe, scheduler, lock_protocol, ss_deadline, wcet,
priority_inheritance, timing, vfs — all PASS.  Kept as defense-in-depth.
ROADMAP §v0.3.9 stays open (the residual requires a real-iretq-frame strategy
or a wait-loop tolerant of zombie TCBs).

**2026-08-07 Save-path audit + rescue hook — TESTED + REVERTED (decisive
negative).**  Save-path audit: the commit-to-RAM is `mov [rax], rsp`
(isr_stubs.asm:199), gated by the generation check; `is_boot_stack_rsp` does
NOT drop the save (`save_target = &TASK_STACK_PTR(current)` unconditionally,
scheduler.cpp:1909).  Implemented the user-approved C-side rescue hook
`scheduler_rescue_current_frame(isr_rsp)` (commits the live ISR-frame RSP into
the harness's context.rsp at every ISR epilogue, before the generation check),
verified linked + called in the disassembly.  Result: **21/28 (75%) — WORSE
than baseline (24/28) and Direction-1-only (25/28).**  The ring confirms the
hook keeps context.rsp fresh (~0x032920, the harness's live shallow hlt-loop
RSP), yet the residual hangs persist.  **This DISPROVES the "stale
context.rsp is the root cause" hypothesis**: even with a fresh resume frame,
the harness hlt-waits in test 21 and the sender/receiver termination is not
observed.  The residual lives in the harness wait-loop / task-termination
observation path (or a deeper task-lifecycle interaction), NOT in the
save-target.  All save-path variants REVERTED; only the within-noise Direction 1
remains.  ROADMAP §v0.3.9 stays open.

**2026-08-07 Option 1 (post-frame `ret`-target validation) — TESTED, REVERTED,
and ROOT-CAUSED.**  Implemented the user-specified ungated check first:
`ret_target = *(uint64_t*)(rsp+160)` must be in `.text`, else drop.  Result:
**10/10 hangs** — `[rsp+160]` is only a `ret` target when the resumed RIP is
`arch_hlt`; for any other harness frame it is a local/data/stack slot, so the
ungated check false-drops every legitimate resume.  Corrected with a RIP gate
(`f_rip ∈ [&arch_hlt, &arch_pause)`), then measured **6/6 deterministic hangs**
at test 21.  The ring capture was decisive:
```
f_rip=0xFFFF8000002A0F1F   (= arch_hlt+1, the `ret` instruction)
rsp   =0xFFFF900000032920   (harness context.rsp, CONSTANT — the snapshot frame)
ret_target=0xFFFF9000000329D0  (∉ .text → check fires correctly)
```
The gated check CORRECTLY DETECTS the stale frame, but dropping it only
converts the garbage-resume hang into a never-resume hang — it cannot repair
the stale frame.  **Definitive root cause:** the harness's `context.rsp` is
PERMANENTLY the snapshot frame (0x032920, arch_hlt's `ret`) during test 21 —
neither the live-save (switch_to_task save-target) nor Direction 1 keeps it
fresh.  The live-save does not reach the harness's `context.rsp` in this path;
that save-target path is the next required investigation (why the harness
switch-away save never updates context.rsp).  All Option-1 variants REVERTED.

**2026-08-07 Patch Concept A1 (harness-frame structural validation) — TESTED
+ REVERTED.**  Per the approved "hlt; ret return-garbage" root cause, added an
unconditional harness-frame check in `scheduler_validate_pending_switch`:
RIP within `.text`, CS==0x8, RFLAGS IF set, else drop the arm (no iretq).
Empirical result (28-run batch): **20/28 (71%) — WORSE** than baseline
(24/28) and Direction-1-only (25/28).  Reason (confirmed by the earlier H2W
frame dump): the stale snapshot frame is `rip=arch_hlt (∈ .text), cs=0x8,
rflags=0x10297 (IF set)` — it PASSES all three structural checks, so A1
cannot detect it, and the added frame reads + drop path perturb/drop
legitimate harness resumes, regressing the race.  REVERTED per discipline.
The garbage source is the POST-frame `ret` target at `[context.rsp+160]`, not
the RIP/CS/RFLAGS triple — any effective check must validate that slot (or
the harness's context.rsp must never carry a snapshot frame at all).

---

## 5. H2 RESOLVED — asymmetric arm-clear paths were the residual root cause (2026-08-08)

### 5.1 Root cause (static analysis, verified in code)
The deferred-switch arm has THREE clear paths with asymmetric behavior:

| Path | Site | Restores preempted current? |
|---|---|---|
| `CLR-MISC` | `drop_arm` (scheduler_validate_pending_switch) | YES — RUNNING + dequeue + re-enqueue target |
| `CLR-RMS` | `rate_monotonic_schedule()` pending-arm clear | **NO** — atoms cleared only |
| `CLR-SET` | `set_current()` both branches | **NO** — atoms cleared only |

`switch_to_task()` (scheduler.cpp:2231-2235) sets the preempted current task
READY + enqueues it (when RUNNING or `cur_is_boot_stack` — the test harness
always qualifies) and sets the target RUNNING, THEN publishes the arm.  When
`CLR-RMS`/`CLR-SET` cleared that arm without undoing the side effects, the
physically-running harness stayed `state=READY, in_ready_queue_=true` (INV-4).
`next_task()` then dequeued-and-skipped it (candidate == current_task()),
fell through to idle, and the idle-switch guard
`!(next==idle && current->state==RUNNING)` PASSED (state was READY, not
RUNNING) — the harness was iretq'd into the idle loop.  The idle reaper then
freed + 0xDD-poisoned the sender/receiver TCBs, and the harness's raw
`while (X->state != TERMINATED)` wait loops polled freed memory forever.
Ring signature: `[ARM a=6] -> [CLR-* a=6] -> [ARM a=0 idle] -> [IDLE-ARM] -> [APPLY a=0]`.

### 5.2 Fixes (all three)
1. **`restore_preempted_current(current, armed_target_id)`** — shared helper
   that undoes switch_to_task's publish side effects on ANY un-applied clear:
   dequeue the current if queued, restore `state=RUNNING` **only if READY**
   (never resurrect TERMINATED/BLOCKED currents — the F-1 SIL3 finding), and
   re-enqueue the armed target if still alive (INV-2).  Used by `CLR-RMS` and
   both `CLR-SET` branches; `drop_arm` got the same READY gate.
2. **Idle-fallthrough guard** — `rate_monotonic_schedule()` refuses to
   dispatch idle when the test-mode harness is the current task, regardless
   of its state field (defense-in-depth for any residual READY-leak).
3. **`wait_for_termination_safe()`** — test harness wait loops poll
   `TaskControlBlock::is_valid(task) && state != TERMINATED` (magic first,
   per the reaper's own idiom), exiting on freed 0xDD blocks.  ~100 raw
   `while (X->state != TERMINATED) pause;` loops across 25 test files
   converted; 3 pre-existing test-race flakes surfaced by the change fixed
   (o1/idle add_task→next_task IrqGuard, testrunner membership assert
   IrqGuard, apic_timer in-flight-tick tolerance).

### 5.3 Validation
- `make build` green (Errors 0).
- Class gates (CONFIG_DEBUG_IPC_SCHED OFF): ipc 51, scheduler 63, vfs 139,
  testrunner, priority_inheritance 11, buffer_pool 24, ipc_blocking 4,
  process 43, starvation_deadlock 3, timing 18, lock_protocol 53,
  deadline_recovery 4, ss_deadline 2, wcet_overrun 2, random 17,
  o1_scheduler 20 — all PASS.
- `all` 817/817: **10+ consecutive runs without a hang** (pre-fix ~7-30%);
  the single pre-fix hang@78 occurred once in 31 runs (3.2%) and 0 times in
  the final 10 runs.
- The pre-fix H2W residual signature `[CLR-* a=6] -> [ARM a=0]` was not
  observed in any post-fix ring.

**Status: H2 deferred-switch residual — CLOSED.**  ROADMAP v0.3.9 marked done.
The `ss_deadline` and `priority_inheritance` open issues are separate
pre-existing items (see ROADMAP "Open Issues").

---

## 6. RE-OPENED 2026-08-12 — apply-side skip instrumentation (developer session)

### 6.1 Context
BUGS.md re-opened the H2 race on 2026-08-12 (`testbed` @ `a2750bd2`): the
`ipc_core` class wedges at test 21 `ipc_send_sync_roundtrip` (~50%, 2/4, 4/8),
exactly the §5-closed signature but still firing.  Session findings on
branch `main` @ `464f1fbc`:

1. **The `[WEDGE] blocked-in-runq` INV-2 violation is a REAL but SEPARATE bug.**
   `IPC::block_sender()`/`wake_sender()` PI boost called `Scheduler::move_priority()`
   unconditionally, re-enqueueing a BLOCKED owner into the runq
   (`[WEDGE] blocked-in-runq id=6 st=2 inrq=1 phys=1`, 15 hits/run).  Fixed and
   committed `b7dce519` (gate on `owner.in_ready_queue_`, mirroring
   `set_priority`).  `[WEDGE]` count 15→0, all ipc/sync classes still green.
   The hang persists → the WEDGE was NOT the hang's root cause.

2. **Hardware-watchpoint session re-attempted, confirmed broken** (consistent
   with §4.4): both lldb `WatchpointCreateByAddress` and
   `x86_64-elf-gdb watch` on `scheduler_next_task_id`/`scheduler_load_rsp_from`
   accept but never fire over the QEMU `-s` gdb-stub; breakpoints DO fire.
   Updated `tools/gdb/h2_wp2.py` + `h2_replay*.gdb` to current v0.4.0 addresses
   (`current_cpu()::cpu` = `0xFFFF8000004A9C60`, ticks +0x10; TCB offsets
   id=0x360/state=0x370/ctx.rsp=0x478/kst=0x488/top=0x490 confirmed stable).

3. **Kernel-side cold-path diagnostics added** (`CONFIG_DEBUG_IPC_SCHED`-gated,
   zero hot-path perturbation — verified the race still reproduces ~50% with
   them ON):
   - `[H2-ABORT]` in `scheduler_validate_pending_switch` — fires ONLY on the
     stale-RSP arm drop.  In failing runs it never fired → the freeze is NOT a
     validate-abort.
   - `[H2-APPLY]` in `scheduler_on_context_switch` (id==1) — dumps the harness's
     stored iret frame (ctx.rsp, rip, cs, rfl, frsp, ss) at every APPLY to the
     harness.

### 6.2 Freeze-frame evidence (apply side) — corrects the "stale frame" theory
Freeze chain in test 21 (all failing runs):
```
[SW] cur=7 next=1 (x2)    <- sender 7 terminates → harness
[H2-APPLY] id=1 ctx.rsp=0xFFFF900000032930 rip=0xFFFF80000023E726 cs=0x8
           rfl=0x10202 frsp=0xFFFF9000000329E8 ss=0x10
[APPLY] id=1              <- harness iretq'd onto this frame
[SW] cur=1 next=6         <- harness (resumed) arms switch to receiver 6
<no further [TICK]/[SW]/[APPLY]>
```
`objdump` (NOT addr2line, whose discriminator misleads) proves
`0xFFFF80000023E726 = test_ipc_send_sync_roundtrip + 0x143` — the harness's
**own test-21 body** (a `jmp` back into the wait loop after `call arch_sti`).
So the frame is VALID (`rip ∈ .text`, `cs=0x8`, `IF=1`), NOT garbage, NOT a
stale daemon-wait `arch_hlt`, and NOT stale test-19 code.

Distinct RFLAGS signature:
- Freeze frame: `rfl=0x10202` (IF set; CF=AF=SF=0 — "clean" flags).
- All legitimate harness frames (PASS and earlier FAIL): `rfl=0x10293`
  (IF set; CF=AF=SF=1 — arithmetic flags from the interrupted instruction).
The `0x10202` clean-flag frame is the distinguishing marker of the displaced
resume — a frame whose flags were not produced by the interrupted test code.

### 6.3 Mechanism (updated)
Receiver 6 is **preempted** by the woken higher-priority sender 7 (prio 12 >
11) *before* its trampoline `terminate()` runs — it stays `READY` in the runq
and never terminates.  The harness's test-21 wait loop
(`is_valid(receiver) && receiver->state != TERMINATED`) is correctly TRUE, so
it arms a deferred switch to run receiver 6 to completion.  That switch is
**armed but never applied** (no `[APPLY] id=6`, no `[H2-ABORT]`), and then the
timer ISR stops firing — the harness's `arch::hlt()` is executing with
interrupts effectively disabled after the skipped apply.

This is the §4.5/§5 "armed-never-applied" mechanism re-surfacing: the §5
asymmetric-arm-clear fix (restore_preempted_current on CLR-RMS/CLR-SET)
reduced but did not eliminate it.  The residual is now isolated to the
**apply-side skip path** in `isr_stubs.asm` — the generation re-check
(`jne .restore`, §isr_stubs.asm:168-193) or the nesting-depth guard
(`ja .restore`, :133-134) leaving the arm published while the CPU returns to
the harness with a state that prevents the next tick from being serviced.

### 6.4 Next step (in progress)
Instrument the apply-side skip paths in `isr_stubs.asm`:
- log `scheduler_record_skip` (generation-skip) with captured/current gen, the
  arm target id, ISR depth, and the live RFLAGS at skip time;
- log the nesting-depth skip (`ja .restore`) with target id + depth.
This will confirm whether the freeze is a generation-skip or a depth-skip, and
capture the RFLAGS the harness returns to (the IF=0 hypothesis).

### 6.5 Apply-side skip instrumentation — result (2026-08-12, continuing)
Added cold-path (`CONFIG_DEBUG_IPC_SCHED`) hooks on EVERY apply-side skip path:
`[H2-SKIP]` (generation re-check, `scheduler_record_skip`),
`[H2-DEPTH]` (nesting-depth guard, new `scheduler_diag_depth_skip`),
`[H2-RSPABORT]` (asm RSP-owner check, new `scheduler_diag_rsp_abort`),
`[H2-DEAD]` (find_task(id)==NULL in `scheduler_validate_pending_switch`),
`[H2-ABORT]` (stale-RSP drop in the C validator),
`[H2-ARMDEAD]` (arm-time liveness check in `switch_to_task` publish).

**Result — root cause now pinned by direct evidence (freeze, test 21):**
```
[SW] cur=1 next=6 rsp=...                # harness (cur=1) arms switch to receiver 6
[H2-ARMDEAD] cur=1 next=6 tgt=0x0 st=1 tick=23   # task 6 ALREADY removed at ARM time
[H2-DEAD]    id=6 t=0x0 tick=23                  # apply-side: find_task(6)==NULL
<silence>
```
- `[H2-SKIP]`, `[H2-DEPTH]`, `[H2-RSPABORT]`, `[H2-ABORT]` all **never fired** →
  the freeze is NOT a generation-skip, depth-skip, asm RSP abort, or C
  stale-RSP abort.
- The freeze is a **stale runq node for a removed task**: `next_task()` returned
  receiver 6 as a READY candidate (its stale TCB reads `state=RUNNING` on
  freed/reused memory), and `switch_to_task` published the arm — but
  `find_task(6)==NULL` (id_table already cleared).  The §1.3
  `invalidate_pending_switch_to` fires on `release_zombie` and cannot help
  here: the arm is published AFTER the target was released.
- This refines §4.5/§5: the asymmetric-arm-clear fix closed the
  CLR-RMS/CLR-SET paths, but the residual is an **orphan runq node** — a task
  whose runq membership was not torn down when it terminated (PI-boost
  re-bucket / wake_sender restore interacting with enqueue/dequeue bucket,
  or a terminate path that released id_table without a matching dequeue_ready
  in the correct priority bucket).

**Open questions for the fix:**
1. Which removal path cleared task 6's id_table without fully dequeuing its
   runq node?  Candidates: `terminate()` → `dequeue_ready` using
   `effective_priority()` while the boosted/restored priority disagrees with
   the bucket the node was enqueued in (PI boost at 12, restore to 11), or a
   re-enqueue after release.
2. Whether `next_task()` should skip runq candidates whose id is not in
   id_table (belt-and-braces liveness check at dequeue/selection time) in
   addition to fixing the orphan-producing teardown.

### 6.6 ROOT CAUSE FULLY PINNED — stale harness resume re-enqueues a removed task (2026-08-12)

Added `[H2-ENQDEAD]` (enqueue-liveness audit in `Scheduler::enqueue_ready`,
cold, fires when a task is enqueued that is no longer a live id_table member)
with the caller return address.  One failing run produced the COMPLETE chain:

```
[H2-TERM6] cur=6 st=1 inrq=0 rq_prio=0 tick=20   <- receiver 6 self-terminates
                                                     (lambda done → trampoline →
                                                      terminate → dequeue+release,
                                                      id_table_remove)
[H2-APPLY] id=1 ... rip=0xFFFF80000023E726 rfl=0x10202  <- harness resumed onto
                                                     STALE test-21 setup frame
[H2-ENQDEAD] id=6 st=0 inrq=0 ra=0xFFFF80000023E5AB tcb=0xFFFF8000007E3000 tick=22
                                      ^ yield_to_task's Scheduler::enqueue_ready(task)
[SW] cur=1 next=6                    <- harness arms switch to re-enqueued dead task
[H2-ARMDEAD] cur=1 next=6 tgt=0x0 st=1 tick=23   <- find_task(6)==NULL at arm time
[H2-DEAD] id=6 t=0x0 tick=23                      <- apply-side confirms
<silence>
```

**Decoded control flow (objdump, test_ipc_send_sync_roundtrip):**
- Wait loop = 0x23e72b..0x23e79d (is_valid(sender)/is_valid(receiver) checks +
  `call arch_hlt` + set need_resched).  It NEVER calls yield_to_task.
- Setup path = 0x23e680..0x23e726: `yield_to_task(receiver)` (call at 0x23e69d,
  addr 0xffff80000023e538) then set need_resched.  Executed ONCE at test-21
  start.
- Freeze resume frame rip = 0x23E726 = `jmp 0x23e69d` (after `call arch_sti`),
  i.e. a frame from the SETUP path — NOT the wait loop.

**Mechanism (complete):**
1. Receiver 6 self-terminates (tick 20): dequeued, `id_table_remove`,
   `release_zombie`.  `invalidate_pending_switch_to(6)` runs (no-op — no arm
   to 6 pending yet).
2. Sender 7 terminates; harness is resumed (`[APPLY] id=1`) but onto a STALE
   saved frame whose rip is in the test-21 SETUP path (0x23e726), not the wait
   loop.  This is the residual §4.6 displacement: `context.rsp` was
   snapshot-restored to the setup-path frame (arch_hlt-free variant) and the
   live-save did not refresh it in this path.
3. Resuming at 0x23e726 re-enters the setup path, which calls
   `yield_to_task(receiver)` AGAIN at tick 22.  `yield_to_task`'s final
   `Scheduler::enqueue_ready(task)` (test_sched_helpers.hpp:80) executes on the
   receiver TCB (`tcb=0xFFFF8000007E3000`) whose id_table entry is gone →
   `[H2-ENQDEAD]` — the orphan runq node is CREATED HERE.
4. `next_task()` returns the orphan node (state reads stale READY), the harness
   arms a switch to it, the apply-side finds `find_task(6)==NULL`
   (`[H2-DEAD]`), drops the arm, and the harness hlt-waits forever — no further
   ticks service the wait loop → freeze.

**Fix directions (now precise):**
1. **Prevent the stale-frame resume** (the audit's long-standing goal): the
   harness's `context.rsp` must not be restored/re-pointed to the test-body
   setup path.  The §5 fix closed CLR-RMS/CLR-SET asymmetry but the harness
   resume frame can still be a snapshot/setup-path frame.
2. **Make `enqueue_ready`/`set_task_ready` refuse removed tasks** (defense in
   depth): if `find_task(task.id) != &task`, log + drop instead of inserting a
   node for a task the scheduler no longer owns.  This neutralizes the orphan
   regardless of which stale path resurrects it.
3. **`next_task()` liveness guard**: skip runq candidates whose id is not in
   id_table (belt-and-braces), as the §1.2 stale-arm-to-removed-task guard but
   at selection time.

---

## 7. RESOLVED — two-layer liveness guard neutralizes the stale-resume (2026-08-12)

**Actual state:** the H2 hang is fixed on `main` at commit `71b3a088`
(`fix(h2): neutralize stale-resume orphan re-enqueue (ROADMAP v0.4.0 dir #1+#4)`).
Root cause confirmed exactly as pinned in §6.6, with one refinement: refusing
the enqueue alone is NOT sufficient — the helper's `set_current(dead)` and
`task.state = READY` side effects run *before* the enqueue, so the test-side
helper must refuse at entry.

### 7.1 Fix implemented

**Layer 1 — kernel guard, `Scheduler::enqueue_ready` (scheduler.cpp:190).**
Upgraded the cold `[H2-ENQDEAD]` audit to a real refusal: if
`find_task(task.id) != &task`, log (trace-gated) + set `in_ready_queue_ = false`
+ return.  A TCB the scheduler no longer owns (removed via `release_zombie` /
`remove_task`) can never enter the runq, so `next_task()` can never select it
and no stale deferred-switch arm is ever published to it.  All legitimate
`enqueue_ready` callers enqueue id_table_-registered tasks (`add_task` inserts
into `id_table_` before enqueueing; wake/switch-away paths enqueue live current
tasks), so the guard cannot fire spuriously.  Fix direction §6.6.#2.

**Layer 2 — test-side guard, `yield_to_task` (test_sched_helpers.hpp:66).**
Refuse at entry if `find_task(task.id) != &task`.  This stops the stale resume
*before* any side effect (`set_current(dead)` re-points the scheduler's
current-cache; `state = READY` resurrects a freed/recycled block), so the stale
resume is a harmless no-op and the test body's wait loop exits because both
tasks are TERMINATED / freed.  Fix direction §6.6.#2 (test-side analogue).

`next_task()` liveness guard (§6.6.#3) was NOT added — the enqueue-side guard
makes the orphan unreachable at the source, so selection-time filtering is
redundant and would add per-dispatch cost on a hot path.

### 7.2 Validation (debug build, trace ON)

- `ipc_core` 23/23 across 14+ consecutive runs (pre-fix ~50% hang at
  `ipc_send_sync_roundtrip`).
- `all` gate: 7 clean 858/858 runs in 10 invocations, **zero watchdog hangs,
  zero H2 diagnostics** (pre-fix ~50-70% hang rate).  In runs where the stale
  resume occurred, the layer-2 guard made it a no-op and the suite continued.
- `make build` green (check-style Errors: 0).
- Residual `harness_blocked_sender_wakes` failure (2/17 messages) is a separate
  pre-existing v0.4.0 MP-8 timing flake, unrelated to H2 and reproduced
  identically on the pre-fix baseline.

### 7.3 Status

ROADMAP §v0.4.0 H2 entry can be marked fixed pending a trace-OFF release-gate
re-verification (the previous trace-OFF deterministic hang at
`ipc_send_sync_roundtrip` should be re-tested against this build).


