# AUDIT REPORT 2026-09-14T16-40-00Z (addendum to 16-12-10Z)
PATCH: audits/pending_patch.diff
FILES: src/kernel/test/test_sched_affinity.cpp (only delta since 16-12-10Z)

## FINDINGS
- [S3] src/kernel/test/test_sched_affinity.cpp:56-156 — three placement tests (default_mask, lowest_bit_targets, requeue_moves) asserted ready-queue placement without IRQ exclusion; a timer tick between enqueue and is_queued_on dispatches the fresh task (RUNNING, not queued) and fails the assert
  WHY: Same race the tree's own cookbook Rule 2 documents for o1_scheduler (guard register+select); exposed under `all` by the timing-phase shift from the new apic_tpr class (standalone 6/6 green, suite red 1-2/1274).
  STATUS: fixed pre-report — whole enqueue/affinity/assert sequences under one IrqGuard in all three tests + doc-block notes (test-sanctity: doc-block and impl changed together); debug all 1274/1274 re-verified on the final tree, release all 85/85 re-verified.
- [S3] Change discipline verified for the addendum delta: no kernel behavior change (test file only; IrqGuard cli/sti nesting is safe; destroy_test_task under guard matches the o1 SimpleTaskPtr pattern).

Positive checks: final-tree gates — make build green, check-style Errors 0, debug all 1274/1274, release all 85/85, x86_64/aarch64/riscv64 builds green; test-history.txt rows appended per class run; no TEMP/DIAG/BISECT leftovers (grep-verified); retrieval artifacts on #26 (graphify + vault + dispositions).

DECISION: APPROVED
