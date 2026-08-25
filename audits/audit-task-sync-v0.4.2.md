# NexIOS Kernel Audit — Scheduler / Task / Sync Subsystems

**Scope:** `src/kernel/task/{scheduler,task,task_queue,ready_queue_manager,sporadic_server,deadline_list}.cpp`, `src/kernel/sync/{spinlock,mutex,irq_spinlock_guard,queue,semaphore,eventgroup,notify,spsc_ring}.*`
**Date:** 2026-08-22 · **Baseline:** v0.4.2-dev (commit `624b9e5f`) · **Method:** static review against CODING_STYLE.md
**Reference standard:** freestanding C++20, MISRA/AUTOSAR ASIL-D posture, fully bounded loops, no heap on RT paths, ASSERT debug-only / ENSURE always-on.

## Executive Summary

The subsystem shows clear evidence of hardening-after-incidents: generation-tagged waiters, `in_ready_queue_` double-enqueue guards, `is_valid()` poison checks, extensive forensic comments referencing past wedges/GPFs/UAFs. However, several **structural concurrency defects remain**: two sync primitives hold a spinlock across `reschedule()` (the exact hazard `Notify::wait()` documents and avoids), one blocking path never dequeues the task from the ready queue, sporadic-server replenishments are silently dropped when the ring is full (permanent budget loss), queue-PIP boosts mutate priorities without ready-queue rebucketing, and `delete` is mixed with `MemPool::free` on the same TCBs. Debug-only diagnostics (`#ifdef CONFIG_DEBUG_IPC_SCHED`) also change failure behavior between builds — classic Heisenbug territory.

**Counts:** 3 CRITICAL · 7 HIGH · 9 MEDIUM · 8 LOW

---

## CRITICAL

### C-1 · Spinlock held across context switch in `Semaphore::wait()` / `wait_err()`
`src/kernel/sync/semaphore.cpp:169–188` (`wait`), `:193–214` (`wait_err`)
```cpp
SpinLockGuard<SpinLock> guard(lock_);
...
task->state = TaskState::BLOCKED;
Scheduler::reschedule();      // guard still holds lock_
```
`reschedule()` arms a *deferred* switch (INV-4); the task keeps running until the timer ISR applies it. The timer ISR enters the scheduler and takes `scheduler_lock_`; any ISR-side `post()` on this semaphore spins forever on `lock_`. `Notify::wait()` explicitly documents this exact deadlock ("never hold a spinlock across a context switch", notify.cpp:126–131) and releases the lock first — the semaphore was not given the same fix.
**Fix:** mirror `Notify::wait()`/`Mutex::lock()`: scope the guard around state inspection/waiter insertion, drop it, then set BLOCKED + reschedule.

