# IMPLEMENTATION SPEC — Issue #582 (scheduler strand)

**Status:** APPROVED for implementation (design audit r6).
**Base commit audited:** main @ 7381dde7b.
**Lane:** dev, branch main. Production kernel code only. Test files, test_expected_counts rows and isolation wiring are QE-owned (see R7).

**Audit trail (each plan revision and its verdict):**

| Plan rev | Audit report | Decision |
|---|---|---|
| rev 1 (base) | audits/design-review-582.md | REJECTED (4 required changes, 2xS1, 5xS2, 2xS3) |
| rev 2 | audits/design-review-582-r2.md | REJECTED (2xS1 open: park hook context/unlock bypass, dead-owner recovery; +3xS3) |
| rev 3 | audits/design-review-582-r3.md | REJECTED (1xS1 park liveness, +5xS3) |
| rev 4 | audits/design-review-582-r4.md | REJECTED (1xS2 latch coverage, +5xS3) |
| rev 5 | audits/design-review-582-r5.md | REJECTED (1xS2 service-table honesty/bound, +4xS3) |
| rev 6 | audits/design-review-582-r6.md | **APPROVED** (S3 items are build notes, no design change) |

This document is the single self-contained implementation spec: rev 1 base merged with rev 2-6 deltas.

---

## 0. PROBLEM STATEMENT (context for the implementer)

Two intertwined scheduler defects on main:

**Defect B (primary, convicted).** A task can be **suspended while holding scheduler_lock_**. The timer path needs that lock (rate_monotonic_schedule, scheduler_tick.cpp:1052), so the whole scheduler freezes: serial silence, live timer counter, no sweep/dispatch/rescue. Convicted with GDB on six wedges: scheduler_lock_.locked_=1 with a SchedulerLockGuard ctor return address as holder and no release record, holder stack inside try_direct_switch -> switch_to_task with no reschedule/park below it. Mechanism: switch_to_task publishes the deferred-switch arm **under arch::IrqGuard** (scheduler_switch.cpp:852-923) while try_direct_switch keeps its SchedulerLockGuard (:1131) alive **after** that IRQ-off window closes (:923). A tick or the M3 self-IPI (vector 0xE1) landing in that window reaches the ISR epilogue, sees the pending arm and applies it, suspending the lock holder. The apply path validates the target's liveness/frame ownership but never checks whether suspending the *interrupted* context is safe. That is the missing invariant (INV-APPLY-1).

**Defect A (secondary, signature proven).** A task observed **READY + in_ready_queue_==1 + runq_next_/runq_prev_ == nullptr** (sometimes holding a queued message it can never receive), last recorded op = ENQ with no unlink note: the ready-queue (flag, links) pair written non-atomically. With B fixed, the per-tick stranded sweep (scheduler_tick.cpp:543-620) rescues the shape and increments orphan_rescue_count_, which is A's oracle (must be 0).

**Sequencing is mandatory: B first, then A**, with a gate between (B turns A from a silent freeze into a counted rescue).

---

## 1. FINAL INVARIANTS (implement exactly these)

### INV-APPLY-1
An ISR epilogue may apply a deferred switch only when the interrupted context holds no scheduler_lock_ take:
scheduler_lock_depth_own() == 0 AND Scheduler::scheduler_lock_held() == false.
Both conjuncts are required: SchedulerLockGuard marks depth **after** lock() (scheduler_lock_guard.hpp:66-70) and clears it **before** unlock() (:81-90), so each conjunct alone leaves an unmarked-but-locked window. Fail direction: **defer** (never apply, never cancel).

### INV-APPLY-1a
INV-APPLY-1 is stated over **live** contexts. A context whose TCB is TERMINATED/REAPED cannot resume its critical section; that is unrecoverable state, not a suspension risk. There is **no lock-recovery mechanism**: recovery was removed by audit (r2/r3) because SpinLock::holder_ is a return address with no TCB identity (spinlock.hpp:86-95). Such state is caught fail-closed by the deadline escalations (INV-APPLY-2 / INV-PARK-2), never repaired.

### INV-APPLY-2 — deferral deadline (apply path)
Deferral is **non-destructive** (no writes to save_rsp_to, load_rsp_from, load_cr3_from, load_tls_from, next_task_id, generation, force_apply; never calls cancel_pending_switch_cpu / restore_preempted_current) and **tick-deadline bounded**. Actions: {None, Warn, RearmContinue, FailClosed}:
- now-since >= kApplyDeferWarnTicks (=1) -> **Warn**: one-shot [S582-APPLY-DEFER-STUCK] + census, arm retained.
- >= kApplyDeferFailClosedTicks (=8) AND the lock word still held -> **FailClosed** (single dump+panic, section 4.9).
- >= 8 AND lock free -> **RearmContinue**: reset since_tick, apply_defer_rearm_count++, SCHED+19 line, arm retained. (Not reachable in health: the RMS CLR path clears/republishes pending arms every tick with a free lock, scheduler_tick.cpp:1142-1162, which resets the window.)
- **No arm is ever cancelled because of a deferral streak.**

