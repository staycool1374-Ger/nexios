# ZombieList — Idle-Task Deferred Resource Cleanup

- **Status:** IMPLEMENTED — idle-task deferred cleanup via
  `Scheduler::drain_zombie_list()` (`src/kernel/task/scheduler_lifecycle.cpp`),
  reached from the `on_tick` reaper path; canonical teardown sequence
  `cleanup() → remove_task()/unregister_task() → MemPool::free()` (see
  `specs/scheduler.md` §6 D5/D6).
- **Target:** v0.3.5 (Idle-Task Deferred Resource Cleanup)
- **Design Doc:** This file (retained as the design record; §1 motivation
  below describes the pre-implementation state)

## 1. Motivation

The current `reap_orphans()` runs inside `on_tick()` (timer ISR context) every 100
ticks.  This has several problems:

1. **ISR-context deallocation** — `MemPool::free()`, `VMM::free_user_pages()`,
   IPC destructors, and FD close are non-deterministic and may block; running
   them from a timer interrupt violates bounded ISR latency.
2. **O(n) scan** — `reap_orphans()` iterates the entire `all_tasks_` registry
   every 100 ticks even when no tasks have terminated.
3. **UAF race with test harness** — the reaper frees terminated test tasks
   before the test's `ScopeGuard` can run, requiring `s_test_active_` as a
   band-aid.

### Design Goals

- Deallocate terminated task resources **only during idle cycles**, never in ISR.
- O(1) termination enqueue, O(1) per-step dequeue.
- Deterministic per-step cycle budget so that cleanup does not delay RT tasks.
- Synchronous drain for snapshot/reboot/reload paths.
- Safety watchdog guards against idle-task starvation.

## 2. Data Structures

### 2.1 TCB field (`src/kernel/task/task.hpp`)

```cpp
/// @brief Singly-linked list pointer for the zombie list.
/// Non-null only while the TCB is in the zombie list (between release_zombie
/// and idle cleanup_step).  Set to nullptr on enqueue and on dequeue.
TaskControlBlock *zombie_next_ = nullptr;
```

Add in the pointer section alongside `runq_next_` / `dl_next_` / `pri_next_` /
`blocked_next`.

### 2.2 Scheduler members (`src/kernel/task/scheduler.hpp`)

```cpp
/// @brief Head of the intrusive singly-linked zombie list.
static TaskControlBlock *zombie_head_;

/// @brief Tail of the zombie list (O(1) append).
static TaskControlBlock *zombie_tail_;

/// @brief Approximate number of zombies currently in the list.
/// Updated atomically on push/pop; used by the watchdog to decide
/// whether a force-flush is needed.
static uint64_t zombie_count_;
```

Config constant (in `jarvis_config.h` or a new config header):

```cpp
/// @brief Maximum zombies allowed before the tick watchdog force-flushes.
/// 0 disables the watchdog (idle-only cleanup).  Default 32.
#define CONFIG_ZOMBIE_STARVATION_LIMIT 32

/// @brief Maximum cycles (arbitrary cheap iterations) the idle cleanup step
/// may consume before yielding via reschedule().  Default 1000.
#define CONFIG_CLEANUP_MAX_CYCLES_PER_STEP 1000
```

## 3. Core Functions

### 3.1 `Scheduler::release_zombie(TaskControlBlock &task)` — new

Called from `terminate()` for any task that is NOT the current task, and from
the self-termination path just before switching away.

**Preconditions:**
- `scheduler_lock_` is held (caller context).
- `task.state == TaskState::TERMINATED` (set by caller).
- Task has already been removed from the ready queue.
- `wake_waiting_parent(task)` has already been called.

**Operations (O(1) each):**

1. `ENSURE(!task.in_ready_queue_)` — invariant: a zombie must never be in the
   ready queue.  `terminate()` calls `dequeue_ready(task)` before this point.
