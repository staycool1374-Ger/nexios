# CODE AUDIT R1 — Issue #597 (SIL 3 code audit of audits/pending_patch.diff)

PATCH: audits/pending_patch.diff
FILES: src/kernel/task/scheduler_lifecycle.cpp, src/kernel/syscall/syscall_handlers_misc.cpp, src/kernel/syscall/syscall_handlers_thread.cpp, src/kernel/task/scheduler.hpp
BASE: main tip 63e1c2f95 (verified via `git log --oneline -1`; branch `main`)
NORMATIVE: audits/plan-597.md + audits/design-review-597-r1.md (APPROVED, 3 S3s)

## EVIDENTIARY BASIS (re-read this session, not testimony)

- Diff read in full (86 lines, 4 files). Worktree diff-stat matches the patch
  (5/5/5/20-line deltas); no src/kernel/test/** file modified; test-history.txt
  carries 44 new rows (excluded from scope per delegation).
- Site 1 gate: scheduler_lifecycle.cpp:690
  `if (p->is_user_ && task_stack_ptr(p)) {` — dequeue_ready/set_task_ready
  (:688-689) above it, deliver_wait_status (:667-668) + pid clear (:713) outside.
- Site 2 gate: syscall_handlers_misc.cpp:207
  `if (p->is_user_ && TASK_STACK_PTR(p)) {` — deliver (:173-174), pid clear
  (:180), set_task_ready (:206) outside; no BLOCKED gate added or removed.
- Site 3 gate: syscall_handlers_thread.cpp:252 `if (p->is_user_ && frame) {`
  (reinterpret of THREAD_JOIN_STACK_PTR(p) preserved, null path intact) —
  deliver (:222-223), latch (:227-228), pid clear (:229), set_task_ready (:245)
  outside.
- Fourth-site sweep: `frame[N] = <child-id>` parent stores exist at exactly the
  3 gated sites (grep `= t->id` / `= child.id` over src/kernel: all other hits
  are tests, debug_regs restore, procfs, snapshot — different pattern).
  No ungated parent-pid store remains.
- Citations verified verbatim: task.hpp:669-674 (MP-1, "This flag is the
  authoritative one"); proc_svc.cpp:390-391 (non-user early return before the
  in-trap wait loop :397-404); GDB values in the comment (slot 0x403c7e,
  pc=0xa) match the issue hunt comment verbatim.
- scheduler.hpp:864-868 adds 5 doc-comment lines only, zero code change.
- Style: independently ran `tools/validate_style.py --quiet` → Errors: 0
  (578 files, 2660 non-blocking warnings).
- Gate rows in test-history.txt: process_waitpid 11/11 (00:03:32, 00:36:54,
  00:36:59 — three post-fix greens); scheduler_core 39/39 (00:15:47, 00:37:11,
  00:37:23); preemption 11+2fail (00:29:56) → 13/13 (00:31:53, flake retried
  green); process_lifecycle 16/16 (00:32:11); selftest 165/165 (00:37:48);
  proc_elf/scheduler_core TIMEOUT rows (00:36:24, 00:42:02, 00:07:45, 00:12:25,
  00:19:57) documenting the separately-reported stalls.
- LEARNINGS.md sweep (waitpid/wake/is_user_/live-stack): #217 (wake precedent —
  preserved here), #513 (is_user_-gated hlt — no hlt on this path), "only test
  code flips is_user_ post-birth" (consistent with the write-once claim).
  No repeat-defect pattern.

## FINDINGS

- [S3] audits/pending_patch.diff — issue-thread posting of retrieval artefacts
  and gate evidence still pending (work uncommitted; only 2 diagnosis comments
  on #597).
  WHY: check-7 artefacts live in audits/plan-597.md (ruled formally satisfied
  by design-r1 §check-8) and gate evidence in test-history.txt rows — the
  developer must paste both into the issue at work-begun/complete time;
  nothing in the diff depends on it.
- [S3] test-history.txt rows carry no arch field (harness format limitation).
  WHY: the x3-arch attribution of the process_waitpid/scheduler_core legs
  relies on session command history, not the rows themselves; row
  count/shape/timing (incl. the 11+2fail→13/13 flake pair) is internally
  consistent with the claim.
- [S3] GDB evidence for the two separately-reported stalls (x86-LTO
  scheduler_core stall, proc_elf TCB-exhaustion) is not yet in the issue
  thread; independence assessed statically.
  WHY: the diff adds one test+branch and only ever skips a wild store —
  user leg byte-identical, kernel leg strictly safer, no consumer depends on
  the skipped store (no production WAITPID callers; kernel observers key on
  publication/status) — so it cannot cause a stall on any leg; selftest
  165/165 + process_waitpid 11/11 post-fix corroborate.

## CHECK DISPOSITION (delegation items 1-8)

1. is_user_ gate at all 3 sites with the exact predicate (`p->is_user_ &&`
   short-circuit first); no fourth parent-pid store ungated (exhaustive grep).
2. Kernel path preserved: deliver_wait_status + pid-clear + set_task_ready all
   outside the gates at all 3 sites; sys_exit no-BLOCKED-gate asymmetry
   preserved byte-for-byte (only the guard line changed).
3. User leg identical: frame[9]/frame[0] store bodies untouched; one branch
   added ahead only.
4. Comment accuracy: trap-frame rule + direct-C++-return cite (proc_svc
   :390-391) + GDB values (0x403c7e, pc=0xa) + task.hpp:669-674 authority all
   verified verbatim in-tree; no task.cpp line cited (r1-S3a applied);
   no overclaim (mechanism = convicted root cause).
5. scheduler.hpp doc-only: 5 added `///` lines, no behavior change.
6. Style Errors: 0 independently confirmed; no new locks/config/syscalls/
   arch branches (one predicate, one branch per site).
7. Gate credibility: rows support every claimed leg (incl. flake-retried
   preemption); stalls documented as rows and assessed plausibly independent
   per static argument above; stale MAX_SYSCALL pins untouched (absent
   from diff).
8. r1 S3s applied: no task.cpp citation drift; no test files in diff;
   sys_exit asymmetry intact.

DECISION: APPROVED
