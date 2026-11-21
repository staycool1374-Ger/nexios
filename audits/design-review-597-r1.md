# DESIGN AUDIT R1 — Issue #597 (SIL 3 design review of audits/plan-597.md)

PLAN: audits/plan-597.md
BASE: main tip 63e1c2f95 (verified via `git log --oneline -1`; branch `main`)
FILES (plan scope): src/kernel/task/scheduler_lifecycle.cpp (site 1),
src/kernel/syscall/syscall_handlers_misc.cpp (site 2),
src/kernel/syscall/syscall_handlers_thread.cpp (site 3),
src/kernel/task/scheduler.hpp (doc-comments only)

## EVIDENTIARY BASIS (re-read this session, not testimony)

- Site 1 hunk exists: scheduler_lifecycle.cpp:685-707 (BLOCKED gate :685,
  `task_stack_ptr(p)` guard :690, frame writes :700-704, pid clear :707).
- Site 2 hunk exists: syscall_handlers_misc.cpp:202-218 (`TASK_STACK_PTR(p)`
  guard :204, frame writes :212-216, riscv split :212-215).
- Site 3 hunk exists: syscall_handlers_thread.cpp:244-256 (`if (frame)` :249,
  frame writes :250-254).
- Kernel early-return: proc_svc.cpp:390-391 verbatim
  (`if (!syscall_is_user_task()) return SYS_ERR_GENERIC_FAIL`); publish
  :304-318, in-trap loop :397-404.
- Status delivery without frame patch: deliver_wait_status at
  scheduler_lifecycle.cpp:667-668, misc:174, thread:223; pid clear :707.
- No production WAITPID caller: `SyscallNumber::WAITPID` referenced only in
  src/kernel/test/** + arch tests + nexios_config.h gate.
- is_user_ authority: task.hpp:669-674 verbatim (MP-1, page_table_ invalid,
  "This flag is the authoritative one"). All production writers are
  pre-publish birth funnels (task.cpp:498/1109/1704/2218/2712,
  elf.cpp:631/878, elf_loader.cpp:847); no post-publish writer exists.
- task_stack_ptr: scheduler_integrity.hpp:41-53 (rsp/sp_el0/sp per arch).
- Kernel birth stores nonzero stack ptrs on all arches: task.cpp:1534 (x86),
  :1551 (aa64 sp_el0), :1566 (rv sp).
- User trap-return reads the patched slot on all 3 arches: x86
  kernel.cpp:2762-2766 (`regs[0] = syscall_handler(...)`, iretq through frame);
  aa64 syscall_entry.S:63-70 (return published to slot 0 pre-switch,
  restore_all loads x0 from slot 0; no-switch epilogue :108 from +272) + eret
  :111; rv64 syscall_entry.S:217 (`sd a0, OFF_A0(sp)`, OFF_A0=72=:29).
- Harness live-RSP citation real: scheduler_snapshot_fields.cpp:110-111.
- Fourth-site sweep: `frame[N] = <child-id>` parent writes occur at exactly
  the 3 sites. Other frame writes are birth context (task.cpp:2775/2791),
  debugger restore (debug_regs.cpp:201/210), tests — different pattern,
  no waitpid a0-override.

## FINDINGS

- [S3] audits/plan-597.md — "task.cpp:396 ctor" citation drift.
  WHY: the `is_user_ = false` default write is at task.cpp:498, not :396;
  all other line citations verified within a few lines — cosmetic only,
  authority claim itself confirmed verbatim at task.hpp:669-674.
- [S3] Test-harness post-hoc `is_user_` flips (test_cap_tcb.cpp, test_klog.cpp:223)
  coexist with the "write-once at birth" production claim.
  WHY: all flips are QE-owned harness TCBs, never production tasks; the plain-read
  race-free argument holds in production (no post-publish writer, waiter is
  BLOCKED), but QE must set the flag explicitly per new-test leg rather than
  inheriting it — the plan's QE sketch already does this (EL0 vs kernel shapes).
- [S3] sys_exit inline wake (misc:202-218) has no BLOCKED gate (unlike site 1);
  plan preserves this asymmetry deliberately.
  WHY: out of scope for #597 and behaviour-preserving; noted so Phase 3 does
  not "fix" it by accident — any change there needs its own issue.

## CHECK DISPOSITION (delegation items 1-8)

1. Fix shape: the three hunks exist exactly as described; `p->is_user_`
   short-circuit conjunct is the correct predicate at each — in production a
   user parent can only reach waitpid via trap (frame slot is genuine), while
   a kernel parent returns via direct C++ return (proc_svc.cpp:390-391) so its
   `task_stack_ptr` is live stack. Predicate correct at all 3 sites.
2. Kernel-path claim: VERIFIED — early return, deliver_wait_status +
   publication clear + readying preserve kernel observation; zero production
   kernel-task waitpid callers; no caller depends on the frame patch.
3. is_user_ authority: VERIFIED — task.hpp:669-674 verbatim; write-once
   pre-publish (exhaustive writer list above); page_table_!=0 invalid per MP-1.
4. Off-x86 correction: VERIFIED — task.cpp:1551/1566 execute the store into
   live kernel stack today on aa64/rv64 (latent, not benign); arch-neutral
   predicate + mandatory process_lifecycle (aa64/rv) and task_fpu (aa64/rv)
   evidence legs close it.
5. Scope honesty: no fourth site with this pattern (exhaustive grep above);
   the "twins untouched = REJECT" trigger is enforceable by the Phase 3
   auditor via the same grep over the final diff.
6. #221 intent: user leg keeps the identical store body; only one
   test/branch is added ahead — user-leg semantics byte-identical.
7. No new locks/config/syscalls/arch branches; regression plan names
   classes + 3-arch legs + style + history rows; dev lands no tests
   (test sanctity respected).
8. Check-7 retrieval real: 2 graphify queries + 1 shortest_path (no-node,
   correctly disposed as graph lag to scheduler_integrity.hpp:41-53) + 2
   vault searches, each with used/ignored disposition. Formally satisfied.
   False-close risk: none found — all corrupting stores gated, no residual
   site; the one directional residual (future kernel-mode trap caller) is
   unreachable today and the plan documents the precondition in-code.

DECISION: APPROVED
