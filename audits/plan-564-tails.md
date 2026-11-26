# PLAN: #564 F11 proc-mgr — tail closure (verify-and-close, two evidence-gated blockers)

## RETRIEVAL (audit check-7 — queries + dispositions, inherited + executed this session)

- graphify `query_graph` "proc-mgr process management fork exec waitpid kill TCB carve VSPACE_MAP" (BFS d=2, 545 nodes) — USED: `task.cpp` create/fork paths, `scheduler` add_task/find_task, `VMM::virt_to_phys_in_pml4`, `test_isolate` snapshot/restore. IGNORED: PCI/VFS/virtio noise (unrelated communities).
- graphify `query_graph` "carve_fork rollback fail unwind CarveBinding begin registry full preserve partial child" (BFS d=2, 123 nodes) — USED: `carve_fork()` (proc_fork_carve.cpp:278), `CarveStatus`, `CarveBinding`, `page_table_levels`, `current_task`; confirms the single `fail()` lambda is the only unwind path and that **no PMM fault-injection node exists in the tree** (searched: no `test_arm_page_alloc_fail_at`), which is the basis for dropping the "dev-lane PMM injector" premise in tail 2.
- graphify `shortest_path` "carve_fork" -> "CarveBinding::begin" (3 hops, via validate_subtree/CarveBinding) — USED: confirms begin() is reached only through carve_fork step 4, i.e. after `create_carved` (post-adoption), so a full registry is the post-adoption failure site the rollback stub needs.
- obsidian `search_vault` "proc-mgr F11 564" and "carve fork rollback proc-mgr" -> 0 matches. DISPOSITION: vault holds only audit mirrors; normative text is in-repo (`docs/specs/microkernel-transition.md` §8.8/§15, `docs/specs/ipc.md` §4). Precedent reports read: `audits/design-review-564-r2.md` (APPROVED, 3 S3), `audits/plan-564-stageD.md`.
- LEARNINGS applied: #361 (fault-injector must gate allocation ATTEMPTS; a PMM delta captured before frame frees must expect +N, and each conjunct is printed separately) — drives T4's delta-0-at-isolation wording; test-wait digest (need_resched + hlt, never BLOCKED-polling) — drives T4.17. Cautions were scanned for proc-mgr/carve hits: none contradict this plan.
- Files read as current truth (graph may lag): `src/kernel/nexios_config.h` (knob defaults + stale comment), `src/kernel/proc/proc_fork_carve.cpp` (fail-lambda unwind order), `src/kernel/cap/carve_binding.cpp` (begin() claims a FREE slot with no task lookup), `tools/test-policy.tsv` (`class|proc_elf|build_scope|debug`), `scripts/run_all_classes.sh` (26-class list + per-arch extras), `meson.options` (`config_defs` override mechanism).

## AFFECTED FILES
- `src/kernel/nexios_config.h` — T1 only: correct the `CONFIG_PROC_MGR_CARVE_FORK` doc block (lines 1638-1652). It claims "shipped default, bit-identical" and "Default: 0"; the shipped state is debug 1 / release 0 and identity is behaviour-only (INV-PROC-7 downgrade, audit r2 OBS-1). Comment-only, identical line count, no statement change.
- `src/kernel/proc/proc_fork_carve.cpp` (+`proc_fork_carve.hpp` if a contract doc line is needed) — T2 only, and only if the #627 bisect convicts the carve path. Site chosen by evidence (aarch64 child frame construction vs teardown drain), never pre-committed here.
- `src/kernel/{task,memory,cap}/*` — T3 only if the #632 bisect convicts a specific 11-26 commit (`7232148ea` #516 death-funnel routing or `44bba28d6` 546-A IRQ contract). No file is edited before its step's evidence gate passes.
- `docs/specs/microkernel-transition.md` — T5: §15 F11 row `in-progress` -> `closed` (with matrix date/legs + residuals), §8.8.1 State records shipped defaults (debug 1/1, release 0/0), Appendix A gains the missing Stage D row (`e019cc467`, `dad528ae9`, `70ee29440`).
- `audits/plan-564-tails.md` — this plan (self-contained; no external session id is normative).
- NOT touched by dev lane (QE-owned, read-only here): `src/kernel/test/**`, `src/kernel/test/test_expected_counts.hpp`, initrd/test config, `tools/test-policy.tsv`, `test-history.txt` content (append-only rows are produced by the harness).