### INV-APPLY-3 — deny is absolute; force never overrides
- force_apply (terminate-driven, issue #221) is a **selector to attempt an apply**, never an override of INV-APPLY-1.
- On **deny** the arm AND the force bit are retained (deny-and-**rearm**); the next epilogue retries.
- Force is never an input to the deny decision; a force-armed deny can only be a live holder (the B window), for which retry is correct and bounded.
- Arch fact (grep-verified): force_apply is read **only** by riscv64 (syscall_entry.S:244-248); x86_64 and aarch64 never read it, so the rules below are whole-story on riscv and unreachable-but-compiled elsewhere.
- Arm-time detection (all arches, C): if switch_away_from_terminating(_discard) publishes while scheduler_lock_depth_own() != 0, that is **detection only** — term_hold_count++ + one-shot [S582-TERM-HOLD] (id, state, depth, tick, ra). No recovery, no fatal there.

### INV-PARK-2 — park deferral for a BLOCKED lock runner
- No context owning a scheduler_lock_ take may enter either park (it could never be dispatched: the tick cannot take the lock). ParkLockGuard users get ParkDenyReason::SchedLock.
- A BLOCKED runner that was **not** parked is latched (lockpark_latch, section 4). Class tag lockpark_latch_class is **diagnostic-only** (section 4.3).
- Service is **hook-at-release** for the SelfLock class in plain task context, and **tick-only** for everything else. The tick bound is single-holder-only; the aggregate case is covered by a **monotonic** covered_ticks counter that drives FailClosed (section 4.5).
- **No F1 window may survive as a silent freeze:** either the latch is serviced (park or tick dispatch), or covered_ticks reaches 8 and the system fails closed loudly.

### INV-RQ-1
in_ready_queue_, runq_next_, runq_prev_ and rq_priority_ are written **only** inside TaskQueue, each mutation being **one IRQ-atomic unit** (arch::IrqGuard), so no ISR can observe a half-linked node. Therefore at every ISR boundary, for every TCB:
in_ready_queue_ == physically linked in exactly one level queue,
with the single documented exception ready_deferred_ (current-task wake deferral; flag false, scheduler_queue.cpp:309-323).

### INV-RQ-2
Every enqueue refusal is **typed and reported**, and leaves the TCB byte-identical to before the call. A caller must never set in_ready_queue_/rq_priority_ itself.

### INV-RQ-3
orphan_rescue_count_, stuck_rescue_count_ and runq_link_flag_desync_count_ are the violation oracles; all must read **0** in a healthy run (#582 backstop gate) and are A's acceptance evidence.

### EDF sentence (scope boundary)
The ready queue and the EDF list are **two separate lists**, each mutated only through TaskQueue-equivalent typed API with its **own** op tags; INV-RQ-1 is claimed for the ready queue only, and **no cross-list invariant is claimed or needed**.

### Preserved (unchanged by this work)
INV-1, INV-2, INV-4, INV-4a (reschedule park), INV-4b (no lock while BLOCKED), INV-C1, the SwSlot single-writer table, no blocking in an ISR epilogue, no dynamic allocation on RT paths, and the try_direct_switch preconditions (scheduler_switch.cpp:1121-1130).

---

## 2. AFFECTED FILES

| File | Change |
|---|---|
| src/kernel/task/scheduler.hpp | static bool scheduler_lock_held(); scheduler_park_service_latch() declaration; diagnostics getters; INV docs above scheduler_lock_ (:1207) |
| src/kernel/task/scheduler_lock_guard.hpp | dtor + unlock() call the release-service hook **after** lock_.unlock() at depth 0 |
| src/kernel/core/global_state.cpp | **all C-side gate logic** inside scheduler_validate_pending_switch() (:768): deny/defer/abort/apply, force clears, apply-defer deadline, FailClosed dump+panic, reset helper |
| src/kernel/arch/x86_64/isr_stubs.asm | branch widening on the existing return value (:339-340) |
| src/kernel/arch/aarch64/vectors.S | branch widening at the validate call site (lines drifted — re-verify) |
| src/kernel/arch/riscv64/syscall_entry.S | branch widening (:328) -> defer target 8f ONLY; force peek (delete :247) |
| src/kernel/task/scheduler_tick.cpp | reschedule() tail latch arming; latch state machine; tick service point (held region, ahead of :1030); :165-fail counting; BLOCKED+lock-held diagnostic (SCHED+16); sweep/desync counters |
| src/kernel/task/scheduler_switch.cpp | arm-time detection (term_hold_count), note_arm_under_lock(), membership-API migration (:280,:282,:486,:701), INV docs |
| src/kernel/sync/park_lock.hpp + .cpp | ParkDenyReason::SchedLock + arm in deny_reason() |
| src/kernel/task/task_queue.hpp + .cpp | IRQ-atomic link+flag discipline; typed API; op-ring tags |
| src/kernel/task/ready_queue_manager.hpp + .cpp | typed enqueue result, no field writes outside TaskQueue, hint-miss bounded scan, validated restore |
| src/kernel/task/scheduler_queue.cpp | ready/EDF mutations through the API (:139,:297,:313,:316-319,:348-350,:357,:494-499; EDF :78-125) |
| src/kernel/task/scheduler_registry.cpp | :505,:511,:596-598,:618-620,:837-839 via API |
| src/kernel/task/scheduler_lifecycle.cpp | :438 via API |
| src/kernel/task/scheduler_smp.cpp | :143 via API |
| src/kernel/task/scheduler_snapshot_fields.cpp | :148-150 via validated API |
| src/kernel/task/scheduler_debug.cpp | permanent CONFIG_DEBUG runq op-ring + dump helper |
| src/kernel/log/dmesg_catalog.hpp | SCHED+16, +17, +18, +19 rows (collision-check vs +15) |
| docs/specs/scheduler.md, docs/specs/ipc.md section 4, prompts/CODING_STYLE.md section 11.7 | invariant rows + apply-gate contract |
| **QE-owned** | relay/latch/runq tests, expected counts, isolation resets, test-history.txt |

---

## 3. PART B — SUSPEND-WHILE-HOLDING (implementation steps)

### B.1 scheduler.hpp
Add in the public section: static bool scheduler_lock_held() { return scheduler_lock_.is_locked_for_debug() != 0; } (is_locked_for_debug is always compiled, spinlock.hpp:102-104); getters apply_defer_count / apply_defer_warn_count / apply_defer_rearm_count / apply_defer_failclosed_count / term_hold_count / arm_under_lock_count / force_denied_count; static void note_arm_under_lock(); free function void scheduler_park_service_latch() declared here (so scheduler_lock_guard.hpp can call it without an include cycle); doc block above scheduler_lock_ (:1207) carrying INV-APPLY-1/1a/2/3 verbatim.

### B.2 core/global_state.cpp — the gate (C-only logic)
Extend scheduler_validate_pending_switch() (:768). **Return ABI: 1 = apply, 0 = abort/clear (existing reasons only), 2 = defer (leave the arm untouched).** Decision order:
1. **Dead-owner/term-hold:** no action here (detection lives at arm time, B.3). Never attempt recovery.
2. **Lock hold (INV-APPLY-1):** if scheduler_lock_depth_own() != 0 OR Scheduler::scheduler_lock_held() -> run INV-APPLY-2 actions and **return 2** (defer). Never touches atoms. One-shot [S582-APPLY-DEFER] with reason id (1 = own depth, 2 = lock word), current task/state/inrq, depth, tick.
3. **Existing abort reasons unchanged:** target missing/invalid magic -> drop_arm(nullptr) + 0; RSP outside the target's live kernel stack / harness boot stack -> drop_arm(t) + 0. drop_arm keeps its current body (:783-820), **including the AP skip** (if cpu_index() != 0 return, :792-793 — APs leave the arm pending for their own next-tick CLR by design).
4. Otherwise return 1 (apply). On the apply return, **clear force_apply** (C-side clear #1 of B.5).
5. Deadline state: per-CPU apply_defer_since_tick[] (0 = none), apply_defer_warned[]; reset on apply/abort/rearm.
6. scheduler_apply_gate_reset_for_test() clears all of the above.
7. All reads __atomic_load_n(..., __ATOMIC_ACQUIRE); **no logging on the allow path**.

### B.3 scheduler_switch.cpp — arm-time detection (detection only)
In switch_away_from_terminating() and ..._discard() immediately before the publish block (~:1018) and before setting force_apply (:1095): if scheduler_lock_depth_own() != 0 -> term_hold_count++ + one-shot [S582-TERM-HOLD] (id, state, depth, tick, ra). **No recovery, no panic here.**
In try_direct_switch() after switch_to_task() returns and before the return: Scheduler::note_arm_under_lock() when scheduler_lock_depth_own() != 0 (makes the legal arm-under-lock window a counted, auditable fact). Extend the doc block (:1105-1119) with INV-APPLY-1 and the mark-after-take / clear-before-release rationale.

### B.4 Per-arch branch widening (mandatory; no new CALL — the branch changes)
All three sites today apply on **any** non-zero return, so a defer (2) would be mis-applied. Widen only the branch; **no new call, no new preserved register**:

| Arch | Site | Today | Required |
|---|---|---|---|
| x86_64 | isr_stubs.asm:339-340 | test rax,rax / jz .abort_switch | cmp eax,2 / je .restore **before** the existing test/jz. Defer target **.restore** — verified it leaves the arm pending (:497-499, 'do NOT clear scheduler_save_rsp_to') |
| aarch64 | vectors.S validate call + cbz w0, .Lswitch_abort | cbz w0,.Lswitch_abort | cmp w0,#2 / b.eq .restore then the existing cbz. Defer target **.restore** (~:379, arm pending). **Line numbers have drifted from earlier revisions — locate by symbol, re-verify at implementation** |
| riscv64 | syscall_entry.S:328 | beqz a0, abort_switch | li t1,2 / beq a0,t1,8f then the existing beqz. Defer target **8f (depth-dec) ONLY** — **not** gen_skip (t3/a0 are clobbered by the C call -> garbage scheduler_record_skip args) and **not** 7f (~:420 steals the pending arm's TLS slot into OFF_TP) |

Registers to preserve: x86 r11 is the per-CPU index and must survive every call block; rax holds the return and is reloaded later; rbx is not yet live at the branch. aa64 w0 holds the return; arm/slot registers are reloaded after. riscv t1 is freshly loaded at ~:334 after the C call, so clobbering it at :328 is safe; ra stays stashed as today.

### B.5 riscv64 force peek + all force clear paths
- syscall_entry.S:244-248: **peek only** — delete the sd zero at :247; keep beqz t1, check_depth / j apply.
- Clear sites (all C-side, co-located with every path that ends an arm; no new asm-side clear is added):
  1. **apply** -> cleared by scheduler_validate_pending_switch on its apply return (new C code, B.2.4).
  2. **validator abort** -> cancel_pending_switch_cpu() (scheduler_switch.cpp:102, existing).
  3. **RMS CLR** -> scheduler_tick.cpp:1159 (existing).
  4. **AP reset** -> scheduler_smp.cpp:54 (existing).
  5. **defer** -> force is **kept** (deny-and-rearm).
- Arm-gone at the epilogue (beqz t0, 7f): **no asm action** — an absent arm implies one of paths 1-4 cleared it, each of which also clears force.

### B.6 Escalation priority table (apply path, per deferral)
| Priority | Condition | Action |
|---|---|---|
| 1 | arm already invalid/cleared | existing reasons; no defer bookkeeping |
| 2 | lock held and now-since >= 8 | **FailClosed** (B.9) |
| 3 | lock held and >= 1 and not yet warned | **Warn** once (one-shot), retained |
| 4 | lock held, below thresholds | return 2 (defer), count |
| 5 | lock free and >= 8 | **RearmContinue** (reset window, SCHED+19, arm retained) |

---

## 4. PART B — PARK LATCH (INV-PARK-2) IMPLEMENTATION

### 4.1 Gate cleanup
**Delete bit 6 from resched_park_gate_base()/resched_park_gate_task()** (scheduler_tick.cpp:1255-1329). It is not evaluable at call time (lock-held at a task-context reschedule() is a T2b discipline case owned by the contract check + take-site ENSUREs) and could never be the sole failing bit. Bits 0-5 and 9-13 keep their meaning; this is a **QE-visible contract change** (no test may assert bit 6).

### 4.2 Arming site (pinned)
In Scheduler::reschedule(), **between the arm-scope close (scheduler_tick.cpp:1483) and the park-candidate check (:1487):**
  if (resched_park_candidate == nullptr && current != nullptr && current_task() == current && current->state == TaskState::BLOCKED) {
      cls = (scheduler_lock_depth_own() != 0) ? SelfLock : ExternalDenial;
      lockpark_latch_arm(*current, cls);   // {task_id, generation, cls, since_tick, covered_ticks}
  }
This covers **every** non-park of a BLOCKED runner (bit-2, bit-3, bit-5, nothing-to-arm, EDF arm, pre-candidate breaks). Arming is a **hint**: all decisions happen at the service points under the lock with {id, generation, state} re-validation.

### 4.3 Latch identity and classification
Per-CPU struct LockParkLatch { uint64_t task_id; uint64_t generation; uint8_t lockpark_latch_class; uint64_t since_tick; uint32_t covered_ticks; bool warned; }.
**lockpark_latch_class is DIAGNOSTIC-ONLY** (verbatim rule): no decision predicate — arm, service, clear, escalation or panic — reads it. Grep-able acceptance: the field appears only in the struct, the arming writer, and dump/formatter code; zero reads in resched_park_gate_*, scheduler_park_service_latch(), the escalation table, or the FailClosed path.

### 4.4 Service points
**S1 — release hook (SelfLock in plain task context):** ~SchedulerLockGuard() (:87-88) and SchedulerLockGuard::unlock() (:97-98) call scheduler_park_service_latch() **after lock_.unlock(), only when the decremented depth reached 0**. The hook never runs a park body unless all five discriminator terms hold:
(isr_nesting_own()==0 OR arch::in_syscall_trap_level()) AND arch::interrupts_enabled() AND arch::irq_guard_depth_own()==0 AND sync::spinlock_held_depth_own()==0 AND CONFIG_TICK_HZ != 0 AND not idle AND current AND state == BLOCKED AND id/generation match.
If any term fails: **latch retained (not cleared)**, lockpark_pending_count++, no sti/hlt.

**S2 — tick service point:** inside on_tick's existing lock-held region (scheduler_tick.cpp:165-816), **immediately before the :1030 rate_monotonic_schedule() call** (same lock scope => same-tick dispatch after the clear). Runs the 4.5 priority table.

**S2b — :165-fail instants:** when on_tick's scheduler_lock_.try_lock() (:165) fails, the held region is never entered, so count there: read the own-CPU latch with a **double-read stability check** (read {task_id, generation} twice with __ATOMIC_ACQUIRE; proceed only if both agree — 64-bit aligned loads are single-copy-atomic on all three arches, so agreement implies no concurrent rewrite). If armed -> covered_ticks++, update max, run the priority table (its clear/FailClosed actions are lock-free-safe).

### 4.5 Escalation priority table (both service sites, first match wins)
| # | Condition | Action |
|---|---|---|
| 1 | find_task(id) != task OR generation mismatch OR invalid magic | **silent clear**, lockpark_stale_cleared_count++, covered_ticks = 0. **Never Warn, never FailClosed** |
| 2 | state != TaskState::BLOCKED (woke) | **silent clear**, lockpark_woke_cleared_count++, covered_ticks = 0 |
| 3 | deliverable (hook: discriminator admits; tick: RMS will run after the clear) | deliver (park / RMS dispatch), clear, serviced_hook++ / serviced_tick++ (tick counts **before** delivery), covered_ticks = 0 |
| 4 | not delivered (RMS :1052 skip after the clear; or a :165-fail instant) | covered_ticks++, update _max; **rearm**: since_tick = now only — **covered_ticks preserved (never reset by rearm)** |
| 5 | covered_ticks >= kLockParkCoveredLimit (=8) | **FailClosed** (SCHED+17 + dump+panic, 4.9), escalated++ |
| 6 | now - since_tick >= kLockParkWarnTicks (=1) and warned == false | **Warn** once (SCHED+18), warned = true, latch retained |

Hook-side placement: rules 1 and 2 run **inside the adopted-lock context, after --depth; unlock(), before any Warn or hook-park**. (No hook fires for ExternalDenial.)
**Warn is one-shot latched per latch lifetime and is NOT reset by rearm** (storm bounded by the SCHED+19 rearm counter).
**FailClosed keys exclusively on monotonic covered_ticks** — rearm can never restart the clock (closes the rearm-livelock).
**Held/deliverability uses the FULL five-term discriminator state**, not scheduler_lock_ alone: an order-table lock held forever drives rules 4->5 (panic), never a rearm loop.

### 4.6 Per-context service table (honest, no overclaim)
| Context at release | SelfLock (runner owns scheduler_lock_) | ExternalDenial / NoCandidate (depth == 0) |
|---|---|---|
| Plain task, IF=1, no outer guard/lock | **S1 hook** — guaranteed to fire (the guarded scope must end), parks in place | **tick-only** (no hook exists — hooks live only on the guard) |
| ISR-adopted (tick:86,105,167,828,1054; smp:117; registry:389/453, :725) | S1 retained (isr_nesting != 0); **that ISR's own dispatch** | tick-only |
| Trap, IF=0 | S1 retained (IF==0); **next-tick-only**, bound from IF=1 restoration. The latch arms when candidate==null, i.e. no arm was published, so the epilogue has nothing to apply | tick-only |
| IrqGuard-nested (task) | S1 retained; next tick after IF=1 | tick-only **and the escalation clock is frozen** (4.7) |
| Order-table lock held (task) | S1 retained; next tick after release | tick-only; clock accrues normally |
The 'first tick after IF=1 / after release' bound is **single-holder-only**; chained holders are covered by the monotonic aggregate rule (4.5 rules 4-5) with lockpark_covered_ticks_max as the witness.

### 4.7 IrqGuard-self-hold freeze (named, with its detector)
A latched task holding an outer arch::IrqGuard with IF=0 **freezes the tick**, so neither since_tick nor covered_ticks can accrue and **latch escalation is provably unable to fire** in that window. Covering detection is explicitly delegated (recorded gating, r6):
- in-kernel witness = tick_watchdog::checkpoint()/note_tick — TSC-driven, reports at the **first checkpoint after IF is restored**; its policy is **log/latch only** (tick_watchdog.hpp:22-28, no panic arm wired);
- [STALL] HALT (tick.cpp:233) is **CONFIG_DEBUG_IPC_SCHED-gated** (macro OFF for the release gate) — do not assume a release halt;
- test-harness per-class timeouts (test-only).
Acceptability: the shape is abnormal by construction — the documented blocking pattern releases the guard **before** reschedule() (scheduler_watchdog.cpp:141-152); try_direct_switch refuses live guards (scheduler_switch.cpp:1123-1129); switch_away_from_terminating releases before publish. Window bound = the enclosing guard scope's own exit.

### 4.8 reschedule() diagnostics
DEBUG-only, after the existing contract check (scheduler_tick.cpp:1386-1400): if scheduler_lock_depth_own() != 0 and the current task is BLOCKED -> dmesg **SCHED+16** + Logger::warn + resched_lock_blocked_count. Log-only, no panic, no control-flow divergence between builds.

### 4.9 FailClosed dump (bounded) + panic
Single site, ISR-safe (panic() is cli+hlt, kernel.cpp:1870-1888). **<=15 unconditional lines + <=8 DEBUG-only:**
1 fatal line: tick, arm id, slock_depth, irq_depth, spl_depth, is_locked, holder() RA, IF, isr_nesting, streak/covered_ticks, force_apply;
<=8 task-census lines (kFailClosedCensusMax = 8, first 8 affine non-idle tasks: id/state/inrq/rq/eff);
1 lock-state line (depth + holder() RA — **release-safe**: scheduler_lock_depth[], SpinLock::holder(), is_locked_for_debug(), spinlock_held_depth_own() are always-on);
then panic("sched: scheduler_lock_ unreleased").
**DEBUG-only adds** spinlock_dump_held() (g_held is DEBUG-only) + H2/op-ring tails **bounded to 4 entries each**.

---

## 5. PART A — READY-QUEUE DISCIPLINE

### 5.1 A1 — permanent instrumentation (CONFIG_DEBUG, scheduler_debug.cpp)
Per-CPU op-ring, single-producer, relaxed index, no allocation. Record: {tag, task_id, flag_before, flag_after, next, prev, rq_prio, level, ra, tick}. Tags: LinkBegin, LinkCommit, LinkRefuseInvalid, LinkRefuseFlagged, PopCommit, PopRefuseEmpty, PopDropInvalidHead, UnlinkSplice, UnlinkNotInQueue, UnlinkInvalid, ResetLevel, SetRawLevel, RqmEnqueueRefuse, RqmEnqueueCommit, RqmPriorityCommit, RqmRemoveHintMiss, RqmRemoveForcedClear, RqmClearAll, RqmRestoreLevel, RqmRestoreDrop, SnapFieldWrite, RebuildLevel, OutOfBandDetach, EdfInsert, EdfRemove, EdfResort.
API: sched_runq_trace_dump(task_id, max=8), called on first orphan/desync detection in the tick sweep.

### 5.2 A2a — TaskQueue single discipline (task_queue.hpp/.cpp)
bool try_link(TaskControlBlock&) (false = invalid/flagged; TCB untouched), TaskControlBlock* pop_front(), bool try_unlink(TaskControlBlock&) (splice from the level that contains it), void detach_out_of_band(TaskControlBlock&) (flag+links as one unit for the documented no-walk sites), void reset_level(), bool restore_level(head, tail, count) (validates the whole chain; false => caller drops the level), bool contains(...).
**Every public mutation body is one arch::IrqGuard unit.** push_back's flag-then-link ordering is made unobservable by that guard.

### 5.3 A2a — ReadyQueueManager
RunqEnqueueResult enqueue(...) -> {Linked, AlreadyQueued, RefusedInvalid}; only Linked sets rq_priority_/flag, and only inside try_link; pop_and_detach/remove/clear_all/reset/restore_pod delegate; remove() keeps rq_priority_ as a hint but on a hint miss (flag set, level not containing) scans 0..CONFIG_PRIORITY_CEILING once ([[unlikely]], counted).

### 5.4 A2b — call-site migration (all direct link/flag writes)
Replace with the API at: ready_queue_manager.cpp:49,91,119,169,181,195,216; scheduler_queue.cpp:139,297,313,316-319,348-350,494-499 + EDF :78-125; scheduler_registry.cpp:505,511,596-598,618-620,837-839; scheduler_switch.cpp:280,282,486,701; scheduler_lifecycle.cpp:438; scheduler_smp.cpp:143; scheduler_tick.cpp:599; scheduler_snapshot_fields.cpp:148-150.

### 5.5 Fail-closed table for the new miss paths (none may leave READY+unqueued silently)
| Path | Repair / escalation | Counter |
|---|---|---|
| Hint-miss scan bound hit, node absent, flag set | clear flag + rq_priority_ (provably unlinked), one-shot ERROR + op-ring dump; task stays READY -> sweep re-enqueues it | runq_desync_forced_clear |
| restore_level validate-fail referencing a live READY task | drop level, then rebuild that level from all_tasks_ (detach_out_of_band + enqueue_ready), one-shot ERROR | restore_drop_rebuilt |
| Refused enqueue AlreadyQueued | no-op + DEBUG membership assert | rqm_enqueue_refused_already |
| Refused enqueue RefusedInvalid | TCB byte-identical; one-shot ERROR with caller ra; sweep rescues + counts | rqm_enqueue_refused_invalid, orphan_rescue_count_ |
| Force-under-hold | arm-time detector + deadline escalation (B.3/B.6) | term_hold_count, force_denied_count |
| Park-gate lock denial | latch + service (section 4) | see 4.5 |

---

## 6. APPENDIX TABLES

### 6.1 The 26-row holder-bounds list — ROW CONVENTION (r6)
Convention: **rows are grouped by function/module; each row names its site count and caller IF; exclusions are listed explicitly.** On main@7381dde7b there are **36 static SchedulerLockGuard constructions, 34 in release** (:86 and :105 are CONFIG_DEBUG_IPC_SCHED-gated), of which **7 adopt a preceding try_lock** (tick:86,105,167,1054; smp:117; registry:389,727; in release: 5). Grouping + exclusions reconcile this to the 26 audited rows.
**Exclusions:** scheduler_tick.cpp:86, :105 (DEBUG-gated diagnostics), scheduler_registry.cpp:354 (test helper). **Re-verify the recount against the then-current tree at implementation.**

| Row | Site(s) | Caller IF | Bound |
|---|---|---|---|
| 1 | switch.cpp:1131 try_direct_switch (1) | **IF=1** | next_task() + validate_switch + owner-resolution walk <= CONFIG_MAX_TASKS (128) => **sub-100 us** (longest legitimate holder) |
| 2 | switch.cpp:947 switch_away_from_terminating (1) | IF=0 | O(1) + next_task() + frame validation; **clock-frozen** |
| 3 | scheduler_queue.cpp:415,434,462 setters (3 sites / 1 row) | **IF=1** | O(1) + <=1 migrate_dispatch_queue / edf_resort |
| 4 | scheduler_debug.cpp:65,91 (2 / 1) | IF=0/held | bounded by CONFIG_MAX_TASKS; DEBUG-only |
| 5 | scheduler_tick.cpp:167 (on_tick body, adopted) | ISR, IF=0 | **clock-frozen**; O(1)+bounded scans |
| 6 | scheduler_tick.cpp:1054 (RMS adopted) | ISR, IF=0 | **clock-frozen**; O(1) + next_task |
| 7 | scheduler_tick.cpp:828 (tick tail guard) | ISR, IF=0 | **clock-frozen** |
| 8 | scheduler_smp.cpp:117 (AP tick, adopted) | ISR, IF=0 | **clock-frozen** |
| 9 | scheduler_registry.cpp:385-389 + live unlock() :453 (balancer) | ISR-driven, IF=0 (outer IrqGuard:383) | **clock-frozen**; latch retained-pending |
| 10 | scheduler_registry.cpp:725-728 | ISR/trap, IF=0 | **clock-frozen** |
| 11 | scheduler_registry.cpp:97,151,258 (create/admit) | boot/locked | bounded; pre-task-existence |
| 12 | scheduler_registry.cpp:492 | locked | O(1) |
| 13 | scheduler_registry.cpp:567,588,606,674 | locked | O(1)+bounded walk |
| 14 | scheduler_registry.cpp:801,850 | locked | O(1) |
| 15 | scheduler_terminate.cpp:54 | trap, IF=0 | frozen |
| 16 | scheduler_terminate.cpp:105 | trap, IF=0 | frozen |
| 17 | scheduler_terminate.cpp:239 | trap, IF=0 | frozen |
| 18 | scheduler_auth.cpp:62,98,152 (3 / 1) | trap/locked | O(1)+bounded |
| 19 | scheduler_watchdog.cpp:95 | ISR, IF=0 | frozen |
| 20 | scheduler_watchdog.cpp:399 | ISR, IF=0 | frozen |
| 21 | scheduler_lifecycle.cpp:816 | locked | O(1) |
| 22 | scheduler_snapshot_fields.cpp:43 | test/quiesce, IF=0 | frozen; not a runtime path |
| 23 | scheduler_switch.cpp:942-947 (ENSURE + take) | trap, IF=0 | frozen |
| 24 | scheduler_registry.cpp remaining adopt sites (grouped) | locked | O(1) |
| 25 | scheduler_queue.cpp remaining mutation sites (grouped) | locked | O(1) |
| 26 | id-table/lookup guards (grouped) | locked | O(1) |

**Aggregate conclusion (evidence):** 22 of the 26 rows run **IF=0**, where the escalation clock is provably frozen (no tick can fire); the IF=1 task-context rows are **#1 and #3**, each O(1)+O(CONFIG_MAX_TASKS), >=80x under the 8-tick (~8 ms at CONFIG_TICK_HZ 1000) deadline. Residual risk = chained holders covering 8 consecutive instants -> accepted, availability-only and fail-safe, witnessed by lockpark_covered_ticks_max.

### 6.2 The 30-writer / 4-bucket state writer table
| Bucket | Writers | Property |
|---|---|---|
| 1. Lock-free, READY-only | scheduler_queue.cpp:313 (**IF=1 reachable**); scheduler_registry.cpp:465 (**IF=0 — inside set_task_ready's function-local IrqGuard at :459**, still READY-only) | Can only invalidate a latch (rule 2), never create one |
| 2. BLOCKED under IrqGuard/primitive lock (INV-4b pattern) | sync/park_lock.cpp:107-109; ipc.cpp:1606; ipc/pager_registry.cpp:595; sync/queue.cpp:225,285,377,441; sync/notify.cpp:119,189; sync/mutex.cpp:282,328,382,426; sync/semaphore.cpp:217,273; sync/eventgroup.cpp:173,225; syscall/syscall_handlers_ipc.cpp:134,399; syscall_handlers_posix_time.cpp:255,576; syscall_handlers_thread.cpp:599; syscall_handlers_process.cpp:147; syscall_handlers_irq.cpp:239; vfs/devfs.cpp:284; driver/virtio_blk.cpp:348; driver/ahci.cpp:589; elf/elf_loader.cpp:1161 | IF=0 => cannot interleave with the tick; each releases **before** reschedule() |
| 3. scheduler_lock_-held | tick.cpp:402,463,502,532,1092; debug.cpp:78,103 (ENSUREs :63/:89); switch.cpp:338,485,700,822,838,842,1057,1060; terminate.cpp:127,245; watchdog.cpp:150,181,307,455; registry.cpp:500,528,575; lifecycle.cpp:664; smp.cpp:139 (IPI drain, IF=0) | frozen against the tick |
| 4. Boot / test only | taskdefs.cpp:658; admission_selftest.cpp:86 | pre-scheduler / harness-serialized |
scheduler_lock_depth is written **only** by SchedulerLockGuard ctors/dtor/unlock() (no recovery helper exists).

### 6.3 The 17-row A-funnel window table (all link/flag writers)
L = scheduler_lock_ held, IF0 = IRQs disabled, dash = neither.
| # | Writer | Mutation order | Window | Desync |
|---|---|---|---|---|
| 1 | TaskQueue::push_back (task_queue.cpp:9-27) | flag=true (17) -> links (18-25) | IF0 **and** task context with IRQs on (wake paths via enqueue_ready_inner -> rq_for().enqueue, scheduler_queue.cpp:357) | flag=1 unlinked; concurrent remove() early-returns on a stale rq_priority_ |
| 2 | ReadyQueueManager::enqueue (ready_queue_manager.cpp:18-50) | flag guard (24) -> push_back (46) -> bitmap (47) -> rq_priority_ (48) -> flag (49) | as #1 | **one** possible producer of the convicted shape (ISR pops the just-linked node between 46-48; outer call then sets flag/prio) — **non-discriminating**; GATE 2(ii) proves the discipline, not this path |
| 3 | TaskQueue::pop_front (29-74) | flag=false (59) -> head fix (60-68) -> links null (69-70) | IF0 / dash | flag=0 while linked => later double-link |
| 4 | TaskQueue::remove (76-111) | splice (88-105) -> flag=false (106) -> links null | IF0 | unlinked with flag=1 => enqueue refused |
| 5 | RQM::pop_and_detach (78-94) | pop -> rq_priority_=0 (84) -> flag=false (91) | IF0 | clears a flag a concurrent enqueue set |
| 6 | RQM::remove (104-120) | validity -> hint -> contains() walk -> splice -> forced writes (117-119) | IF0/L | forced clear kills a concurrent link; stale hint skips membership |
| 7 | RQM::clear_all (190-200) | pop loop + flag clears | L/IF0 | bypasses the tagged API |
| 8 | RQM::reset (202-221) | walk+flag clear -> queues_[i].reset() (links not nulled) | L | stale TCB links => remove() splices garbage |
| 9 | TaskQueue::reset/set_raw (hpp:51-65) | heads/tails/count only | L | raw, no discipline |
| 10 | RQM::restore_pod (147-188) | set_raw + two flag walks | L (snapshot only) | resurrects a captured desync; unvalidated chain |
| 11 | EDF writers edf_insert/remove/resort (scheduler_queue.cpp:78-125) | link-then-flag (98) / splice-then-flag (114) | IF0/L | same pattern on the **separate** EDF list (own tags, no cross-list claim) |
| 12 | migrate_dispatch_queue (133-145) | remove -> clear -> enqueue_ready | L | READY+unlinked between steps (sweep-visible) |
| 13 | enqueue_ready H2 refusal (297) | in_ready_queue_ = false **without unlinking** | dash/L | linked with flag=0 |
| 14 | enqueue_ready current deferral (309-323) | ready_deferred_=true, READY, remove+flag clear | dash | the single legitimate READY+unlinked shape |
| 15 | dequeue_ready (360-378) | edf_remove -> remove -> deferral clear -> invalidate arm | L/IF0 | flag write inside RQM (writer-type violation) |
| 16 | rebuild_ready_queue (478-504) | resets all levels -> per task clears flag/edf/links (494-499) -> enqueue | L | bulk uninstrumented writer |
| 17 | Detach/terminate clears: switch.cpp:280,282,486,701; lifecycle.cpp:438; registry.cpp:505,511,596-598,618-620,837-839; smp.cpp:143; snapshot_fields.cpp:148-150 | direct flag/links, sometimes with no unlink | L mostly, some dash | the 'unlink bypasses RQM::remove' funnel the TEMP op-ring convicted |
Completeness claim: after A2 every row is expressed through TaskQueue (rows 2,5,6,7,10,16,17 become call sites; 1,3,4 are IRQ-atomic; 11 keeps its own list under the same discipline; 13/14 keep documented semantics).

---

## 7. GATES (evidence bars — no argument-only fallback)

### GATE 1 — after Part B (before any A code)
1. **Control:** scratch relay repro (/Users/arnold/jarvis-repro-582, src/kernel/test/test_ipc.cpp:1272-1372, K=150, prio 27-30; never merge) on unmodified main wedges within <=2 rounds, 3/3 boots, logged.
2. **Fix:** same repro on the fixed tree, 20 rounds x 5 boots per arch — zero wedges, **and >=1 [S582-APPLY-DEFER] per boot** (proves the gate executes).
3. **Injection:** hold scheduler_lock_ across an arm, force an ISR apply -> save_rsp_to != 0, apply_defer_count() == 1, arm lands after release, streak resets.
4. **GDB:** scheduler_lock_depth[0] == 0 at >=1000 sampled ticks; breakpoint on the gate with a return-code distribution.
5. **Health counters:** lockpark_arm_count == 0, lockpark_covered_ticks_max == 0, lockpark_escalated_count == 0, term_hold_count == 0; predicate-table unit test over {None, Warn, RearmContinue, FailClosed} x {lock held, free} x {<1, =1, =8, >8}; ONE deliberate panic exercise in the GDB/fault harness (dump content asserted), never in a class run.
6. Classes: x86_64 debug core_sched, ipc, ipc_blocking, core_sync.

### GATE 2 — after A1, before A2
1. Op-ring captures the named interleaving for a real reproduction (RqmEnqueueCommit after PopCommit for the same id with no intervening unlink tag) **or**
2. deterministic injection of the try_link -> flag-commit window proving the **discipline** (READY + flag=1 + unlinked unreachable, or repaired within one tick and counted) **and**
3. bijection test over a deterministic enqueue/dequeue/move/rebuild/restore sequence with tick interleave **and**
4. zero-rescue: orphan_rescue_count_ == 0 AND stuck_rescue_count_ == 0 AND runq_link_flag_desync_count_ == 0 across the GATE 1 classes.
If neither (1) nor (2) can be produced, **A2 is not accepted** regardless of green classes.

### GATE 3 — full
all (x86_64 debug), release all, core_sched/ipc/core_sync/selftest per arch, 3-arch link, bench/bench_context_switch/bench_ipc_throughput slope gate (1.00 tick/hop), all counters 0 as in GATE 1.5 / GATE 2.4, style Errors 0, then the SIL 3 audit.

---

## 8. R7 — QE MERGE GATES (acceptance prerequisites, not dev scope)

| Artifact | Reset wiring | Expected-count rows |
|---|---|---|
| lockpark_latch[] (incl. lockpark_latch_class), covered_ticks_current/_max, serviced_hook, serviced_tick, warned, stale_cleared, woke_cleared, escalated, warn, rearm, pending | lockpark_reset_for_test() <- test_isolate.cpp | core_sched, ipc, ipc_blocking |
| apply_defer_*, term_hold_count, arm_under_lock_count, force_denied_count, resched_lock_blocked_count | scheduler_apply_gate_reset_for_test() | core_sched, ipc, core_sync |
| runq_link_flag_desync_count_, runq_desync_forced_clear, restore_drop_rebuilt, rqm_enqueue_refused_*, op-ring | runq_desync_reset_for_test() | core_sched |
| dmesg SCHED+16, +17, +18, +19 (registered, collision-checked vs +15) | ring clear as today | logging_debug, core_sched |
| bit 6 removal from resched_park_denied_why | — | premise/expected-count row update |
| New tests: holder_defers, window_ctor_dtor, force_denied_live, gate_lock_bit, repark_service_hook, covered_ticks_failclosed (injection), schedlock_denied, runq_link_flag_bijection, enqueue_refuse_preserves_fields, remove_hint_miss, sched_orphan_relay_gate | — | all named classes |
No merge without: resets wired, dmesg rows registered, expected counts updated, test-history.txt rows appended, and the SIL 3 audit report posted.

---

## 9. S3 IMPLEMENTATION NOTES (from r6 — build notes, not design changes)

1. **asm diff re-verify per arch.** x86 isr_stubs.asm jz:340 -> .restore:497; riscv beqz:328, gen_skip:389, abort_switch:406; **aarch64 gate lines have drifted** from the revisions used in review — locate the validate call and cbz by symbol/context, not by line number, and re-read the surrounding block before editing.
2. **:165-fail test-context note.** In task-context on_tick reentrancy (documented tick.cpp:94-100) the caller's own body holds the lock, so the ISR's :165 try_lock fails and the double-read rule will count a covered instant against a live latch. Harmless in production (task-context on_tick is harness/test only); R7 resets must cover SCHED+16..19 and the latch array.
3. **Detector gating record.** Production action for the IrqGuard freeze (4.7) is **log/latch only**: [STALL] HALT is CONFIG_DEBUG_IPC_SCHED-gated (OFF for release), tick_watchdog is log/latch with no panic arm, harness timeouts are test-only. Do **not** claim a release halt in docs or on the issue.
4. **Dump bounds.** FailClosed <=15 unconditional + <=8 DEBUG lines; release census from always-on holder()/depth/lock-word only; spinlock_dump_held() and ring tails are DEBUG-only (4 entries each).
5. **reschedule()-entry service stays removed.** It cannot serve one-shot callers and runs under the arm's own IrqGuard; the release hook and the tick service points subsume it. Do not re-add.
6. **Latch-array double-read rule.** At the :165-fail and any lock-free latch read, read {task_id, generation} twice with acquire semantics and proceed only on agreement; 64-bit alignment makes each load single-copy-atomic on x86_64/aarch64/riscv64, so agreement implies no concurrent rewrite. Never let a torn read drive a counter increment.

---

## 10. RE-VERIFY AT IMPLEMENTATION (record before coding)

1. **aarch64 line drift** — re-locate the validate call, the cbz, and the .restore/.Lswitch_abort labels by symbol.
2. **26-site recount** — recount SchedulerLockGuard constructions on the then-current tree (36 total / 34 release at 7381dde7b), refresh the grouping/exclusion list, confirm the #1/#3 IF=1 rows.
3. **:165 adopt-site count** — confirm the adopting try_lock sites (7 total / 5 release) and that the :165-fail branch is the correct lock-free counting point.
4. **dmesg collision check** — SCHED+15 is taken (scheduler_tick.cpp:1393); verify +16..+19 are free in dmesg_catalog.hpp before assigning.
5. Fresh GATE 1 control run on unmodified main (the repro must still wedge) before applying Part B.

---

## 11. SEQUENCING, ROLLBACK

**Commits (each self-contained, in order):** (1) Part B: gate logic + asm branch widening + latch + diagnostics + dmesg rows; (2) A1 op-ring; (3) A2a TaskQueue/RQM discipline; (4) A2b call-site migration; (5) docs. Gates between each as above.
**Rollback criteria:** a defer streak persisting with the lock free (B.6 rule 5 counter) => revert the gate branch widening; a bench slope regression > 2% => revert the lock-word read and keep the depth conjunct only (and re-audit); any new core_sched/ipc failure attributable to A2 => revert commits (3)-(4) alone, keep A1; any audit S1/S2 => revert the whole Part B commit (smallest self-contained unit).

---

## 12. REJECTED ALTERNATIVES (one line each)

- Dead-owner lock recovery (release the lock on a dying task's behalf) — unimplementable: SpinLock has no TCB identity and the recovery would sit behind the very blocking take it cannot pass (r2/r3).
- Adding TCB-owner identity to SpinLock — new hot-path field write + new invariant to serve an unreachable path.
- Force outranks the deny — reintroduces defect B on the exact path that must be gated (r1 S1).
- Retry-loop inside reschedule() — self-deadlock: the guard's destructor cannot run until reschedule() returns.
- Cancelling the arm on a deferral streak — destroys healthy arms (lock-free at sample time != stale) and never deschedules the runner (r3/r4).
- A third asm call site for the gate — reopens the per-arch clobber/ordering closures; the return-value branch widening suffices (r2 items 3-5 stay valid).
- Serialising all ready-queue ops under scheduler_lock_ — reschedule()'s peek is lock-free by design (T2b no-reentrant take); IRQ-atomicity is the correct primitive.
- Scanning for membership instead of the flag on every enqueue — O(n) on the dispatch hot path (WCET).
