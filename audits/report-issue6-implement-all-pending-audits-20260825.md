# SIL 3 Audit Report — P4 scheduler ID allocation + sweep (issue #6)

**Patch:** `audits/pending_patch.diff`
**Files:** `src/kernel/task/scheduler.cpp`, `src/kernel/task/scheduler.hpp`,
`src/kernel/task/ready_queue_manager.cpp`, `src/lib/logger.cpp`,
`src/kernel/sync/semaphore.cpp`, `src/kernel/sync/eventgroup.cpp`
**Date:** 2026-08-25
**Scope:** audit-scheduler-tasks-v0.4.2.md + scheduler_audit.md remediation:
H-1/H-2/H-3/H-4/M-1/M-2 + LOW items.

## FINDINGS

All findings are [S3] (style/hardening/notes). No S1 or S2 violations.

- **[S3]** scheduler.cpp `alloc_id()` / `alloc_id_err()`: return `UINT64_MAX`
  (TASK_INVALID sentinel) / `SCHED_ERR_TABLE_FULL` when
  `all_tasks_.size() >= MAX_TASKS`, using the same table-full condition as
  `add_task_err` — acceptable because `alloc_id()` runs *before* `add_task()`,
  preventing allocation when the table is full; monotonic no-reuse policy
  documented.
- **[S3]** `next_task_id_`: all accesses converted to `__atomic_fetch_add` /
  `__atomic_load_n` / `__atomic_store_n` (alloc_id, alloc_id_err,
  reset_next_task_id, capture_state next_id_out, restore_state next_task_id_).
  No plain `++`/assignment remains (H-2/H-4).
- **[S3]** scheduler.cpp `%u`/canary format sites: removed the 32-bit
  `static_cast<unsigned>(current->id)` truncation (upper bits garbage in the
  custom logger's 64-bit va_arg slot) at the two Logger::fatal canary sites
  (H-3).
- **[S3]** logger.cpp: length-modifier loop `while (*fmt == 'l' || *fmt == 'z')`
  — `%llu` previously left the va_arg list misaligned for a following `%s`,
  causing a crash when the selftest H2 flake tripped the
  `o1_scheduler_add_remove_ready_queue` diagnostic branch. Resolved at the
  formatter root (H-3).
- **[S3]** scheduler.hpp: `static_assert((ID_TABLE_SIZE & (ID_TABLE_SIZE-1))
  == 0)` (M-1); duplicate `#pragma once` removed (LOW).
- **[S3]** scheduler.cpp: duplicate `assert.hpp` include removed (LOW).
- **[S3]** ready_queue_manager.cpp: `diag_dumped` one-shot latch accessed
  atomically (task+ISR context) (M-2).
- **[S3]** `validate_iret_frame()` extraction is byte-identical to the two
  duplicated frame-validation lambdas; both frame orderings preserved
  (created: rip first via IRET_RIP_IDX/IRET_RSP_IDX; isr_common: ss first);
  harness boot-stack RSP allowance preserved; iret-frame offset constants
  named (IRET_RIP_IDX/CS_IDX/RFLAGS_IDX/RSP_IDX/SS_IDX) (LOW).
- **[S3]** `TASK_STACK_PTR` macro → inline `task_stack_ptr()` returning
  `uint64_t&`: `&task_stack_ptr(t)` still yields the address of the live TCB
  field (save-target usage); `task_stack_ptr(&next)` address-of-reference
  dereference correct; `_stack_start`/`_stack_end` externs moved from inside
  `namespace kernel` to global scope, all in-namespace references updated from
  `kernel::_stack_start` to `_stack_start` (LOW).
- **[S3]** semaphore.cpp / eventgroup.cpp: `bool added;` →
  `bool added = false;` (check-style init_required; behavior-neutral).

## POSITIVE VERIFICATIONS

- The deferred-switch hot path (switch_to_task, switch_away_from_terminating)
  logic is preserved exactly: frame validation semantics, boot-stack allowance,
  and the lvalue save-target write are unchanged.
- No ResourceTracker accounting change; no new dynamic allocation; no test
  files modified.

## GATES

scheduler_core 16/16, scheduler_o1 13/13, scheduler_sporadic 25/25,
task_lifecycle 9/9, scheduler_zombie 5/5, scheduler_preemption 11/11,
scheduler_atomic 6/6, scheduler_budget 6/6, scheduler_idle 11/11,
scheduler_cpu_load 5/5, ipc_core 23/23, ipc_blocking 4/4,
process_lifecycle 16/16, memory_resource_exhaustion 5/5, wcet_overrun 2/2,
wcet_scheduler 1/1, timing_core 18/18, deadline_miss 5/5, deadline_recovery
4/4, deadline_ss 3/3, deadline_action 1/1, selftest 133/133 ×5 (the `%llu`
logger fix resolved the flake-triggered o1 crash). check-style 0 errors.

## DECISION: APPROVED