2. `deadline_list_.remove(&task)` — remove from deadline list.
3. `all_tasks_.remove(&task)` — remove from global task registry.
4. `id_table_remove(&task)` — remove from ID lookup table.
5. `task.zombie_next_ = nullptr`.
6. Tail-append to zombie list:
   ```
   if (zombie_tail_)
       zombie_tail_->zombie_next_ = &task;
   else
       zombie_head_ = &task;
   zombie_tail_ = &task;
   ```
7. `__atomic_add_fetch(&zombie_count_, 1, __ATOMIC_RELAXED)`.

After `release_zombie` returns, the scheduler **never touches this TCB again**
(exception: watchdog or idle `cleanup_step` which pop and free it).  The TCB
is live only in the zombie list.

### 3.2 `Scheduler::terminate()` — refactored

```cpp
void Scheduler::terminate(TaskControlBlock &task, uint64_t exit_code) noexcept {
    dequeue_ready(task);
    task.state = TaskState::TERMINATED;
    task.exit_code = exit_code;
    wake_waiting_parent(task);

    if (&task != current_task_ptr_) {
        // Non-self termination: enqueue and let idle clean up.
        release_zombie(task);
        return;
    }

    // Self-termination: we must switch away, but we can still call
    // release_zombie first because we only remove from tables — we do NOT
    // call cleanup() yet (that would free our own stack).
    release_zombie(task);

    // Switch away.  After this, the terminated task is just a zombie
    // in the list.  Its kernel stack is still valid until cleanup_step
    // calls cleanup() && MemPool::free().
    switch_away_from_terminating(task);
}
```

### 3.3 `IdleTask::cleanup_step()` — new

Called from the idle main loop once per iteration.

```cpp
void IdleTask::cleanup_step() noexcept {
    // Pop one zombie under IRQ-disable so on_tick watchdog cannot race.
    TaskControlBlock *task;
    {
        arch::IrqGuard irq_guard{};
        task = zombie_head_;
        if (task) {
            zombie_head_ = task->zombie_next_;
            if (!zombie_head_)
                zombie_tail_ = nullptr;
            task->zombie_next_ = nullptr;
            ENSURE(!task->in_ready_queue_);  // zombie must never be in RQ
            __atomic_sub_fetch(&zombie_count_, 1, __ATOMIC_RELAXED);
        }
    }
    if (!task)
        return;

    // Free resources.  IRQs are enabled — on_tick may fire, but it will only
    // access zombie_head_ (which no longer points to task) or call
    // release_zombie on a different task (which holds scheduler_lock_ and
    // appends to tail).  Both are safe concurrently.
    task->cleanup();
    MemPool::free(task);

    // Cycle-budget check: reschedule if we've spent too long.
    // For now, one TCB per step is the simplest possible bound.
    // Future: multi-TCB flush with a cycle counter.
}
```

### 3.4 Idle main loop — updated

Current (`src/kernel/memory/integrity.cpp`):

```cpp
void idle_task_main() {
    // ... init ...
    for (uint64_t _i = 0; _i < UINT64_MAX; ++_i) {
        check_section_markers();
        crc_process_chunk();
        arch::hlt();
    }
}
```

New:

```cpp
void idle_task_main() {
    // ... init ...
    for (uint64_t _i = 0; _i < UINT64_MAX; ++_i) {
        IdleTask::cleanup_step();
        check_section_markers();
        crc_process_chunk();
        arch::hlt();
    }
}
```

The `hlt()` at the end lets the CPU sleep if no zombies and no integrity work
is pending.  If `cleanup_step()` frees a zombie, the next timer tick may
preempt idle for a newly-READY task (the zombie's parent woken by
`wake_waiting_parent`).  This is correct — idle yields immediately.

## 4. Safety Watchdog (`on_tick` replacement)

**STATUS: IMPLEMENTED** — the watchdog runs every 100 ticks inside `on_tick`'s
gated tail (`if (lock_acquired) { arch::IrqGuard ... }`):

