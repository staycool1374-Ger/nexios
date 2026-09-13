# AUDIT REPORT 2026-09-13T09-47-59Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/sync/notify.cpp, src/kernel/syscall/syscall_handlers_sync.cpp, src/kernel/test/test_cap_irq_notify.cpp, src/kernel/test/test_kernel_top.cpp

## FINDINGS

- [S3] src/kernel/sync/notify.cpp:wait() retry loop — foreign-waiter path spins reschedule()+pause() unboundedly until waiter_ clears
  WHY: Scheduler-mediated yield each iteration (never lock-held) and identical to the Semaphore::wait() table-full retry precedent, so no safety violation; noted only as a liveness shape inherited from the reference pattern.
- [S3] src/kernel/sync/notify.cpp:wait_err() returns SYNC_ERR_ALREADY_WAITING before the fast-path consume, asymmetric with wait()'s yield-and-retry
  WHY: Per-overload contract (err variant must report contention rather than block) and a pending-value-with-registered-waiter state is unreachable via notify()/try_wait() paths, so the asymmetry is intentional, not a defect.

DECISION: APPROVED
