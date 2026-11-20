# AUDIT REPORT 20261120T004757Z — code audit, issue #590 (SIL 3)
PATCH: /tmp/pending_patch_590.diff (uncommitted production diff on main @ e98296a7c; graphify-out/ + test-history.txt drift excluded per scope)
FILES: docs/specs/scheduler.md, docs/specs/zombie-list.md, src/kernel/log/dmesg_catalog.hpp, src/kernel/syscall/syscall_handlers_misc.cpp, src/kernel/syscall/syscall_handlers_process.cpp, src/kernel/syscall/syscall_handlers_thread.cpp, src/kernel/task/scheduler.hpp, src/kernel/task/scheduler_lifecycle.cpp, src/kernel/task/scheduler_snapshot_fields.cpp, src/kernel/task/scheduler_watchdog.cpp, src/kernel/task/task.hpp, src/kernel/test/wait_publish_seam.hpp (new; DEBUG-gated, planned exception)

## VERIFICATION (claim-by-claim against plan-590 rev 2 + design-review-590-r2.md APPROVED)

1. Publish order + fence pair (INV-WAIT-4/3): genuine. sys_waitpid publishes
   status → dequeue_ready → BLOCKED → pid LAST (process.cpp:214-218), fence
   immediately after (224), covering the kernel-caller return too. Wake-side
   fences at wake_waiting_parent entry (lifecycle.cpp:641), sys_exit (misc:155),
   sys_thread_exit (thread:207) — exactly 4 SEQ_CST sites, no others in
   task/syscall TUs (vfs_srv/block_srv fences are all RELEASE/ACQUIRE, so
   SEQ_CST is a new codegen class as claimed). riscv64 lowering VERIFIED in
   post-patch build/kernel.elf: SEQ_CST thread-fence → `fence rw,rw` at the
   publish point and the wake entry; waiter acquire-load → `fence r,rw`;
   flag release-store → `fence rw,w`. `fence rw,rw` is the CORRECT GCC SeqCst
   lowering for normal memory (orders all prior R/W before all later R/W;
   forbids SB under RVWMO) — the plan step-19 expectation string
   "fence iorw,iorw" is inaccurate (iorw adds device-I/O ordering GCC never
   emits for atomic_thread_fence). Code correct; expectation text wrong (S3-1).
2. INV-WAIT-6 precedence: sound. Post-fence `!= target_pid` continue
   (process.cpp:240) resolves via top-of-loop collect; undo (-1/block paths)
   unreachable on the consumed leg. Only-writer claim holds for production:
   writers are publish (218), undo-clear (253), wake clears
   (lifecycle:702, misc:180, thread:229); task.cpp:1810,2212 zero NEW TCBs
   (different object); snapshot restore deliberately rewrites NO wait state
   (capture-only, snapshot_fields.cpp:237-243); shell:2209 read-only.
   Interleaving analysis: wake always delivers-before-clear, so
   consumed ⇒ delivered; late-clear vs undo converges via hlt-exit+rescan
   collect (status via local status_ptr, never the nulled slot). No loss path.
3. Second residence: sound. collect_zombie_child pops under IrqGuard +
   zombie_lock_ leaf, claims in-leaf, re-links + reports false on lost claim
   (never double-free/leak), runs remove_child+cleanup+pinned-aware-free
   OUTSIDE the leaf. Retention flag set ONLY at the 3 wake match points
   (lifecycle:678, misc:178, thread:227 — release stores, single-writer
   terminator, no SB pair introduced). All 4 drains (flush/drain/
   cleanup_step/cleanup_step_try) + kill_declined (lifecycle:1033) consult
   wait_collect_spared; owner-death escape (parent gone/TERMINATED ⇒ free)
   leaves no leak; watchdog-skipped shape covered by reap_orphans.
   #523 traced: no-match wake sets no flag ⇒ immediate free, as today.
   Re-resolve by id only (find_wait_child_by_id); no pointer crosses window.
   R-1 leg: SCHED+23 WARN + return -1, never hang/leak/UAF (accepted per r2).