## INVARIANTS & CONCURRENCY BOUNDARIES
- Zero behavioural delta on every path except the two evidence-gated fixes (T2/T3). INV-PROC-1..15 and INV-CARVE-1..10 stay exactly as shipped; no knob default value changes this cycle (T1 is macro-identical by proof).
- T2 boundary (carve fork unwind): the `fail()` order **destroy(child) first, then abandon(slot), then release composer pins (tables/frames/root/tcb)** is the Stage C audit S1 fix (destroy-before-pin-release prevents UAF on the carved TCB region). Any aarch64 change must preserve it verbatim. No lock is held across it; `CarveBinding` leaf lock only, task context only, no ISR, no allocation, no reschedule.
- T2 boundary (child frame): aarch64 child `context.sp_el0` is written at `kernel_stack_top - frame` in `proc_fork_carve.cpp`; it must stay inside `[kernel_stack, kernel_stack_top]` or #623's under-base validator fails closed (that validator now returns false, `a35809bbf2`). A fix that moves the frame must keep the X0=0 fork-return convention.
- T3 boundary (release leg): release has `CONFIG_PROC_MGR=0` and `CONFIG_PROC_MGR_CARVE_FORK=0`, so #632 cannot be an F11-code defect; prove knob-OFF identity before touching VFS/sched. No spinlock held across `reschedule()` (§11.1), every BLOCKED transition dequeues (§11.2), death funnel stays IrqGuard-leaf-only with `scheduler_lock_depth_own()==0`.
- QE test isolation: every new proc-mgr test runs under `test_isolate` snapshot/restore with `CarveBinding::snapshot_reset` + `FrameForeignMap::snapshot_reset` + `proc_tcb_caps::snapshot_reset` wired first (already in QE tree); ResourceTracker delta must be 0 and is the pass/fail oracle.
- Evidence discipline: one `test-history.txt` row per class invocation; a TIMEOUT/STALL is never recorded as PASS; identical-binary rerun is the only flake remedy (policy `max_attempts=2`).
- Close ordering: matrix green (C1) -> docs flip (T5) -> close (C6). No docs row flips before the evidence exists.
- Cross-arch parity: every production item ships x86_64 + aarch64 + riscv64 in the same cycle; #627 already forces the aarch64 leg, and the 3-arch build/link + knob-combo matrix is the parity proof for T1.

## STEP-BY-STEP CHANGES

### Tail triage (each quoted open tail -> disposition)

| # | Tail | Disposition | Owner |
|---|---|---|---|
| 1 | E2E stub `pid_reuse_safe_carved` | QE-handoff (test file; no dev delta) | QE |
| 2 | E2E stub `carve_fork_rollback` "needs dev-lane PMM injector" | QE-handoff, re-scoped: **no injector needed** — `CarveBinding::begin()` full is the exact post-adoption failure site | QE |
| 3 | off-x86 legs (aa64/rv64 dump-counts + runs) | QE-handoff; aarch64 leg currently BLOCKED by open S2 #627 | QE + dev (T2) |
| 4 | release legs (proc_elf debug-only per policy) | already-done (`fec55d268` policy row; 23/23 green at `814763348`); re-run BLOCKED by open S2 #632 | QE + dev (T3) |
| 5 | full 2x3 matrix (leg 1/6 started) | QE; must run on a tree re-merged to the current main tip, not `d8829fa51` | QE |
| 6 | combined tree with main #623 `a35809bbf2` | superseded: main tip is now `518af5b44` (`d8829fa51` is 10 commits stale) | QE |
| 7 | (discovered) config comment contradicts shipped default | dev production, comment-only, audited | Dev (T1) |
| 8 | (discovered) #627 aarch64 `fork_marker_smoke` red (PMM +45 / Tasks +1, green 11-24) | dev production, evidence-gated | Dev (T2) |
| 9 | (discovered) #632 x86_64 release selftest stall at 68/87 | dev production, evidence-gated (blocks tail 5) | Dev (T3) |
| 10 | (discovered) spec §8.8.5 test names vs in-tree tests | QE ledger (map or file coverage issue) | QE (T4) |