```cpp
// Safety watchdog: if the zombie list has grown beyond the starvation
// limit, idle hasn't gotten enough CPU.  Force-drain a batch inline.
// Runs inside the on_tick tail gate (lock_acquired && IrqGuard), which
// satisfies the "hold scheduler_lock_ or IRQs disabled" precondition.
#if CONFIG_ZOMBIE_STARVATION_LIMIT > 0
if (tick_counter % 100 == 0) {
    uint64_t zcount = __atomic_load_n(&zombie_count_, __ATOMIC_RELAXED);
    if (zcount > CONFIG_ZOMBIE_STARVATION_LIMIT) {
        flush_zombies(CONFIG_ZOMBIE_STARVATION_LIMIT / 2);
    }
}
#endif
```

> **Note on the gate:** `on_tick()` is invoked from the timer ISR (IRQs off)
> **and** from task context (tests). The `flush_zombies`/`reap_orphans`/
> `process_deferred_kills`/sporadic-block tail must not race a concurrent
> `scheduler_lock_` holder's partial writes, so it is gated on
> `lock_acquired` and additionally wrapped in `arch::IrqGuard`. When the lock
> is contended, the watchdog batch is deferred to the next uncontended tick
> (a 100-tick cadence makes this harmless).

### 4.1 `flush_zombies(uint64_t max_flush)` — new

Drains up to `max_flush` zombies from the list.  Used by the watchdog, by
`drain_zombie_list()` (which passes UINT64_MAX), and by pre-snapshot
cleanup.

```cpp
void Scheduler::flush_zombies(uint64_t max_flush) noexcept {
    // Must hold scheduler_lock_ (or IRQs disabled).
    for (uint64_t i = 0; i < max_flush; ++i) {
        TaskControlBlock *task = zombie_head_;
        if (!task)
            break;
        zombie_head_ = task->zombie_next_;
        if (!zombie_head_)
            zombie_tail_ = nullptr;
        task->zombie_next_ = nullptr;
        ENSURE(!task->in_ready_queue_);  // zombie must never be in RQ
        __atomic_sub_fetch(&zombie_count_, 1, __ATOMIC_RELAXED);

        task->cleanup();
        MemPool::free(task);
    }
}
```

### 4.2 `drain_zombie_list()` — new (convenience wrapper)

```cpp
void Scheduler::drain_zombie_list() noexcept {
    IrqGuard irq_guard{};
    SpinLockGuard guard(scheduler_lock_);
    flush_zombies(UINT64_MAX);
}
```

## 5. Snapshots and Restore

### 5.1 Pre-snapshot (`test_isolate.cpp`)

Before saving scheduler state, drain all zombies:

```cpp
// In snapshot_restore(), before restoring state:
Scheduler::drain_zombie_list();
```

This ensures no TCB memory is leaked between test runs.

### 5.2 Post-restore

After `Scheduler::restore_state()`:

```cpp
zombie_head_ = nullptr;
zombie_tail_ = nullptr;
zombie_count_ = 0;
```

The zombie list is always empty after restore because no tasks are in it
(they were either re-created or never existed).

## 6. Removal / Deprecation

### 6.1 `reap_orphans()`

- Demoted to a static helper called only by the watchdog (on_tick).
- No longer iterates `all_tasks_`; the watchdog uses `flush_zombies()` instead.
- Can be removed entirely after all callers are migrated.

### 6.2 `cleanup_zombies()`

Currently has **zero call sites** in the kernel.  Remove entirely.

### 6.3 `s_reap_in_progress`

No longer needed.  The zombie list is non-recursive (push and pop are O(1)
and never nest).  Remove the variable and all checks.

### 6.4 `s_test_active_` reaper guard

The `s_test_active_` guard around `reap_orphans()` (line 1133) can be removed
because the reaper no longer runs during tests — zombie cleanup is deferred to
idle, and idle does not run during test execution (the harness task has higher
priority).  Zombies are drained by `drain_zombie_list()` in
`snapshot_restore()`.  The other uses of `s_test_active_` (deadline-monitor
wake, harness_nonpreempt) remain unchanged.

## 7. Caveats and Side-Effects

### 7.1 Synchronous-drain callers

