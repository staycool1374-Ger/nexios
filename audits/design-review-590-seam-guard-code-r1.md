# CODE REVIEW — #590 follow-up (fire_wait_publish IrqGuard seam fix)

PLAN: `audits/plan-590-seam-guard.md` · DESIGN: `audits/design-review-590-seam-guard-r1.md` (APPROVED, 4×S3)
PATCH: `audits/pending_patch.diff` (29 lines) → working-tree `src/kernel/task/scheduler_lifecycle.cpp` (+18, verified identical)
BASE: `0eb94f82d` (`git log --oneline -1` matches). No test files in scope (verified: only production file + excluded `test-history.txt`/graphify artifacts).

## SOURCE VERIFICATION (all claims checked against tree)

- Guard at entry (`scheduler_lifecycle.cpp:567`), before exchange (`:568`), spans fire+hook (`:570-571`). Rationale comment cites funnel sites — all exact: `terminate_err` guard `:96`, wake `:152`, release `:165`; `terminate_err_from_trap` guard `:254`, wake `:293` (verified `scheduler_terminate.cpp`).
- `ID_TABLE_SIZE = 2*CONFIG_MAX_TASKS` (`scheduler.hpp:1509-1510`), `CONFIG_MAX_TASKS 64` (`nexios_config.h:58`) → bound **128**: comment correct (plan's stale "80" fixed in code).
- Affinity-clamp cite (`scheduler_registry.cpp:172-180`): exact — zero-mask→CPU0 clamp + `&= allowed`. UP unreachability of the `scheduler_lock_` branch follows: arm gate `up_cpu_count()==1` (`:537`), `queue_target` is pure ctz (`scheduler_queue.cpp:535-539`), so in `set_task_ready` (`scheduler_registry.cpp:560-609`) `target==here==0` → `enqueue_ready` fallthrough; the `:577-607` lock branch is textually unreachable, and even-if-reached order (IRQ-off→global, `ENSURE(depth==0)` at `:584`) is funnel-identical.
- `remove_child` sibling walk (`task.cpp:2810-2834`): form-unbounded but `num_children`-bounded (≤`MAX_TASKS`), and skipped on this path — BLOCKED parent ⇒ `waiter_collects==true` ⇒ skip (`scheduler_lifecycle.cpp:735-740`). Comment accurate.
- `dequeue_ready` (`scheduler_queue.cpp:496-514`): lock-free list ops + arm invalidation, no lock taken — same discipline as under the funnel guard.
- Success-path cleanliness: `find_task` bounded probes (`scheduler_id_table.cpp:100-131`); `deliver_wait_status` logs only on the cold fault leg via lock-free dmesg documented IRQ-safe (`scheduler_lifecycle.cpp:599-606`); `admin_absorb_wake` DEBUG-warn suppressed (IF=0 under guard); no alloc/IPC/VFS/`reschedule` anywhere on the path.
- Reentrancy: `IrqGuard` depth-counted, IF save/restore, saturating dtor (`irq_guard.hpp:135-189`); `proc_svc.cpp` contains zero `IrqGuard` (grep: no matches) ⇒ fire site depth 0→1→0, no stray `sti`.
- Release identity: whole function inside pre-existing `#if defined(CONFIG_DEBUG)` (`:523-593`); diff adds no `#ifdef`; `wait_publish_seam.hpp:56-77` `#else` no-ops and `proc_svc.cpp:319-321` call site untouched ⇒ zero release bytes by preprocessor structure.
- Multi-arch: `arch::IrqGuard` is HAL-shared, no arch-gated code in diff; include already present (`:40`); cannot break arch builds (no new symbols/includes).
- Style (static): all 18 added lines ≤90 cols (checked), 4-space, braces, no new `using`/single-char names. `make check-style` tool output not in-repo — see S3.
- Gate credibility (`test-history.txt`): post-fix `process_waitpid` 11/11 at 10:02:02, 10:02:41, 10:15:24, 10:15:38; release 1/1 at 10:16:14; adjacent green (lifecycle 16, signals 8, core_task 66, ipc_core 37, ipc_blocking 4). Pre-fix 10/1 row (11/21 22:55:44) consistent with the reported wedge. Full-class green implies both arming scenarios (`waitpid_missed_wake_consumed_collects`, `waitpid_reverify_wildcard_collects`) pass — WEDGE GONE corroborated.

## OPEN ITEM — core_sched HOST-BLOCKED assessment

- (a) **No core_sched test arms the seam.** Repo-wide grep: `WaitPublishSeamScope`/`arm_wait_publish` referenced only in `test_waitpid.cpp` (+ declaring header, included only there). Unarmed `fire_wait_publish` = guard + exchange-null + return; mechanistically inert for core_sched.
- (b) **IRQ-off window cannot flip timing tests.** Unarmed path ≈ 10¹–10² ns (guard ctor/dtor + one atomic exchange) vs 1 ms tick (`CONFIG_TICK_HZ=1000`, `nexios_config.h:71`) — ratio ~1e-4. Armed path (tens of µs) never executes outside the two arming waitpid tests. Reported failures (watchdog TIMEOUT, leak deltas) are not marginal-timing signatures.
- (c) **Disposition endorsed: HOST-BLOCKED + track-in-issue, no code change.** 0/3-vs-1/1 at n=4 under load avg 3.6–5.1/4 (saturated host) is statistically null; same-window noise corroborates loaded host (`core_syscall` 88/1, `capability` 198/2, `posix_time` 10/4 — all pre-existing patterns). **Requirement: re-run `core_sched` on an idle host before closing #590; do not merge-gate on it.**

## FINDINGS

- [S3] Style/off-arch evidence lives in the issue thread only — `test-history.txt` carries no arch tags and no `check-style` rows, so "Errors: 0" and the aarch64/riscv64 `process_waitpid` legs are auditor-unverifiable from the tree (added lines statically conform).
  WHY: audit check 11 evidence for those two claims must be pasted in the issue before merge; code itself is clean.

Counts: S1 0, S2 0, S3 1.

`audits/rejected_patch.diff` untouched (stale artifact left alone on APPROVE). Nothing committed.

DECISION: APPROVED
