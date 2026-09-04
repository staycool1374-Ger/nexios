# SIL 3 Audit Report — Issue #11 In-Register IPC Fastpath

- **Date:** 2026-09-04
- **Auditor:** auditor subagent (SIL 3 checklist: memory safety, concurrency/ISR, boundary/MP-3/MP-4, integer/overflow, invariant preservation, fail-closed, test honesty, ResourceTracker, CODING_STYLE)
- **Method:** code review of `git diff main` (13 files, +1288/-31) against docs/specs/ipc-fastpath.md INV-1..INV-9/INV-P; ABI verified against live `int $0x80` path (isr_stubs.asm:132-146, kernel.cpp:1623-1625, syscall.cpp FAST-bit); deferred-switch frame-survival verified (scheduler.cpp:2780-2828). Auditor ran ipc_fastpath 14/14 and syscall_fastpath 5/5 independently; developer gates: build Errors 0, 10 regression classes, debug all 1071/1071 (registered 1086), release all 85/85.

## Verifications
- ABI §3.2 exact (rsi/rdi/r8/r9/r10/r11 = regs[4,5,7,8,9,10], rbp excluded); libc wrappers pin registers via asm-register locals + matching constraints.
- Frame survival across blocking: deferred switch stores the ISR frame pointer in context.rsp; resume restores GPRs then iretqs — reply payload scattered into regs[] reaches the caller even after a real block/wake.
- pop() refactor byte-identical (out captured before compaction); pop_clamped is single-lock (no TOCTOU).
- send_sync default reply_max_size=0 → plain pop(); only the new fast handlers pass non-zero. All existing callers (syscall_handlers_fs.cpp:78,136 + 6 test TUs) pass 0.
- 128-bit mask: number < MAX_SYSCALL(77) < 128, bounds-checked before shift; table sized 77.
- WEDGE invariant preserved (BLOCKED then dequeue_ready, reschedule, sti/hlt/cli only for user tasks); wake_sender parity with IPC::recv; pop_clamped holds lock only, no scheduler call under lock.
- Fail-closed: INV-1/4/5/6/9 all hold; null regs → -1 in all three handlers.

## Findings
- [S3] F1 docs/specs/ipc-fastpath.md §5 — test names/asserts drift from impl (fast_latency_lt_full vs fast_latency_vs_full; canary intent). → FIXED (doc §5 table updated).
- [S3] F2 test_ipc_fastpath.cpp fast_no_user_deref_canary weaker than doc intent (no tampered canary; inherited structurally from FAST membership). → FIXED (doc amended to describe the implemented weaker check).
- [S3] F3 sys_recv_fast adds `else { arch::hlt(); }` for kernel tasks, which sys_receive lacks; matches audited send_sync kernel-task pattern, safe. → FIXED (INV-3 note documents the deviation; full path untouched).
- [S3] F4 fast handlers use x86_64 regs[] indices without an arch guard (unreachable on aarch64/riscv; wrappers x86_64-only). → FIXED (ARCH NOTE added to the handler block).
- [S3] F5 benign pop_clamped transient (concurrent push between lock release and is_empty() re-read → spurious -1, fail-safe). → FIXED (INV-4 note documents it).
- [S3] F6 loop index `w` in gather/scatter helpers — cosmetic, no action required.

## Verdict
No S1 or S2 findings. All invariants hold; memory safety, lock discipline, WEDGE, authority parity, fail-closed behavior and test honesty verified. Gates green.

**DECISION: APPROVED**

(No rejected_patch.diff required.)