The following code paths previously relied on `reap_orphans()` for synchronous
cleanup and must be audited to call either `drain_zombie_list()` or
`flush_zombies()` instead:

| Caller | File | Action |
|--------|------|--------|
| `cleanup_test_tasks()` | `scheduler.cpp` | Must drain before force-remove pass |
| `reboot_from_table()` | `taskdefs.cpp` | Must drain before recreating daemons |
| `kill_test_tasks()` | `test_isolate.cpp` | Must drain before snapshot restore |
| `test_idle_task.cpp` teardown | — | Replace `reap_orphans()` with `drain_zombie_list()` |
| `test_waitpid.cpp` teardown | — | Same |
| `test_iocd.cpp` teardown | — | Same |
| `test_vfsd.cpp` teardown | — | Same |
| `test_task_lifecycle.cpp` teardown | — | Same |
| `test_scheduler.cpp` teardown | — | Same |
| `test_cleanup.cpp` teardown | — | Same |

### 7.2 Test teardowns using `delete tcb`

~30 test ScopeGuards use `delete tcb` which calls TCB::operator delete →
`cleanup()` + `remove_task()` + `MemPool::free()`.  If the task is already
in the zombie list, calling `delete` would double-free (the zombie head/tail
pointers still alias the TCB).  These tests must use `Scheduler::terminate()`
+ `drain_zombie_list()` instead, or call `remove_task()` before `delete`.

### 7.3 Self-termination stack validity

When a task calls `Scheduler::terminate(self, code)`, `release_zombie()` is
called BEFORE `switch_away_from_terminating()`.  At that point the task is
removed from `all_tasks_` / `id_table_` / `deadline_list_`, but its **kernel
stack and TCB memory are still valid** — `cleanup()` has NOT been called.
The context switch away is safe because the stack is still mapped.  After
the switch, the zombie is in the list, and idle will eventually call
`cleanup()` + `MemPool::free()`.  This is safe.

### 7.4 Direct pointer to terminated TCB

