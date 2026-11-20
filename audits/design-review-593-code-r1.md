# CODE REVIEW r1 — issue #593 (plan-593.md + design-review-593-r1.md)

PATCH: /tmp/pending_patch_593.diff (uncommitted production diff on main, base ec9d2173b; ~290 lines)
FILES: src/kernel/task/scheduler_terminate.cpp, src/kernel/task/scheduler.hpp,
  src/kernel/task/scheduler_lifecycle.cpp, src/kernel/syscall/syscall_handlers_process.cpp,
  src/kernel/daemon/daemon_mgr.cpp, src/kernel/log/dmesg_catalog.hpp,
  docs/specs/scheduler.md, docs/specs/zombie-list.md
  (graphify-out/* dirty = expected hook noise, out of scope; test-history.txt rows
  are appended evidence, excluded from the patch per brief; no test files, no
  nexios_config.h, T-1 untouched — verified via `git status --short` + `git diff --stat`)

EVIDENCE READ: full patch text; scheduler_terminate.cpp:46-65 (latch), :82-217
  (terminate_err scope-B, byte-identical — untouched by patch), :242-301 (trap funnel);
  scheduler_lifecycle.cpp:70-114 (release_zombie), :354-364 (collector spare), :407-464
  (cleanup_step spares + #409 claim guard); syscall_handlers_process.cpp:120-197, :255-302
  (waitpid gate + R-1 legs), :479-501 (self-fatal leg); daemon_mgr.cpp:458-496;
  dmesg_catalog.hpp:60-67; scheduler_switch.cpp:943-968 (standing-arm shape);
  riscv64 syscall_entry.S:238-251 (force_apply gate); microkernel-transition.md:729-735;
  test_daemon_restart_crash.cpp:150-199; test-history.txt rows 7674-7683.
  Style: `python3 tools/validate_style.py --quiet` → Errors: 0 (Warnings: 2588, non-blocking).
  Check-7: no GitHub tool access from this session; relying on plan-593 lines 3-9
  retrieval artifacts (verified by design-review-593-r1 §6) — limitation recorded, not a finding.

VERDICT PER BRIEF ITEM:
1. Self-fatal leg = EL0-fault shape (funnel + switch_away_from_terminating), inline
   TERMINATED/publish/disclosure deleted, cause param Fault/Kill by signal class,
   terminate_err scope-B untouched and unreachable from trap — HOLDS.
2. Order dequeue → TERMINATED → admin-clear → deadline-remove → join-note →
   release_zombie → wake → reaper-poke → publish (terminate_terminate.cpp:259-299);
   wake keys on parent_id/find_task, release_zombie never clears parent_id;
   teardown_step order Terminate→Release unchanged — HOLDS.
3. WakeResult consumed via `(void)wake` with no-branch rationale (residence ⇒
   PARENT_COLLECTS spared by INV-WAIT-5 until collect_zombie_child, else ordinary
   drain; wake-after-release means no in-table corpse exists to defer) — rationale
   verified sound; no branch owed — HOLDS.
4. AP-live spares: collect predicate `!is_current_on_any_cpu` (:361) + waitpid in-table
   gate (:170-171) close the corpse-still-current window; reverse race lands on the
   pre-existing R-1 loud-deny legs (SCHED+23 + -1 at :268-272 and :296-301) — HOLDS.
5. Daemon: guard order find/current/magic/state (:458-462); terminate_err(*,0,Admin);
   refusal zero-mutation (refusal precedes ENSURE/lock/mutation at :96-104), entry+PID
   retained (return before notify_death), DAEMON+13 slot free tree-wide (only claim;
   +1000 in test_dmesg is a probe); no switch-away (local current excluded, AP-current
   refused pre-mutation, remote non-current needs dequeue+release only) — HOLDS.
6. F4 admin latch cleared via funnel (:266-270, same fields as terminate_err :133-137,
   no new site); #221/#523 preserved (drains spare at cleanup_step :430/:441, untouched);
   DeathCause single latch first-writer-wins (:59-61), funnel publish is the sole writer — HOLDS.
7. No asm changes; rv64 force_apply gate spot-verified (:248-251); S3-note-3 standing-arm
   force_apply+return (:955-959) byte-identical to shipped #523 shape, reschedule path
   untouched — HOLDS.
8. Style Errors 0 confirmed by execution (rc=0); T-1 untouched; docs subsections match
   code (scheduler.md release→wake order + spares; zombie-list.md §7.3 kill-self shape) — HOLDS.
9. Residuals: servers_daemon_restart 2/2 PASS (row 7678) despite the predicted FAIL —
   the #409-absorbs explanation HOLDS WATER: teardown's manual `Scheduler::terminate`
   (:195) double-pushes an already-released corpse, but release_zombie's table removals
   are membership-guarded/idempotent (:89-96), the list push is the only duplicate, and
   the drain's second pop fails `try_claim_teardown` → early return, no second free
   (:460-461, "leak-free — the owner frees it"); count balances, isolate delta 0 or the
   run would have failed. Green for the right reason, not a masked leak.
   Capability trio FAILED:3 identical pre (11-19) and post (11-20 08:40/08:41) — same
   F2 set, no new failures. QE testbed edit downgrade to cleanup — AGREE (stale comment
   + redundant push are deterministically absorbed single-threaded; deferrable).

S3-NOTE DISPOSITION (design-review-593-r1): S3-1 moot (microkernel-transition already
  reads IMPLEMENTED; dev correctly left it); S3-2 honored (single-caller contract noted
  at daemon_mgr.cpp:471-476); S3-3 honored (switch/reschedule code untouched).

## FINDINGS
- [S3] docs/specs/microkernel-transition.md:735 — "(+12 dmesg row)" now omits the new +13 refusal row.
  WHY: Cosmetic catalog-reference staleness; lookup is table-driven, no behavior impact.
- [S3] src/kernel/test/test_daemon_restart_crash.cpp:191-195 (QE-owned, testbed) — teardown comment
  ("queues exactly once; it was never queued") is now factually inverted and the manual
  `Scheduler::terminate` is a redundant second push.
  WHY: Deterministically absorbed per item 9 (no leak/UAF, 2/2 PASS delta 0), so cleanup-grade,
  not merge-gate; fix on testbed at QE convenience.

DECISION: APPROVED
