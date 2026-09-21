# AUDIT REPORT 2026-08-25T14-30:00Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/sync/semaphore.cpp, src/kernel/sync/eventgroup.cpp, src/kernel/task/sporadic_server.cpp, src/kernel/task/sporadic_server.hpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_sporadic_server.cpp, src/kernel/test/test_sync_block_pattern.cpp (new), src/kernel/test/test_weak_stubs.cpp

## FINDINGS
- [S3] src/kernel/sync/semaphore.cpp:161 — Added `#include <kernel/arch/io.hpp>` for `arch::interrupts_enabled()` usage in C-1/C-2 spin-unwind paths
- [S3] src/kernel/sync/eventgroup.cpp:9 — Added `#include <kernel/arch/io.hpp>` for `arch::interrupts_enabled()` usage in H-1/M-6 spin-unwind paths
- [S3] src/kernel/task/sporadic_server.hpp:392-397 — Added `coalesce_count()` accessor and `coalesce_count_` member declaration for C-3 diagnostic
- [S3] src/kernel/test/test_registry.cpp:427,448,456 — Registered `register_sync_block_pattern_tests()` in three test-class registration scopes
- [S3] src/kernel/test/test_weak_stubs.cpp:844 — Added weak symbol `register_sync_block_pattern_tests()` for test framework completeness

**No S1 or S2 findings.**

### Concurrency boundaries (S1/S2 check)
All spinlock guards are correctly scoped: the `SpinLockGuard<SpinLock> guard(lock_)` is now confined to the state-inspection/waiter-insert critical section only. `Scheduler::dequeue_ready(*task)` and `Scheduler::reschedule()` are called **outside** the lock, mirroring the Notify/Queue pattern (queue.cpp:221-237). This eliminates the ISR-deadlock exposure where a timer tick while holding `lock_` across `reschedule()` would save the task holding the lock, and the ISR-side `post()` would spin forever on it.

- **C-1/C-2** (semaphore.cpp:wait/wait_err): Guard released before `dequeue_ready + reschedule`; interrupt-roll-back spin-wait provides INV-4 rollback correctness (if interrupts already enabled, spin-wait until wake; if disabled, `remove_waiter + state=RUNNING + enqueue_ready` cleanup).
- **H-1** (eventgroup.cpp:wait_bits/wait_bits_err): Identical pattern — lock released before dequeue/reschedule, with the same interrupt-roll-back discipline.
- **INV-2** (BLOCKED tasks never physically queued): `dequeue_ready(*task)` before `reschedule()` guarantees a BLOCKED task is never on the ready queue. The new tests `semaphore_wait_blocked_not_in_ready_queue` and `eventgroup_wait_bits_blocked_not_in_ready_queue` verify this.
- **INV-4** (interrupts-off path must fully restore state): Both wait() and wait_bits_err() have complementary else-branches that remove the waiter, restore `state=RUNNING`, and `enqueue_ready` when interrupts are disabled, ensuring no state corruption.

### INV-4 rollback correctness
The interrupt-roll-back pattern is consistent with the IPC::send() fix documented in earlier audits. When `arch::interrupts_enabled()` is true, the caller spin-waits until the timer ISR applies the deferred switch and a `post()` wakes the task. When interrupts are disabled, the state is fully rolled back (remove_waiter, state=RUNNING, enqueue_ready) before returning, satisfying INV-4.

### Priority-inheritance semantics (H-6)
- **H-6** (semaphore.cpp:170): `max_remaining >= holder_priority_` changed to `max_remaining > holder_priority_`. This ensures `holder_priority_` clears when the holder's boost equals the original priority, preventing a permanent priority-inflation latch across repeated lock cycles. The test `semaphore_pi_restore_clears_holder_latch` verifies owner priority returns to base 10 after post() with a waiter boosted to 15.

### No new dynamic allocation
No `new`, `malloc`, or heap allocations appear in any critical path. The retry loop in `wait()`/`wait_err()`/`wait_bits()`/`wait_bits()` iterates on `add_waiter()` failure (waiter table full), which is a bounded condition — the table drains as waiters are woken by `post()`/`set_bits()`. No ResourceTracker leak deltas are introduced.

### Test sanctity (doc-block + implementation updated together)
- **M-8** (sporadic_server.cpp:340, test_sporadic_server.cpp:480-482, 500-503): `process_replenishments` changed from `state != ACTIVE` to `state == EXHAUSTED`, so an IDLE server is never forced ACTIVE by a replenishment. The two existing tests (`sporadic_server_granularity_completion`, `sporadic_server_replenishment_partial`) had their doc-blocks and assertions updated together: assertions changed from `JARVIS_ASSERT(ss.state() == SporadicServer::ACTIVE)` to `JARVIS_ASSERT(ss.state() == SporadicServer::IDLE)`, with M-8 commentary explaining the correct behavior.
- **C-3** (sporadic_server.cpp:352-372): `schedule_replenishment` signature changed `bool→void`; ring-full now coalesces amount into newest pending entry instead of returning false (all call sites ignored the return → permanent budget loss). Added `coalesce_count_` counter + query. The new test `sporadic_replenishment_ring_full_coalesces` verifies coalesce_count() > 0 and that total budget restoration reaches the cap.
- All new regression tests (`test_sync_block_pattern.cpp`) were added in lockstep with the implementation fixes, verifying the corrected behavior rather than the old buggy behavior.

DECISION: APPROVED