Tests that hold a direct `TaskControlBlock *` pointer to a terminated child
and busy-wait on `ptr->state != TaskState::TERMINATED` are safe because:
- Idle runs at priority 0 and cannot preempt the RUNNING test task.
- The zombie list is only drained by idle or by `on_tick` watchdog.
- The watchdog runs only every 100 ticks and only if `zombie_count_ > LIMIT`
  (which won't happen with 1-2 terminated tasks).

### 7.5 Race between idle cleanup_step and on_tick watchdog

`cleanup_step()` pops a zombie with IRQs disabled, then calls `cleanup()` /
`MemPool::free()` with IRQs enabled.  The watchdog in `on_tick()` runs inside
the gated tail (`if (lock_acquired) { arch::IrqGuard ... }`).  Both pop from
`zombie_head_`.

- While idle has IRQs disabled: watchdog cannot fire (IRQs masked).
- After idle re-enables IRQs: the popped task is no longer in the list.
  Watchdog pops a different task — no conflict.
- If both attempt to pop concurrently (watchdog fires between idle's pop
  and its next check): they pop different tasks because each pop advances
  `zombie_head_` atomically under their respective lock/IRQ-disabled
  region.

**No data race exists.**

> **`cleanup_step()` magic-guard (RACE-FIXED):** `cleanup_step()` validates
> `task->magic == TCB_MAGIC` BEFORE dereferencing any field past offset 0
> (the head TCB may have been freed/poisoned).  On a bad-magic head it resets
> the entire list (`head`/`tail`/`count = 0`) rather than popping a poisoned
> node, matching `flush_zombies`/`drain_zombie_list`.  Cleanup and free run
> with IRQs enabled, outside the pop's `IrqGuard`.

### 7.6 `process_deferred_kills()` (deadline-miss KILL)

Currently calls `delete task` on the killed task.  With the ZombieList, the
KILL handler should set TERMINATED → `wake_waiting_parent` → `release_zombie`
instead.  The deferred-kill queue is no longer needed because the zombie list
handles deferred cleanup.  `process_deferred_kills()` can be simplified.

**Note:** `process_deferred_kills()` calls `delete` → `TaskControlBlock::operator delete`
→ `cleanup()` + `Scheduler::remove_task()`, which re-acquires
`scheduler_lock_`.  It must therefore run OUTSIDE the lock (in `on_tick`'s
gated tail, protected by `IrqGuard` + the `lock_acquired` gate only).

### 7.7 Zombie list memory overhead

Each zombie TCB occupies a full MemPool block (8KB pool 8) until cleaned up.
With `CONFIG_MAX_TASKS = 64`, the worst-case zombie accumulation is 64 × 8KB
= 512 KB.  Acceptable.

### 7.8 `child->parent_id = 0` in `wake_waiting_parent`

Already called during `terminate()` before `release_zombie()`.  When
`cleanup_step()` later calls `task->cleanup()`, the `parent_id == 0` check
in `TaskControlBlock::cleanup()` skips the parent-child removal (correct —
already done).

### 7.9 s_test_active_ unused for reaper after migration

After removing the `s_test_active_` guard from the on_tick reaper path, the
flag remains for:
- Deadline-monitor wake suppression (line 965).
- Harness-nonpreempt logic (line 1847).

These are unrelated to reaping and are unchanged.

## 8. Migration Plan

### Phase 1 — Core Implementation
1. Add `zombie_next_` to TCB + `zombie_head_`/`zombie_tail_`/`zombie_count_`
   to Scheduler.
2. Implement `release_zombie()`.
3. Refactor `terminate()` to call `release_zombie()`.
4. Implement `flush_zombies()` and `drain_zombie_list()`.
5. Implement `IdleTask::cleanup_step()`.
6. Update idle main loop.

### Phase 2 — Caller Migration
7. Replace `reap_orphans()` calls in:
   - `cleanup_test_tasks()` → pre-drain + inline flush.
   - `reboot_from_table()` → pre-drain.
   - `kill_test_tasks()` → pre-drain.
   - All test teardowns → `drain_zombie_list()`.
8. Update `on_tick()` watchdog.

### Phase 3 — Cleanup
9. Remove `s_reap_in_progress`.
10. Remove `cleanup_zombies()`.
11. Remove `s_test_active_` reaper guard.
12. Simplify `process_deferred_kills()`.

### Phase 4 — Tests
13. `test_idle_cleanup_simple`.
14. `test_idle_cleanup_no_deadline_impact`.
15. `test_zombie_starvation_watchdog`.

## 9. Test Plan

| Test Class | Test | Description |
|------------|------|-------------|
| `idle_cleanup` | `test_idle_cleanup_simple` | Create+terminate 3 tasks. Spin until idle has run (check zombie_count reaches 0). Verify each task's resources freed (PPM page count returns). |
| `idle_cleanup` | `test_idle_cleanup_no_deadline_impact` | Create RT workload (4 periodic tasks at priority 10). Terminate a large task (with many FDs, user pages). Verify no RT task misses a deadline during cleanup. |
| `scheduler` | `test_zombie_starvation_watchdog` | Create 33+ tasks, terminate all, prevent idle from running (busy-loop). Verify watchdog force-flush within 100 ticks. |
| `scheduler` | `test_release_zombie_self` | Self-terminating task reaches zombie list, idle cleans up. |
| `scheduler` | `test_release_zombie_other` | One task terminates another, zombie cleaned up by idle. |
| `test_isolate` | snapshot_restore | Verify no zombie leaks across test cycles (existing ResourceTracker check catches this). |

## 10. Configuration

| Constant | Default | Description |
|----------|---------|-------------|
| `CONFIG_ZOMBIE_STARVATION_LIMIT` | 32 | Max zombie count before on_tick force-flush. 0 = disable watchdog. |
| `CONFIG_CLEANUP_MAX_CYCLES_PER_STEP` | 1000 | Budget cycles for a single idle cleanup step (currently 1 TCB per step, future: multi-TCB with cycle counter). |
