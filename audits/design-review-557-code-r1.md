# AUDIT REPORT 2026-11-19T2010Z — #557 CODE (F4 N1–N3, rev-2 plan)

PATCH: /tmp/pending_patch_557.diff (uncommitted production diff on main @ cc1a67046, ~2465 lines)
FILES: task/tcb_control_errors.hpp, task/tcb_control.{hpp,cpp}, cap/tcb_cap.{hpp,cpp}, task/scheduler_suspend.cpp, syscall/syscall_handlers_tcb.cpp, task.hpp, scheduler.hpp, registry/queue/tick/smp/watchdog/terminate/task/posix_time, cap.{hpp,cpp}, carved_object.hpp, syscall.hpp/errors/handlers_watchdog, libc syscall.h, gen/nexios_syscalls.{c,h}, global_state.{hpp,cpp}, dmesg_catalog.hpp, docs (transition/scheduler/cspace/death-notify/irqguard-ledger). No test files (verified via diff stat). test-history.txt rows + graphify-out/ churn excluded per brief.

## NORMATIVE BASELINE

- Plan audits/plan-557.md rev 2 (INV-ADM-1..16) + design verdicts -r1 (REJECTED) / -r2 (APPROVED, 2 S3 nits).
- #557 SIL 3 justification comment EXISTS pre-audit (issuecomment-7020729806, 2026-11-19T20:03:31Z: 3 numbers, cap-gated, deny-never-degrade, Q9). Spec-sync commit-message URL citation is a developer at-commit gate (nothing committed; auditor commits nothing).
- In-spec edits present and consistent: §3.2 T-1 rows 99/100/101 (table is §3.2, plan said §3.1 — trivial ref drift, content correct), §3.3 N1–N3 resolved, §3.4 KEEP 51 / New 4 / total 55 with 51+5+46=102 ✓. Plan's "54→57" was wrong arithmetic; the code's 55 is correct.
- Retrieval artifacts (check 7): plan lines 82–103 carry graphify queries 5–6 + vault searches with dispositions for the newly-touched areas; r1 artifacts cited. Satisfied.

## VERIFIED (evidence for the record)

- Orthogonal latch: 7 fields + AdminPrevState beside TaskState (no new enumerator), ctor init, zeroed at all 5 memset sites (create/create_user/clone/elf finalize/try_rerun), static_assert(sizeof <= kMemPoolMaxBlockSize) tripwire intact, +24B layout as documented (smalls borrow existing pad).
- 8 absorb guards before every READY write (registry set_task_ready, queue wake-while-current, tick x3 + monitor, smp mailbox drain, posix reapply_wakes) + 2 structural backstops (enqueue_ready/inner, owning refusal restoring BLOCKED). Counter folded into admin_absorb_wake single point — sound drift, removes the forgotten-counter class.
- Watchdog INV-ADM-11 exact: latch expiry (never zeroed), clear armed only, bump gen with 0→1 clamp (r2-N1 honored, stated in code + md); resume shifts expiry+last_kick saturating, re-arms only when latched!=0, bumps gen, emits SCHED+21/+22 (catalog + severity mirror verified). Paused (armed=0/expires=E) vs revoked (expires=0) observable. Arm refused typed on parked; kick path needs no change. scan_deadlines skip present (INV-ADM-16).
- Authority INV-ADM-12 in order with zero-side-effect refusals; is_watching excised; creator branch unforgeable (kernel-assigned parent_id); SUPERVISOR requires create_supervised scope + caller id/gen binding + target binding; NO syscall/compose producer exists (definitions only — deny-all fail-closed verified by grep).
- Mem-mgr INV-ADM-15 live: pid cell 0 + bounded Untyped walk under CNode leaf lock (single take, no scheduler lock held at gate — no order, ISR-safe by single-take) + double static_assert(kPdRoleCount==13) tripwire. Residual is owner choice O1/O2 per plan, predicate enforceable today.
- Debugger mutual exclusion both directions (suspend refuses debug_parked; debug_park_stop refuses admin_parked OR stop_requested — documented, sound). Deferred stop: null-current guarded, user-boundary + debugger-precedence preserved, RUNNING-resume race analyzed safe (park→switch-away atomic under scheduler_lock_; resume NOT_SUSPENDED before park).
- #588 carve-out: lifecycle/join wake paths byte-identical (no diff), anchors documented. N1 mask: 0/unknown→INVALID_ARGS, bit2/3→UNSUPPORTED before any application. Resume: deadline restore gated on latched!=0; clear-before-enqueue keeps backstop net exact; enqueue_ready takes no lock (no reentrant-take deadlock). Terminate (both funnels) + cleanup clear latch; TERMINATED/REAPED filtered at resolve + mechanism.
- Numbers/ABI: 99/100/101 + MAX 102, table sized [MAX_SYSCALL] with 3 appended rows, dispatch bounds symbolic, 1000-block face fresh (prior max 907), libc + regen + SYS_MAX 102 consistent. CapType::TCB pre-exists (F1). TcbCap tag-guard, ThreadCap-shape cascade, ResourceTracker cap_objects folding, no new tracker type.
- R2 nits honored: gen clamp stated (N1), never-armed bump stated + no-phantom-arm (N2).
- Checks 1/2/4/5/6/8/9/10: no heap alloc on park/resume/absorb paths (MemPool pool-alloc at cap mint only, tracked); RAII + release-outside-lock throughout; no test edits; no double-free (gen-checked); shared-C++ only, no arch/asm/trap change (parity by construction); all failures typed, fail closed; no new cross-domain channel.
- Evidence (check 11): test-history rows present — x86_64 core_task 52, core_sched 127, ipc 96, core_sync 103 (after one transient 102/1 → rerun green), selftest 157 green; capability 167/3 + proc_elf 134/1 + off-x86 140/3 pair match the F2 APPROVED audit's predicted failure list exactly (names + loader_cap_smoke exit-5 mapping in design-review-555-code-r1.md:31–32); counts 167/134/140/157 identical, no other deltas. F4 paths inert when latch unused (all guards short-circuit on parked=false; terminate remove() membership-guarded).

## FINDINGS

- [S3] docs/specs/scheduler.md INV-ADM-9 names `gs::lockpark_latched_task`; code symbol is `gs::lockpark_is_latched` (global_state.hpp:335, suspend.cpp:222)
  WHY: Doc/symbol name drift only; call site and 8-CPU bounded semantics verified correct.
- [S3] Terminate funnels + resume clear 4 latch words, leaving suspend_tick/deadline_ticks/prev_state stale until cleanup/slot-reuse; admin_park_locked never clears admin_stop_requested
  WHY: All readers gate on admin_parked==false (resume demands parked, absorb/debug/scan skip, tick consumer early-returns on non-RUNNING); next suspend overwrites every word — unreachable stale state, hygiene only.
- [S3] sys_tcb_configure applies priority before a failing affinity leg returns the 800-block denial (partial application across independent fields)
  WHY: Benign per-field independence, no privilege gain; consider validating all legs before applying.
- [S3] Plan §3.4 "certified 54→57" vs code's correct 55; §3.2 vs plan's "§3.1" label; core_sync transient 102/1 rerun-green; style Errors 0 developer-attested (auditor spot-checked added kernel lines ≤80 cols, 3× #pragma once, NOLINTs — checker not re-run, recorded limitation)
  WHY: Process notes only; no code action.

DECISION: APPROVED
