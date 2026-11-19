# DESIGN REVIEW 557-R2 — F4 TCB_CONFIGURE/TCB_SUSPEND/TCB_RESUME (N1–N3), REV 2

PLAN: audits/plan-557.md (rev 2)
BASE: main @ cc1a67046 (plan header claims cc1a67046049fce9b6aa06948c1d5a20b781ef53; `git rev-parse HEAD` this session returns cc1a67046049fce9b6aa06948c1d5a20b781ef53 — match)
R1: audits/design-review-557-r1.md (REJECTED: 1 S1 + 2 S2 + 4 S3)
NORMATIVE: unchanged from r1 (microkernel-transition.md §3.1 closed set + #80 feeder, §3.3 N1–N3, §16 Q4/Q9/Q11/Q12)
SCOPE: design only. No code exists; nothing committed (auditor wrote only this file).

## EVIDENCE SPOT-CHECKS (this session, files treated as truth, all at cc1a67046)

- scan_deadlines (scheduler_watchdog.cpp:93-135): all-task walk; TERMINATED skip at :109; period/deadline-zero, missed, future guards after. No admin_parked term exists — the r2-added skip is new code, placement after :109 as specified. CONFIRMED the r1-missed hole is real.
- scan_watchdogs_locked (:355-388): one-shot disarm-first, no state filter. watchdog_due (:70-74) = `wdog_armed && wdog_gen != 0 && now >= expiry` — a disarmed parked task can never fire via scan OR idle seam (idle_monitor.cpp:243 checks wdog_armed before routing to fire_stall_watchdog). Triple coverage (disarm + node removal + INV-ADM-16 skip) is redundant-by-design, not contradictory.
- disarm_target (cap/wdog.cpp:110-123): armed=false, expiry=0, ++gen (0→1 clamp) — r2's "paused != revoked observable" contrast (expires=E vs 0) is accurate. arm_watchdog saturating now+period (:89-91) ✓; kick refuses !armed at :140-141 ✓.
- set_task_ready anchors: :514 IrqGuard, :518 liveness, :520 READY, :536 mailbox_publish, :554-556 wake-while-current deferral, :568 remote direct enqueue, :572 enqueue_ready — every number in step 8 verified verbatim. Guard-at-:514 precedes all of them. The r1 S3-2 composition question is closed structurally.
- mailbox_drain (scheduler_smp.cpp:108-148): READY at :139, enqueue at :146 — rev-1 "balancer" label correction accepted (only READY write in the file, verified by grep).
- terminate wake sites at :135 and :250 exactly (grep) ✓; lifecycle carve-out code read (deliver_wait_status store/slot-clear, frame patch :494-500 area, waiting_child_pid=0 :502 area); join sibling task.cpp:156-157 ✓.
- parent_id: set at task.cpp:1745 (clone) / :2162 (create_user); cleared at scheduler_lifecycle.cpp:509 (waiter-collects path), :629 (reap orphan→init), task.cpp:2522 (remove_child), :2655 (cleanup unbind). All kernel-only; add_child (:2497) is called from creation + reaper paths only — no syscall writes parent_id, no user-mappable alias. Unforgeable as claimed; orphan/reap fails the creator branch closed automatically.
- DeathNotify::watch (death_notify.cpp:55-93): gate is actor==watched || actor==sup — a foreign task CAN still self-install as supervisor of any live target. r1 S2 quote confirmed live. r2 removes is_watching from the predicate entirely (INV-ADM-12 step 7 + doc line) — the hole is closed by excision, not by tightening the watch gate. TEST 14 negative leg anchors it.
- SUPERVISOR unforgeability: no CapType::Supervise (cap_types.hpp:37-57, 16 values, kCapTypeLast=SchedContext), no create_supervised, no grantee field anywhere in src/kernel (only unrelated debug-cap "grantee" in test_debug_syscall.cpp). Branch 6 is deny-all until M5/M12 and provably so. Copy/grant preserving (grantee_id, grantee_gen) fails closed for delegates ✓.
- mem-mgr layer (b) vacuity: transfer_all_to (untyped_handover.cpp:89) has ZERO callers — Untyped caps sit only in the root cspace. Root holder is a kernel task (branch 2 refuses first anyway). Layer (b) changes no behavior today; self-populates at M4 without a table to remember ✓. Walk: CONFIG_CSLOT_COUNT=64 (nexios_config.h:575), peek() lock-free/bounds-checked (cap.cpp:173-180) so the leaf-lock-then-walk order is safe (no double-take). Layers verified: (a) is_user_ field exists; (b) bounded walk; (c) static_assert(kPdRoleCount==13) at boot_sdf.hpp:119-120 ✓.
- dmesg SCHED catalog ends at +20 (:280-291); +21/+22 free ✓. Syscalls 97/98 live, MAX_SYSCALL=99 (:127-129) → 99/100/101 + MAX 102 collision-free ✓. TCB static_assert at task.hpp:1403; pool top 8192 (x86_64/riscv64) / 16384 (aarch64) at memory/mempool.hpp:34-39 ✓. core_task 52 / capability 161 rows (:300-306 area) ✓.

## R1-FINDING DISPOSITION

1. [S1] watchdog disarm → CLOSED. Suspend latches expiry/last_kick, clears armed only, bumps gen; resume re-arms shifted by elapsed with saturation, bumps gen, emits SCHED+21/+22. Arithmetic verified: remaining N=E−S resumes N=(E+(R−S))−R — phase preserved, neither a free full period nor a permanent revocation. Overdue leg (E≤S → already-due, fires next scan) is fail-safe and documented. Kick-after-resume (TEST 8) proves liveness; unarmed-resume leg proves no phantom arm. Paused-vs-revoked observable in procfs without format change (armed=0/expires=E vs 0) ✓.
2. [S1-new] scan_deadlines hole → CLOSED. Hole confirmed real (walk skips TERMINATED only); skip placed after :109; TEST 8 no-miss leg asserts it. No other all-task walker was found that needs the same skip (scan_watchdogs_locked is covered by disarm via watchdog_due).
3. [S2] authority → CLOSED. Exact rule INV-ADM-12 steps 1-7 in order, typed refusals, zero side effects. is_watching excised; creator branch unforgeable (anchors above); supervisor branch kernel-mint-only with (grantee_id, grantee_gen) binding; TEST 14 regression leg (self-installed watch → NO_AUTHORITY) directly pins the r1 exploit.
4. [S2] mem-mgr → CLOSED as enforceable. Three layers verified above; tripwire breaks the build if a role row is added; residual framed as owner choice O1/O2 (Q-r2-b), not a deferral. TEST 15 exercises both the Untyped leg and the pid-cell leg with teardown reset.
5. [S3-1] #80 feeder → CLOSED (step 18 gate item: comment must exist, URL cited in spec-sync commit).
6. [S3-2] guard precision → CLOSED (all line numbers verified verbatim; debugger/admin mutual exclusion enforced both directions + TEST 13).
7. [S3-3] #588 carve-out → CLOSED (INV-ADM-13 with anchors verified).
8. [S3-4] benign freezes → CLOSED (INV-ADM-14 + TEST 3/8 doc-block asserts).
9. Regression intact: stop-guarantee two-layer structure, latch set (6→7 fields with named fallback), closed-set numbering, scope cuts, join-resolve shape reuse, syscall/ABI/libc steps, doc set, cross-arch-by-construction, gate classes — all carried over. No new S1/S2 introduced: TcbControlAuthority POD (resolves the swappable-scalar smell), TCB-size tripwire + pack fallback, catalog slots free.

## NITS (non-blocking, planner may adopt or decline)

- N1 (S3): suspend/resume ++wdog_gen lacks the 0→1 clamp the scan paths carry (wdog_gen is uint32_t). Benign — the zero window occurs only while disarmed (kick refuses via !armed; seam takes report-only branch) and resume bumps 0→1 — but stating the clamp (or deliberately omitting it) in INV-ADM-11 would remove the asymmetry.
- N2 (S3): suspend bumps wdog_gen even for never-armed targets (0→1). Harmless per above; TEST 8's "gen unchanged after resume" leg covers the resume edge only, which is the correct pin.

## PATCH

No rejected_patch.diff: design review — no code exists to patch.

DECISION: APPROVED
