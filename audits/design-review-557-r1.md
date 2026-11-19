# DESIGN REVIEW 557-R1 — F4 TCB_CONFIGURE/TCB_SUSPEND/TCB_RESUME (N1–N3)

PLAN: audits/plan-557.md
BASE: origin/main @ 4fc5ed779 (verified plan header; re-verified key anchors at this tree this session)
NORMATIVE: docs/specs/microkernel-transition.md §3.1 (closed set + #80 feeder), §3.3 N1–N3, §4.1–4.3, §6.1–6.2, §8.7, §8.8, §9.1–9.3, §13 (mk_ names), §14 M11, §15 F4, §16 Q4/Q9/Q11/Q12
SCOPE: design only. No code exists; nothing committed (auditor wrote only this file).

## EVIDENCE SPOT-CHECKS (this session, files treated as truth)

- READY-writer sweep (`src/kernel/task`: `= TaskState::READY`): ~14 direct write sites — switch 824-826/840-841 (current leaving CPU), 1059-1062 (terminating exit), restore 338/349; queue 340 (wake-while-current); tick 402 (monitor), 463/502/532 (level-triggered re-apply loops), 627 (rescue sweep), 675 (IPC sweep, via set_task_ready); registry 520/555 (set_task_ready), 585/632 (idle init), 709 (creation); debug 104 (debugger_resume); smp 139 (balancer). IPC dir: ZERO direct READY writes — all IPC wakes funnel through set_task_ready. WAITING never assigned (only read, tick 261) — BUSY-refusal vacuous-but-harmless. procfs 330: read-only display.
- Backstop routing: every runq link goes through enqueue_ready/inner (queue 290-388; EDF branch inside inner 374-381). One exception: registry 567-569 remote direct `rq_for.enqueue` — sits BELOW the 520 entry guard, so covered only if the guard early-returns (see S3-2).
- Dispatch-path writers (switch 825/841/1060) unreachable for parked targets by construction (parked ⇒ BLOCKED ⇒ never current; terminate clears latch first). debugger_resume (debug 104) bypasses guards but is netted by the enqueue backstop. Monitor/creation/idle writers target unsuspendable tasks.
- Watch authority: DeathNotify::watch gate (death_notify.cpp:55-62) lets actor==watched nominate ANY live task as supervisor and actor==sup install on ANY live watched — self-grantable, no capability, no SDF.
- Watchdog: disarm = wdog_armed=false + gen bump (watchdog.cpp:116-120, scheduler_watchdog.cpp:371-374); no in-tree re-arm-on-resume precedent; armed state is per-TCB (task.hpp:830-861).
- Roles: PdRole has 13 values, no supervisor/mem-mgr member (boot_sdf.hpp:75-120). Syscalls 97/98 live, MAX_SYSCALL=99 (syscall.hpp:127-129) — 99/100/101 sequential, no reuse.

## WHAT PASSES

- S1 stop-guarantee structure: no-new-TaskState + 6 transient fields ⇒ procfs/dump/liveness/read-predicates untouched (verified read-only/unassigned above). 8-guard + owning-refusal + must-read-0 counters + DEBUG assert is a sound two-layer design; EDF/deadline/sleep/IPC-wait linkage analyzed airtight for dispatch (EDF/deadline lists are not dispatch vectors for BLOCKED tasks; suspend removes the deadline node; waiter lists preserved per INV-ADM-4 with absorb on every wake funnel).
- S2 scope cuts UNSUPPORTED (bit2/bit3) = INV-MK-9-faithful fail-closed; reserved bits, no silent ignore. No F5/#50 smuggling (carved_object doc-only, no VSPACE_MAP); no #590/#589 fixes (anchor-only). #582-compat trap correctly identified as name-only; BUSY-on-armed-latch + halted-in-park release is the correct composition.
- S3: shared-C++ only, no arch/asm/trap changes ⇒ parity by construction (INV-MK-5 untouched). Check-7 artifacts present with dispositions (4 graphify queries + vault searches, gap recorded not skipped). nexios_config.h untouched. Test sanctity respected; QE sketch concrete (15 tests, arch placement, teardown-or-terminate rule, count rows, off-x86 capability explicit-run flagged). Gate classes include M11 (capability + proc_elf) plus the wider chain. No new O(n) RT work; lock envelope follows debugger precedent.

## FINDINGS

- [S1] planned scheduler_suspend.cpp + TEST 8 — resume leaves the per-task watchdog permanently disarmed with no re-arm path and no owner notification.
  WHY: Suspend does wdog_armed=false + gen bump (destructive, cf. watchdog.cpp:116-120); resume never re-arms and TEST 8 pins "wdog still disarmed" as the EXPECTED end-state, so a resumed PD runs indefinitely without its WdogCap hang supervision and the wdog owner cannot distinguish "supervisor-stopped" from "supervision revoked" — the named S1 risk is normalized by test, not closed.
- [S2] planned tcb_control_authorize — authority (creator-parent OR DeathNotify::is_watching) is not Q9-faithful: no supervisor-cap gate, no SDF supervised-set check, no client/server subtree distinction.
  WHY: Watch slots are self-grantable by any live task (death_notify.cpp:55-62: watched self-nominates any supervisor; any task self-installs as watcher of any watched), so ANY task can watch then suspend ANY task; Q9's "cross-subtree attempts fail closed" and "supervisor-cap-gated" requirements are unenforced — the gate admits a strictly larger authority set than the spec allows.
- [S2] planned tcb_control_role_protected — the Q9 "never mem-mgr" exclusion is shipped unenforceable (predicate false for all 13 current roles; Q-q1 left to owner).
  WHY: Q9 hardcodes the exclusion, but with no mem-mgr/supervisor identity in PdRole (boot_sdf.hpp:75-120) the design's only tripwire is static_assert(kPdRoleCount==13); a future user-space mem-mgr PD is suspendable until someone populates the predicate — risk named-but-not-closed, deferred past the N2/N3 landing.
- [S3] closed-set shape (99/100/101, MAX 102, no reuse) is correct, but the §3.1 SIL 3 justification feeder (#80) is a planned issue comment + spec sync, not yet evidence — must land in the same change (#585 precedent).
  WHY: Process item only; design shape already conforms.
- [S3] guard precision: the set_task_ready entry guard must early-RETURN before mailbox_publish (registry 536-538) and the remote direct rq enqueue (567-569), or a parked task is cross-CPU resurrected; debugger_resume (debug 104) is netted ONLY by the backstop — state both compositions explicitly in scheduler.md.
  WHY: Correctness-preserving precision on an otherwise sound design.
- [S3] document the #588 exception: deliver_wake path writes the stopped parent's user status slot + clears waiting_child_pid while the wake is absorbed (lifecycle 445-502); correct on resume but it is a "stopped task's memory is still written" carve-out to the stop guarantee — must be stated in scheduler.md alongside the absorb rule.
  WHY: Contract completeness, no behavior change.
- [S3] state the unstated benign freezes: sporadic-server budget untouched (no consumption while BLOCKED, replenishment bounded — benign but unstated); remaining_ticks frozen across suspension is consistent with the deadline shift (both frozen, phase preserved) — assert both in TEST 8's doc block.
  WHY: Closes the "no free budget" argument fully.

## PATCH

No rejected_patch.diff: design review — corrections travel as plan edits by the planner, not as a code patch.

DECISION: REJECTED