4. Flag placement: after join_reaped_ in the bool cluster (task.hpp:851),
   zero-size-growth claim structurally sound; F21 pool-size static_assert
   (task.hpp:1474) holds — post-patch tree builds+links+runs (kernel.elf
   01:44 + x86_64 debug rows below). Restore is capture-only (deliberate
   parity — sound: a rewound waiter must not inherit a stale latch).
5. Seam: DEBUG-gated exactly as planned — header #if/#else (release no-ops),
   gated include (process.cpp:203-205), gated call site (349-351), gated
   definitions (lifecycle:758-810), single-shot exchange, RAII dtor clears
   unfired hook, UP-only + first-armer-wins fail-closed. Release zero-cost
   holds by structural inspection (no reference survives without
   CONFIG_DEBUG). No release gate row post-patch — covered by S3-3.
6. R-1 path: loud deny confirmed (SCHED+22-adjacent +23 WARN, once per leg,
   then -1; retry-cap leg keeps publication and blocks — correct).
7. Style Errors 0 (validator re-run: 563 files, Errors: 0). No test-file
   edits (only the planned-exception header). No nexios_config/gen.mk drift.
   +23 singly registered (catalog:297) with lookup_severity arm (case 23).
8. Residuals: ipc 94/2 + capability 167/3 + proc_elf 134/1 corroborated —
   #594 (owner comment: byte-identical on clean base with #590 stashed,
   shared [RUNQ] harness signature) and #595 exist OPEN; test-history shows
   post-patch x86_64 rows (core_task/core_sched/deadline/selftest/
   process_lifecycle green; ipc-base H1-validation-base-identical;
   arch_riscv64 31/0 at 01:44). The byte-identical stash comparison itself
   is developer-attested (not independently re-executed here) — S3-2.
   C1 HONORED: #593 OPEN, cross-referenced in sys_kill comment (476-488)
   and scheduler.md. C3 HONORED: exact comment texts verified (precedence
   rationale, undo order, staleness disclosure in wait_collect_spared).
   C4 HONORED: dequeue_ready (scheduler_queue.cpp:442-460) takes no lock —
   pure queue ops + invalidate; lock-agnostic at all three call contexts,
   so no SchedulerLockGuard needed on either watchdog arm.

## FINDINGS
- [S3] docs/specs + plan step 19 expectation "fence iorw,iorw" (riscv64) — inaccurate
  WHY: post-patch binary shows GCC lowers SEQ_CST thread-fence to `fence rw,rw` (correct full barrier for normal memory); iorw is never emitted for atomic_thread_fence. Correct the expectation string; code needs no change. Per-arch objdump remains partial: riscv64 verified directly, x86_64 evidenced via post-patch build+run rows only, aarch64 has no post-patch gate row — run the aarch64 arch gate + record true lowerings (mfence / dmb ish) before merge.
- [S3] residual stash-validation (ipc/capability #594, proc_elf #595) — developer-attested, artifacts consistent but not independently re-executed
  WHY: #594 owner comment + H1-validation-base-identical row + matching failure signatures corroborate; the clean-base stash re-run itself was not re-performed in this audit. No code action; close via the filed issues' own verification.
- [S3] src/kernel/syscall/syscall_handlers_process.cpp:34 — unused `kernel/test/test_isolate.hpp` include (no symbol used in TU)
  WHY: harmless (precedent: vfs.cpp, pmm.cpp, scheduler.cpp include it; release-safe header), but dead include invites layering confusion. Remove or justify in one line.
- [S3] no post-patch aarch64 gate row in test-history.txt (last arch_aarch64 2026-11-16, pre-patch); no post-patch release gate row
  WHY: diff holds zero arch-conditional code (shared C++ only; riscv64 verified, x86_64 green), so this is evidence-completeness, not a code defect. Run arch_aarch64 + one release leg before merge to close plan §TEST STRATEGY / step 20.

DECISION: APPROVED