### C-2 · Blocking semaphore wait never removes the task from the ready queue
`src/kernel/sync/semaphore.cpp:186–189`
`Semaphore::wait*()` sets `state = TaskState::BLOCKED` and calls `reschedule()` but — unlike every other blocking path (`Queue::send/receive`: `Scheduler::dequeue_ready(*task)`; mutex PCP path) — never dequeues the task from the ready queue. If the deferred switch applies late (or the scheduler's `next_task()` scans the runq), a BLOCKED task remains physically queued: precisely the "INV-2 desync" live-lock that scheduler.cpp:1330+ detects and halts on. The debug halt masks this in test builds; **release builds live-lock**.
**Fix:** call `Scheduler::dequeue_ready(*task)` before `reschedule()`, add the interrupts-disabled rollback branch used by `Queue`.

### C-3 · Sporadic Server permanently loses budget when the replenishment ring is full
`src/kernel/task/sporadic_server.cpp:158–170` (`schedule_replenishment`), called from `on_completion` (:55–60) and `consume` (:100–104)
`schedule_replenishment()` returns `false` when `replenishment_count_ == MAX_REPLENISHMENTS (8)`; **every call site ignores the return value**. The consumed amount is gone: `budget_remaining_` was decremented but no replenishment is queued. Under bursty aperiodic load the ring fills, budget monotonically drains to zero, the server drops to background priority and never recovers — unbounded starvation with no error signal (silent failure).
**Fix:** coalesce/merge the oldest replenishment (standard sporadic-server practice), or propagate the failure and treat ring-full as a deadline-miss event via the existing hook.

---

## HIGH

### H-1 · Spinlock held across context switch in `EventGroup::wait_bits()` / `wait_bits_err()`
`src/kernel/sync/eventgroup.cpp:166–190, 196–220` — identical pattern to C-1. Same ISR-deadlock exposure; also `bits_` is returned after waking without recheck. Fix identically to C-1.

### H-2 · `Notify` wakes REAPED tasks (UAF) — inconsistent dead-state filter
`src/kernel/sync/notify.cpp:31–41 (~Notify), 66–75 (notify), 84–90 (notify_err)`
These three sites check only `state != TERMINATED`, whereas every other primitive was hardened to reject both `TERMINATED` **and** `REAPED`. A reaped-and-recycled TCB passed to `Scheduler::set_task_ready()` corrupts the ready queue / is a use-after-free. `waiter_` carries no generation tag either.
**Fix:** apply the two-state filter everywhere in notify.cpp; store `waiter_gen_` alongside `waiter_`.

### H-3 · Queue PIP: raw `last_sender_`/`last_receiver_` pointers with no liveness/generation check
`src/kernel/sync/queue.cpp:356–404` (`boost_receiver/boost_sender/restore_*`)
Dereferences bare TCB pointers captured on previous operations. If the receiver terminated and its TCB was reaped/recycled, the boost writes a freed block's `priority` (memory corruption). Additionally, boosting `priority` directly never calls `ReadyQueueManager::move_priority()`, so a boosted task sitting in the ready queue stays in its old priority bucket — PI ineffective exactly when it matters.
**Fix:** generation-tag `last_sender_/last_receiver_`; route priority mutations through a scheduler helper that re-buckets queued tasks.

### H-4 · `Mutex::lock()` (void overload) panics on waiter-table-full — assert-masked DoS
`src/kernel/sync/mutex.cpp:243–246` vs. guarded `lock_err()` (:330–333)
`lock()` calls `add_waiter()` then `ENSURE(added)` with no `MAX_WAITERS` pre-check; under contention saturation this panics the kernel where `lock_err()` cleanly returns `SYNC_ERR_MAX_WAITERS`. Same issue in `EventGroup::wait_bits()` (eventgroup.cpp:171). Per CODING_STYLE §5, ENSURE is for impossible invariants — reachable resource exhaustion is not one.

### H-5 · Mixed `delete` / `MemPool::free` ownership on TCBs
`src/kernel/task/task.cpp:926, 936, 1006, 1127, 1157, 1167, 1367, 1461, 1478, 1503`; `src/kernel/task/scheduler.cpp:2988`
Error paths in `TaskControlBlock::create()` family `delete tcb` for objects allocated via `MemPool::alloc(sizeof(TaskControlBlock))`, and scheduler.cpp:2988 deletes a task whose teardown comments insist on `cleanup() + MemPool::free()`. Unless global `operator delete` routes to MemPool (not evidenced), this is a heap mismatch → corruption/double-free on exactly the OOM/error paths these guards exist for. CODING_STYLE §4 forbids `new/delete` on RT paths outright.

### H-6 · PI restore inconsistency: `>=` vs `>` leaves `holder_priority_` latched
`src/kernel/sync/semaphore.cpp:150–160` uses `max_remaining >= holder_priority_`; `mutex.cpp restore_priority()` uses `>`.
With equal priorities, `owner_->priority` is set to `max_remaining` and `holder_priority_` is never cleared → next `inherit_priority()` refuses to re-save, base priority permanently inflated — a slow priority-inversion leak across repeated lock cycles.

### H-7 · Deferred-switch window: `task->state = BLOCKED` written unlocked in `Queue::send/receive`
`src/kernel/sync/queue.cpp:203–226, 305–328, 420–443, 500–523`
After releasing `lock_`, the task writes `state = BLOCKED`, dequeues, reschedules. Between the unlocked store and the ISR applying the switch, the ISR can observe half-transitioned state. The interrupts-off rollback path additionally races the deferred switch: if the tick lands between rollback and re-enqueue, the task switches away as BLOCKED and never wakes.
**Fix:** perform the BLOCKED transition under `scheduler_lock_`; make rollback cancel the armed deferral or defer it to the tick handler.

---

## MEDIUM

- **M-1 · Unbounded spin waits** — mutex.cpp:222–225, 258–261, 307–310, 342–345: `while (state == BLOCKED) pause()` has no timeout/bound; lost wake = infinite spin at raised priority.
- **M-2 · `TaskQueue::pop_front()` discards whole queue on bad head node** — task_queue.cpp:47–56: invalid head zeroes head/tail/count, orphaning every remaining queued task (Heisenbug suppression instead of containment).
- **M-3 · `#ifdef CONFIG_DEBUG_IPC_SCHED` changes failure behavior** — scheduler.cpp:1203–1215, 1342–1355, 1806–1808, 2106, 2188, 3192: halt-on-detect detectors exist only in debug builds; release continues with corrupted state. Gate only the logging, never the detect-and-halt policy.
- **M-4 · Non-atomic one-shot diagnostic flag** — ready_queue_manager.cpp:18–27: `diag_dumped` checked/set from task+ISR context; surrounding diagnostic queue walk can be concurrent and unbounded.
- **M-5 · `SPSCRing` mixes plain and atomic accesses on same indices** — spsc_ring.hpp:33,46,70–76: data race (UB) under -O3; `reset()` writes head/tail as unsynchronized pair while active. Use atomics on all accesses; document reset as quiescent-only.
- **M-6 · `EventGroup::try_wait_bits` reads `bits_` unlocked** — eventgroup.cpp:245–256: answer may be stale immediately; take the spinlock or document the racy contract.
- **M-7 · SporadicServer forces ACTIVE even when idle** — sporadic_server.cpp:128–140: idle-but-ACTIVE server blocks `on_activation()` (:38 requires IDLE), delaying the next aperiodic job.
- **M-8 · Granularity accounting skews consumption timeline** — sporadic_server.cpp:83–92: with `budget_granularity_ > 1`, replenishment times aren't anchored to actual consumption points; utilization bound no longer guaranteed.
- **M-9 · Init-detection heuristics misfire** — Mutex::init never sets `initialized_` (double-init possible); Queue::init_err treats any nonzero counter as initialized; Semaphore::init_err keys on default value. One explicit `initialized_` flag per primitive, honored by every init variant.

---

## LOW

- **L-1** SpinLock: no owner/recursion validation; `holder_` written RELAXED after acquisition (spinlock.hpp:37–58).
- **L-2** IrqSpinLockGuard re-lock resamples IRQ state; no pairing assertion (irq_spinlock_guard.hpp:50–57).
- **L-3** IrqGuard unconditionally `sti()`s on restore-if-enabled; stray `sti` inside scope silently promoted (irq_guard.hpp:37–45).
- **L-4** DeadlineList O(n) with no iteration bound or cycle guard — corrupted `dl_next_` cycles forever in tick context (deadline_list.cpp:15–49,62–88).
- **L-5** ReadyQueueManager::restore_pod drop-path clears flags on nodes reached through possibly dangling chains (ready_queue_manager.cpp:139–150).
- **L-6** `dequeue_highest` relies on priority 0 being reserved — encode reservation as constant/assert (ready_queue_manager.cpp:52–63).
- **L-7** Test-mode scheduler divergence — `is_test_active()` disables deadline-monitor wake and periodic `reap_orphans()` during tests, so tests never exercise reaper races production hits (scheduler.cpp:1348–1356).
- **L-8** SporadicServer::dispose debug-only invariant: missing decrement not logged in release builds (sporadic_server.cpp:196–206).

---

## Positive Observations

- Generation-tagged waiter arrays (mutex/queue/semaphore/eventgroup) are a solid recycled-TCB defense, consistently applied except in Notify (H-2).
- ReadyQueueManager's authoritative `in_ready_queue_` flag design and double-enqueue refusal reflect genuine root-caused fixes.
- `Notify::wait()` is the model implementation of release-lock-before-reschedule — should be the template for Semaphore (C-1) and EventGroup (H-1).
- `TaskQueue::remove()` correctly treats intrusive links as source of truth.
- `Mutex::unlock()`'s direct-ownership-transfer ordering is correct against the classic PI-restore bug.

## Recommended Fix Order

1. **C-1/C-2/H-1** (semaphore + eventgroup blocking paths) — small, mechanical, removes two live-lock/deadlock classes.
2. **C-3 + M-7/M-8** (sporadic server budget integrity).
3. **H-2/H-3** (UAF-class defects in Notify and Queue PIP).
4. **H-5** (ownership unification delete→MemPool).
5. **H-4/M-1/M-9** (error-path hygiene and init flags).

## Audit Limitation

scheduler.cpp (3,605 lines) and task.cpp (1,887 lines) were audited via targeted pattern extraction rather than full line-by-line reading. Findings there are anchored to verified line numbers, but a follow-up deep pass on those two files could surface additional issues.
