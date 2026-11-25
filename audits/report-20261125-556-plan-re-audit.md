# AUDIT REPORT 20261125-102356-plan-556-re-audit
PATCH: audits/plan-556.md (after applying audits/rejected_patch.diff)
FILES: audits/plan-556.md, src/kernel/ipc/ipc.cpp, src/kernel/syscall/syscall_handlers_ipc.cpp, src/kernel/task/scheduler_switch.cpp, src/kernel/task/scheduler.hpp, src/kernel/test/park_seam.hpp, src/kernel/test/test_park_seam.cpp, src/lib/test.hpp, src/lib/test.cpp, src/kernel/test/bench_mk_ipc_baseline.cpp, docs/specs/ipc.md, docs/specs/microkernel-transition.md, docs/specs/irqguard-ledger.md

## FINDINGS
- [S3] audits/plan-556.md:52 — step count label vs table rows: header says "13 steps" but table enumerates 0..13 (14 rows including baseline 0). Baseline row 0 is not a code-changing step; counting is ambiguous. Advisory only — GREEN gates remain FAIL-closed per row.
  WHY: No correctness impact; retain as S3 doc note.

## PATCH
No new rejected_patch.diff — prior rejected_patch.diff (8 queries + 9 invariants + 13-step FAIL-closed table + 4-site blocked_in_recv clears + ordered RELEASE publish + per-TCB waiver + 3-arch trigger matrix + per-wait budget reset + slope FAIL gate) has been applied verbatim to audits/plan-556.md and closes all prior S1/S2 findings. Plan now verifiable.

Check 7: 8 pasted queries with one-line dispositions (4 graphify query/path/get_neighbors + 4 obsidian_search_vault) — PASS, staleness doctrine acknowledged.
Invariants: 9 rows (INV-TOK-1, INV-TKT-1, INV-RECV-1, INV-LOCK-1, INV-SW-1, INV-BOUNDED-1, INV-IRQ-1, INV-ZERO-1, INV-RET-1) each with enforcement point and status (TRUE/PARTIAL/VIOLATED with fixing step cited) — verifiable.
Steps: 0..13 each FAIL-closed with explicit GREEN gate (baseline evidence + per-class test-history rows, DEBUG-only counters relaxed outside decision, per-TCB conjunctive waiver bit-2 only, 4-site blocked_in_recv clears 1437/1458/1500 + STALE paths, ordered tag→flag→ticket→stamp RELEASE inside IrqGuard with Contended-not-failure rationale, generation re-check at deliver + REPLY_RECV entry + drop counter ipc_reply_dropped_count, typed REPLY_RECV face next_cap==0 fail-closed, bench parked until #582 sweep verified with slope FAIL gate slope_ticks_per_hop_x100<100, per-wait kDirectMaxAttempts=3 reset cur->ipc_direct_attempts=0 at ipc.cpp:909/1424, 3-arch parity aarch64 SGI11 / riscv64 SSIP / x86_64 0xE1 depth-2 idempotent zero-alloc) — green gates present.

DECISION: APPROVED
