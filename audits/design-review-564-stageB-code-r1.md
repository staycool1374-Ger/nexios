# AUDIT REPORT 2026-11-20-564-stageB-code-r1 (SIL 3 code)
PATCH: audits/pending_patch.diff (~1531 lines, worktree uncommitted diff on main)
BASE: main tip de0a4dc3b verified (`git log --oneline -1` = `de0a4dc3b #564 F11 Stage A`)
NORMATIVE: audits/plan-564-stageB.md rev 1 + audits/design-review-564-stageB-r1.md (APPROVED, 2 S3s) + parent plan-564.md rev 3 + Stage A code r1 APPROVED
FILES: src/kernel/task/task.cpp, src/kernel/proc/proc_tcb_caps.cpp, src/kernel/proc/proc_tcb_caps.hpp, src/kernel/proc/proc_mgr_proxy.cpp, src/kernel/proc/proc_mgr_proxy.hpp, src/kernel/proc/proc_svc.cpp, src/kernel/proc/proc_svc.hpp, src/kernel/task/taskdefs.cpp, src/kernel/task/boot_sdf.cpp, src/kernel/log/dmesg_catalog.hpp, userspace/proc-mgr.c (new), userspace/proc-mgr-parity.c (new), docs/specs/microkernel-transition.md, audits/plan-564-stageB.md (new). NOT touched: src/kernel/test/**, test_expected_counts, src/kernel/syscall/*, src/kernel/nexios_config.h (knob stays 0), scheduler_terminate.cpp, tcb_control.cpp.

## FINDINGS
- [S3] src/kernel/proc/proc_svc.cpp fork() — holder is a raw find_task pointer with no pin across hold()/add_task_err
  WHY: PD death in the verdict-to-hold window leaves a dangling holder; identical to the in-tree find_task+use precedent (kill path), fail direction is destroy-child closed, but a re-resolve or magic re-check before use would harden it.
- [S3] src/kernel/task/boot_sdf.cpp VFS_SRV row — `0 *` guard changes OFF preprocessor text for that row
  WHY: "OFF byte-identical" is overstated at the token level; behavior-identical holds because compose is per-enabled-row (VFS_SRV disabled rows never compose, cap_sdf seam links helpers not sdf_row), so no OFF behavior change.
- [S3] src/kernel/proc/proc_mgr_proxy.cpp proc_mgr_verdict — every non-zero PD status maps to STOP_REFUSED
  WHY: Naming conflates refusal classes but the mapping is fail-closed and documented in the hpp contract; no authority widened.
- [S3] ON-world proc_elf 134/1 (signal_kill_delivers) vs OFF green — QE parity class must update the test, not the kernel
  WHY: Fork verdicts pass in the same ON run, no +21 pushed, OFF proc_elf 135/0 green: consistent with the documented transient-BLOCKED-during-verdict-RPC timing assumption (§8.8.5 rule added in this diff); exoneration evidence is coherent, tracked as QE work.

## CHECK DISPOSITION (task items 1-9)
1. Generation fix: single `next_task_generation()` allocator (task.cpp:458), init_task_common same stream (:463), clone assigns fresh pre-publication (:1833). No gen==0-as-unset consumer of TCB.generation exists (only wdog_gen/timerfd-slot/buffer-pool sentinels, never TCB.generation). spawn_thread gen-0 honestly recorded as S3 follow-up in the comment (:455) and plan step 1. CLOSED.
2. Hold redesign: claim predicate `!occupied && !reserved && !reclaim_pending` (design S3 #1 closed in code); rollback_claim on all five mint-window failures with counter balance only when this call cleared; publish re-check under lock, stale path removes installed slot + STALE_PID, counter untouched (funnel already balanced). Funnel index-only, zero CNode* capture, no ScopedRef (release_all_objects refcount==1 contract intact). Target-death parks reclaim_pending reaped by drain_reclaim at hold()/release() top only; lookup read-only (no drain call). Funnel takes only the table leaf under caller IrqGuard with scheduler_lock_ NOT held (scheduler_terminate.cpp:79-85), idempotent, no logging. CLOSED.
3. Verdict legs: knob-ON per-op in caller context; fork pre-publication hold + CAP_TABLE_FULL destroy; admission-failure release-then-destroy; waitpid single verdict before loop, #590/#593 legs statement-identical (insertion-only hunks); kill verdict before disposition, fatal/handler classification + #593 legs untouched, deny-only per INV-PROC-11 with Q-B2 deviation from parent plan honestly documented in code + plan + §8.8; sigreturn/getpid no-RPC splits documented; OFF build = Stage A text (all legs #if-guarded, proxy TU empty when 0). CLOSED.
4. PD: SS(1,10,0) row fields (4,10,0,1,10,0) match procfs-srv shape, Q-B1 honored not literal 1/1; R-INPUT rsvd/rsvd3 → -22 kernel-side (handle_decision) and PD-side; replies echo owner_id/owner_gen via ipc_send(req.owner_id) never PID 1 (#441); STOP/RESUME -1, unknown -22, infinite server loop never exits. Parity probe uses sys_* inlines with libc-kill/signal-undefined honestly recorded in its header comment (D4). CLOSED.
5. taskdefs row appended AFTER procfs-srv row (PIDs stable), SDF zero-cap row pre-exists in tree (Stage A, untouched), MOVE legs gated behind knob default 0 so nothing takes effect — §8.10 literal ordering satisfied by step order + §8.8.5 text; prio 4/period 10/SS(1,10,0)/sdf_role 13 verified at taskdefs.cpp:472-475; restart hook sdf_rebind-first mirrors procfs-srv; knob NOT flipped (nexios_config.h untouched, default 0 confirmed at :1686-1688) with parity-green gating stated. CLOSED.
6. boot_sdf: nesting fix minimal (close kernel::sdf before gated #endif + reopen in the gated compose region); kVfsRegionPages guard fixes PROC_MGR-only builds; OFF behavior unchanged (see S3 on token-level wording). CLOSED.
7. dmesg +21: TASK catalog +10/+17/+18/+19/+20/+21, no collision (+1/+7/+9 intentionally uncatalogued via TaskError table per catalog comment); docs §15 new→in-progress (never closed), App A Stage B entry, §8.10 ordering + §8.8.5 transient-BLOCKED rule (no spin-on-BLOCKED, no spin above PD priority) all present. CLOSED.
8. Style Errors 0 is developer testimony (auditor ran nothing, per protocol); gate evidence exists as test-history.txt rows: OFF 8-class set (selftest 157, proc_elf 135, core_task 66, capability 200, cap_tcb 6, cap_vspace 13, ipc 106, servers 74) + release selftest 87 + ON selftest 157 exploratory + ON proc_elf 135/0 green and 134/1 signal_kill_delivers (see S3). Counts match base-era magnitudes. Off-x86 blocked by D1 test_hal arch_pause (QE commit f6f155e8b, test file read-only, pre-existing IPC-side blocker, #596 covers) — not a Stage B defect. LIMITATION recorded, no S-level attached.
9. Stage C untouched: fork stays kernel-composed clone, no carve surfaces, no §4.3b amendment; no new syscall numbers (syscall tables untouched, hooks live in ProcSvc bodies); no arch branches; Q2(a) 32-cap fail-closed stands (overflow → CAP_TABLE_FULL + pre-publication destroy); snapshot_reset clears new flags but stays unwired in test_isolate.cpp (QE lane, test sanctity respected). CLOSED.

## CHECKS 1-11 (PROMPT-audit)
1. Dynamic allocations: none — fixed 32-slot table, stack-array drains, no new heap in paths. PASS.
2. Concurrency: RAII SpinLockGuards, sequential leaf takes, no lock across RPC, no CNode* across unlock. PASS.
3. Assertion masking: no test files touched. PASS.
4. Memory safety: gen-checked idempotent remove_if, rollback on every mint failure, no double-free. PASS.
5. Critical sections: funnel adds only the table leaf under existing IrqGuard contract. PASS.
6. Preprocessor: ON/OFF semantics verified per leg; OFF = Stage A text modulo comments + catalog row. PASS (S3 wording note only).
7. Context-retrieval artifacts: plan-564-stageB.md carries 3 query_graph + 2 shortest_path + vault searches with dispositions (design-r1 verified anchors in tree). PASS.
8. Arch parity: no arch-gated code, no new numbers, 48 B wire assert intact on both sides. PASS (off-x86 blocked pre-existing D1, recorded).
9. Fail-closed: verdict-deny, hold-overflow, admission-failure, stale-claim, RPC-bound, PD-down all fail closed with typed errors. PASS.
10. Capability isolation: holds CREATOR-scope lifecycle-only, Q9 gate byte-identical, PD never a stop caller, no new cross-domain channel (bounded Call only). PASS.
11. Evidence: test-history.txt rows present for OFF gates + release + ON exploratory (see item 8). PASS with S3 on the ON proc_elf 134/1 follow-up.

`audits/rejected_patch.diff` left untouched (holds a stale artifact; overwrite only on REJECT — not applicable).

DECISION: APPROVED