### T0 — Baseline re-verification (no edits)
1. Pin main tip: `git log --oneline -1` must be >= `518af5b44` (recorded at planning: `518af5b44` 2026-11-26T18:26Z). Record that the QE station merge `d8829fa51` (2026-11-26T08:18Z) predates `7232148ea` (#516), `44bba28d6` (546-A), `eb1ece352` (546-B), `2bdc19672` (546-C), `518af5b44` (546-D) — the stale-merge tail is therefore still open by construction.
2. Confirm shipped knobs: `grep -n CONFIG_PROC_MGR src/kernel/nexios_config.h` -> debug 1 / release 0; `CONFIG_PROC_MGR_CARVE_FORK` debug 1 / release 0; cross-knob `#error` present.
3. Confirm `class|proc_elf|build_scope|debug` in `tools/test-policy.tsv`; `#597` closed; OVMF preflight present in `scripts/run_all_classes.sh` (lines 114-120).
4. Post T0 output as the work-begun comment on #564 (with the retrieval dispositions above).

### T1 — Config doc-truth (production, comment-only, audited)
5. `nexios_config.h:1638-1652`: replace three comment lines in place (identical line count): `0 = kernel-composed clone (release default; behaviour-identical)`; `Default: 1 on DEBUG, 0 on release (F11 Stage D, 70ee29440; release flip awaits a separate matrix decision)`. No `#define`/validator line changes.
6. Proof of no-op: for each of {x86_64,aarch64,riscv64} x {debug,release}, dump the macro set before/after and `cmp`:
   `echo '#include <kernel/nexios_config.h>' | <arch>-elf-g++ -std=c++20 -ffreestanding -I src -D<arch-define> [-DCONFIG_DEBUG] -dM -E -x c++ - | sort` -> byte-identical (6/6). A non-identical dump means a behavioural change and aborts T1.

### T2 — #627 aarch64 fork_marker_smoke (production, evidence-gated)
7. Reproduce on main tip: `python3 tools/meson/qemu_test.py --arch aarch64 --build debug --class arch_aarch64` — expect 37/1 at `test_aarch64.cpp:192` with PMM +45 / Tasks +1 fallout. If it is green, record the run and out-of-scope #627 with that evidence (no edit).
8. Knob bisect (same source, build-time only): `meson configure build/meson-aarch64 -Dconfig_defs=-DCONFIG_PROC_MGR=1 -Dconfig_defs=-DCONFIG_PROC_MGR_CARVE_FORK=0` then re-run. GREEN => carve-fork-ON regression (T2 is #564 work). RED => the verdict path / EL0-fault path is implicated; escalate to #614-`f0e0ae6ee` / #607-`504a91825` as the suspect set, keep #627 as the close-blocker and hand the fix to that owner with the same gate.
9. If carve path: instrument (CONFIG_DEBUG only, no control-flow change) the child's teardown — `CarveBinding::live_count()`, `FrameUserMap::live_count()`, PMM/Tasks counters — around child exit; GDB via manual QEMU `-s -S` + `aarch64-elf-gdb` per Rule D1 (no batch hardware watchpoints) to read the child's `sp_el0` frame and the EL0 return. Convict exactly one of: (a) child trap-frame construction differs from the clone path, (b) teardown does not drain the binding/frames (accounts for +45 pages), (c) child never reaps (Tasks +1).
10. Minimal fix at the convicted site only. Reverting `70ee29440` is NOT allowed without owner sign-off (it abandons the F11 flip).
11. Gates: `arch_aarch64` 38/38 x3 consecutive; `proc_elf` + `process_lifecycle` + `core_task` x86_64 debug; 3-arch build/link; style 0; SIL 3 code audit APPROVED on the diff.

### T3 — #632 x86_64 release selftest stall (production, evidence-gated; prerequisite of tail 5)
12. Reproduce: `python3 tools/meson/qemu_test.py --arch x86_64 --build release --class selftest`; capture the serial across tests 67->68 (`vfs_fat32_identity_reject_drift` -> next RUN_PRE then silence).
13. Establish the last green release tree: `814763348` (2026-11-25T22:29Z, "23/23 release SKIP proc_elf"). Re-run release selftest there (worktree; no branch switch). GREEN => #632 is a 11-26 regression, bisect `7232148ea` (`#516` kill routing) then `44bba28d6` (546-A IRQ contract); RED => genuine pre-existing (record identical-binary evidence; Rule B requires the fix anyway).
14. Minimal fix in the convicted subsystem. If the conviction lands in the 546 lane, prefer the smallest local fix over a revert of A/B/C/D; a policy row (SKIP) is NOT acceptable without a filed defect + owner approval, and is QE-owned when used.
15. Gates: x86_64 release selftest 87/87 x2; release leg 23/23 with proc_elf absent; x86_64 debug `storage` + `selftest` (regression blast radius); SIL 3 audit APPROVED.

### T4 — QE handoff (no dev edits; exact files, names, assertions)
16. `src/kernel/test/test_process.cpp` — finish `carve_fork_rollback` (replace pseudocode). Deterministic post-adoption failure with **no new kernel seam**: fill the binding registry with `CONFIG_CARVE_BINDINGS` (64) synthetic `cap::CarveBinding::begin(pid, gen)` claims (distinct pairs; `begin()` performs no task lookup), then `fork()` from a real `create_user` parent. Assert: (i) fork returns the typed refusal, no published child; (ii) `live_count()` is 64 before and after (no neighbour slot drained); (iii) the carved child was destroyed (registry/all_tasks count unchanged, Tasks counter unchanged); (iv) dmesg `TASK + 22` "Carve fork rolled back" observed exactly once; (v) ResourceTracker delta 0 (PMM/MemPool/FrameUserMap/CarveBinding); (vi) epilogue abandons all 64 and relies on `snapshot_reset`. Second leg if cheap: cap-table-full mid-composition (after tables/frames are carved) to exercise the composer-pin unwind. Delta discipline per LEARNINGS #361: capture PMM/Frame deltas at the same point on the success and failure legs and print each conjunct separately — a capture taken before the unwind frees must expect +N, not 0; only the post-isolation ResourceTracker check asserts 0. If no reachable mid-composition seam exists, the residual is a filed coverage issue, NOT a #564 close blocker — the `fail()` unwind is the same code for every post-adoption status.
17. `src/kernel/test/test_process.cpp` — finish `pid_reuse_safe_carved` (real PD-verdict E2E, x86_64 debug ON): carved child exits, pid is reused, a stale `(pid, generation)` hold/lookup is refused while the live task is untouched. Follow docs §8.8.5 ON-world rule: wait on the INV-WAIT-4 publication (`wait_point_reached()` / `g_reached_wait`), never on `state == BLOCKED`; call `Scheduler::SwSlots::need_resched()` (via `wait_until_yielding()`) before `hlt`; surface `TASK + 21` in FAIL messages.
18. `src/kernel/test/test_expected_counts.hpp` — bump `process_lifecycle` (x86_64/aarch64/riscv64) and `proc_elf` off-x86 rows for the tests above; no count row is edited before its green run; `--dump-counts` CONSISTENCY must show 0 delta.
19. Spec §8.8.5 coverage ledger: enumerate every name (`proc_mgr_parity`, `pid_reuse_safe`, `proc_mgr_crash_restart`, `proc_mgr_stop_authority`, `proc_mgr_hold_full`, `proc_mgr_death_visible`, `carve_fork_parity`, `carve_fork_rollback`, `carve_activation_guard`, `carve_foreign_frame_map`, `pid_reuse_safe_carved`, `carve_tcb_storage`) and map each to an in-tree test or a filed coverage issue with owner/date. #564 close requires zero unledgered names.
20. Legs: aa64/rv64 debug `process_lifecycle` + `servers` + `ipc` (+ policy-permitted `capability`/`hal`) with updated counts; `cap_vspace` is x86-only (count row 0) — state it as "not an off-x86 leg", never as SKIP. Release leg excludes `proc_elf` by policy; `proc_elf` appearing in a release leg means policy plumbing regressed -> fail loud.
21. Matrix: on qe-station, merge the current main tip (>= `518af5b44`) into `testbed`, assert `git merge-base --is-ancestor 518af5b44 HEAD` and `git merge-base --is-ancestor a35809bbf2 HEAD`, then `bash scripts/run_all_classes.sh {x86_64,aarch64,riscv64} {debug,release}` = 6 legs, 0 failures, one history row per class per leg.

### T5 — Docs close-out (after C1 green; docs-only, no code audit)
22. §15 F11 row -> `closed` with the matrix date, the 6 leg results and residuals; §8.8.1 State: shipped defaults debug 1/1, release 0/0 + matrix pointer + release-flip-not-taken note; Appendix A: add the Stage D row (self-call guard INV-PROC-14, terminate-self INV-PROC-15, parity oracle, D6 flip).

### T6 — Issue close (owner-approved)
23. Post one evidence comment on #564: per-leg class counts, matrix summary, style Errors 0, audit report path + `DECISION: APPROVED` for T1/T2/T3 diffs, ledger residuals with issue links. Close as completed only when C1-C5 hold. Milestone v0.6.1 is not touched by this plan.

### Non-goals (explicit)
- Flipping `CONFIG_PROC_MGR`/`CONFIG_PROC_MGR_CARVE_FORK` to 1 on release (needs its own plan + fresh matrix + owner decision; release stays 0/0).
- Adding a PMM (or any) fault-injection seam for the rollback test — the registry-full seam is sufficient; a future injector is a separate hardening issue.
- Any `src/kernel/test/**`, `test_expected_counts.hpp`, initrd/test-config or `tools/test-policy.tsv` edit from the dev lane.
- New `proc_mgr_parity` class / `carve_tcb_storage` implementation by dev (QE; ledger item).
- Untyped-quota work (M12), hold-table quota extension (#574), ProcSvc/proc_mgr_proxy refactors, IPC/scheduler rework not convicted by T2/T3.
- Milestone close, release tag, or version bump.

## TEST STRATEGY
- New tests (QE-owned, stub-first per PROMPT-dev.md): `carve_fork_rollback` (post-adoption unwind at a full binding registry: typed refusal, live_count stable, child destroyed, TASK+22 once, ResourceTracker delta 0) and `pid_reuse_safe_carved` (stale `(pid,gen)` hold/lookup refused after carved-child exit + pid reuse, live task untouched, constitution-compliant waits).
- Dev-lane verification after T1/T2/T3 (x86_64 debug unless noted): `cap_vspace` 16/16, `process_lifecycle` 20/20, `proc_elf` 147/147, `core_task` 66/66, `capability` 203/203, `cap_tcb` 6/6, `ipc` 110/110, `servers` 75/75, `selftest` 165/165, `core_sched` (validate_switch/#623), `storage` + `selftest` release (87/87); aarch64 `arch_aarch64` 38/38 (T2); riscv64 `arch_riscv64`.
- Knob-combo parity gate (build-only, 3 arches x 3 legal combos): (OFF,OFF), (ON,OFF = Stage B clone), (ON,ON = shipped debug); (OFF,ON) must fail the cross-knob `#error`. Builds via `-Dconfig_defs=-DCONFIG_PROC_MGR=<0|1> -Dconfig_defs=-DCONFIG_PROC_MGR_CARVE_FORK=<0|1>`.
- Host gates: `python3 tools/validate_style.py --quiet` -> Errors 0; `meson test -C build/meson-<arch> --suite host` (parity/policy/counts) for all three builddirs.
- Full matrix: `bash scripts/run_all_classes.sh {x86_64,aarch64,riscv64} {debug,release}` on the merged tree, 6/6 legs 0 failures, `proc_elf` excluded on release by policy.
- Known-red policy: #626 (S3 tick-race flake), #633 (S3 tls publish flake) are tolerated only as identical-binary rerun-green; #627 and #632 are hard blockers (not tolerated).

### Close criteria (C1-C5) and ownership
- C1 MATRIX: 6/6 legs 0 failures on a tree containing main >= `518af5b44` + the QE test tree. Owner: QE (qe-station).
- C2 CLASS EVIDENCE: x86_64/aa64/rv64 debug proc-mgr legs green with counts matching `k_expected_counts`; release 23/23 with `proc_elf` absent. Owner: QE.
- C3 DEV GATES: 3-arch build/link green (debug + release), knob-combo gate 3x3, style Errors 0, T1/T2/T3 audits APPROVED. Owner: Dev.
- C4 COVERAGE LEDGER: every §8.8.5 name mapped to a passing test or a linked residual issue. Owner: QE.
- C5 DOCS: §15 row closed, §8.8.1 state accurate, Appendix A Stage D row present. Owner: Dev.
- Close action: owner-approved close after C1-C5; evidence comment per T6.

## SIL 3 / SAFETY RISKS
- [S1] Matrix evidence from the stale merge `d8829fa51` omits #516 death-funnel routing and 546-A..D scheduler/IRQ changes that fork/waitpid depend on — closing on it would certify an untested combination. Mitigation: ancestry assertion + re-merge before any matrix leg (T4.21).
- [S1] #627 already shows an aarch64 fork child leaking +45 PMM pages and +1 Task; closing #564 over it would ship a leaking carve-fork path on aarch64. Mitigation: hard blocker, knob-bisect + GDB conviction, 38/38 x3 gate (T2).
- [S2] `carve_fork_rollback` left as pseudocode leaves the one post-adoption unwind path (whose Stage C audit already caught an S1 destroy-before-pin-release UAF) unverified. Mitigation: T4.16 assertion set on the exact failure site, delta-0 oracle.
- [S2] #632 leaves the release leg unverifiable; even though release ships knobs OFF, an unrun release gate hides regressions in the same binaries (VFS/?). Mitigation: T3 evidence-gated fix before tail 4/5.
- [S2] `nexios_config.h` comment states "Default: 0" / "bit-identical" for a knob that is 1 on debug and behaviour-only-identical — operator/maintainer misreads are a real configuration hazard (SIL 3 cfg-management). Mitigation: T1 with `-dM` identity proof.
- [S3] A rollback test that asserts a flat 0 delta at the wrong point can fail on the *correct* unwind order (freed-after-capture, LEARNINGS #361) or pass while a limb is broken; mitigate: per-conjunct prints + delta 0 only at the post-isolation check.
- [S3] QE filling all 64 binding slots could leak into later tests. Mitigation: epilogue abandon-all + `snapshot_reset` (already wired) + ResourceTracker delta check.
- [S3] Release exclusion of `proc_elf` (247 s STALL evidence) can mask a genuine release-only defect. Mitigation: policy row is documented and provenance-linked; revisit only with new evidence, never widen silently.
- [S3] Docs row flipped before evidence would be a false close. Mitigation: enforced T5-after-C1 ordering.
- [S4] Style/clang-tidy warnings unchanged (non-blocking).

## OPEN QUESTIONS
- Q1 (blocks close scope): is the release flip (both knobs 1/1) part of #564's done-criteria, or deferred to a separate issue? Plan default: deferred (non-goal); a "yes" adds a config flip + a second matrix and invalidates C1 as written.
- Q2 (blocks T2 branch selection): if the #627 knob bisect is RED (clone path also fails), does the fix stay in this cycle as a close-blocker, or is #627 handed to the #614/#607 owner while #564 waits on the aa64 leg? Either way C1 cannot pass until it is fixed.
- Q3 (blocks T3 ownership): if #632 convicts `7232148ea` (#516) or `44bba28d6` (546-A), does that lane fix it, or does this plan absorb it? The release leg is blocked until then.
