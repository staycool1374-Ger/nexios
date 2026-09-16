# AUDIT REPORT 20260916T185611Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/ipc/ipc.cpp, src/kernel/ipc/ipc.hpp, src/kernel/syscall/syscall_handlers_ipc.cpp, src/kernel/task/scheduler.cpp, src/kernel/task/scheduler.hpp, src/kernel/task/task.cpp, src/kernel/task/task.hpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_ipc_timeout.cpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_weak_stubs.cpp

## FINDINGS
- [S2] src/kernel/task/scheduler.cpp:2032 — level-triggered tail calls enqueue_ready() on any state after the BLOCKED check, so a waiter dispatched to RUNNING (slot still armed until its resume path runs recv_wait_cancel) gets queued while executing.
  WHY: enqueue_ready() refuses only duplicates (in_ready_queue_) and id_table_-orphans, never RUNNING tasks, while next_task() explicitly dispatches RUNNING candidates and set_current() documents current-never-queued as a wedge-causing invariant.
- [S3] src/kernel/ipc/ipc.cpp:677 — recv_timed_out is a plain bool written in tick-callback context and read in task resume paths and the scheduler tail, unlike need_resched which uses __atomic.
  WHY: Byte access is benign on x86-64 but the cross-context flag lacks the codebase's own atomic precedent for tick-observed state.
- [S3] src/kernel/task/scheduler.hpp:509 — TaskFields comment claims the wheel handle rewinds to gen-0-invalid, but handle/generation are not captured, so rewind safety rests solely on snapshot_reset() bumping wheel generations.
  WHY: A mid-wait rewind (armed=true, timed_out=false, dead wheel entry) would strand with no waker, and the comment misdirects future maintainers about where the guarantee lives.
- [S3] src/kernel/ipc/ipc.cpp:712 — recv_wait_arm overwrites recv_timeout_handle without cancelling a possibly live prior arm, leaking the old slot until its expiry fires against a matching generation.
  WHY: Unreachable under today's single-arm discipline plus cleanup cancel, but a defensive cancel-first would close the stale-fire window by construction.
- [S3] src/kernel/syscall/syscall_handlers_ipc.cpp:114 — CONFIG_TICK_HZ==0 makes arm fail closed into a RUNNING-poll fallback whose frozen-ticks deadline can never expire.
  WHY: The degenerate no-tick configuration hangs the fallback instead of failing fast with -1 (the wheel path is equally dead there, so impact is confined to that config).

## PATCH
```diff
diff --git a/src/kernel/task/scheduler.cpp b/src/kernel/task/scheduler.cpp
--- a/src/kernel/task/scheduler.cpp
+++ b/src/kernel/task/scheduler.cpp
@@ -2029,6 +2029,13 @@
                 continue;
             if (task->state == TaskState::BLOCKED)
                 task->state = TaskState::READY;
+            // SIL 3 guard (audit issue #18): never queue a task that is not
+            // BLOCKED-turned-READY or already READY. A dispatched waiter is
+            // RUNNING with its slot still armed until its resume path runs
+            // recv_wait_cancel; queueing it violates the
+            // current-never-queued invariant (set_current defense, H2).
+            else if (task->state != TaskState::READY)
+                continue;
             enqueue_ready(*task);
             __atomic_store_n(&Scheduler::SwSlots::need_resched(), true,
                              __ATOMIC_RELEASE);
```

DECISION: REJECTED
