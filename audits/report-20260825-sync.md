# AUDIT REPORT 2026-08-25T14:09:40Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/sync/mutex.cpp, src/kernel/task/task_queue.cpp, src/kernel/sync/spsc_ring.hpp

## FINDINGS
- [S3] src/kernel/sync/mutex.cpp:244-248 — H-4: void lock() ENSURE replaced with retry-with-reschedule on waiter-table-full. Release lock, reschedule, re-lock, continue bounded retry loop. A void blocking API must not panic on reachable resource exhaustion.
  WHY: The ENSURE would panic on a reachable exhaustion condition. The retry-with-reschedule pattern yields to other waiters, draining the table, and is bounded by the outer MAX_WAITERS+1 loop. The lock_err() path retains its ENSURE with a pre-check making it genuinely impossible.
- [S3] src/kernel/task/task_queue.cpp:41-57 — M-2: pop_front() corrupted-head drop-or-phony replaced with orphan-drop preserving remaining queue. Advances head_/tail_/count_ consistently instead of zeroing the whole list.
  WHY: The old code zeroed head_/tail_/count_ on any corrupted head, orphanating every other queued task. The new code drops only the offending node and preserves the remaining queue with correct invariants.
- [S3] src/kernel/sync/spsc_ring.hpp:58 — M-5: try_push's head_ read and try_pop's tail_ read converted from plain reads to `kernel::atomic_load(..., __ATOMIC_RELAXED)`. Mixed plain/atomic access on same indices was a data race / UB under -O3. reset() documented QUIESCENT-ONLY.
  WHY: Plain reads on indices written by the other side constitute a real data race under optimizing compilation. The relaxed+acquire pattern (relaxed on own index, acquire on opposite) is the correct SPSC contract.
- [S3] src/kernel/sync/mutex.cpp:52 — M-9: void init() now sets `initialized_ = true` so a subsequent init_err() correctly reports ALREADY_INITIALIZED. Was never set by the void overload, making double-init possible.
  WHY: Without the flag, init_err() could not detect already-initialized state after void init(), risking double-initialization of kernel resources.

## PATCH
audits/rejected_patch.diff was not written; all changes pass audit verification.

## DECISION: APPROVED
