# Kernel Test Harness & Test-Isolation Specification

**Semantics:** binding contract for writing and running kernel tests: the
driven-test discipline, the snapshot isolation mechanism, the ResourceTracker
leak detector, the registry/class system, and the watchdog/QEMU verdict
protocol.  All file:line anchors verified against the current tree.

```
 Makefile execute-test <arch> <build> <class>
   │ write class → initrd/tests/test-config.txt; build ISO; launch QEMU+expect
   ▼
 kernel_init → Registry::init → parse_test_config → register_class(name)
   ▼
 init_task_main (PID 1, prio raised to 10) → run_registered(0)
   ▼
 run_filtered(0, use_isolation=true)
   ├─ Registry::set_expected_count (PLANNED)
   ├─ Scheduler::ensure_monitor()
   ├─ snapshot_create()                    ← ONE full-kernel snapshot
   ├─ set_test_active(true)
   ├─ loop per test:
   │    watchdog_arm(30000, name)
   │    run_one(tc, class, n, total)
   │      ├─ capture before_rsrc
   │      ├─ tc.factory()->execute() / tc.func()   ← TEST BODY (driven)
   │      ├─ capture after_rsrc; passed = failed()==before_fail
   │      └─ "S: class suite::name n/total: PASS|FAIL"
   │    watchdog_disarm
   │    snapshot_restore("suite::name")     ← per-test isolation
   ├─ snapshot_destroy(); reload_daemon_tasks()
   └─ print_report() → TEST SUMMARY → shutdown_kernel(result)
   ▼
 expect regexes TEST SUMMARY: PASS iff PLANNED==EXECUTED && FAILED==0
```

## 1. The Driven-Test Discipline (INV-4 / trigger-driven cookbook)

**Why:** the scheduler defers every switch to the timer-ISR epilogue
(INV-2/INV-4).  `Semaphore::wait()`/IPC block set `state=BLOCKED` and return
immediately; the physical switch happens later.  A test can therefore not
observe a block inline — it must **spin on observed task state** until the ISR
has applied the switch.  The timer ISR also fires between arbitrary
instructions, so a half-arranged scenario can be dispatched prematurely.

### Mandatory rules (each stopped a real hang)
1. **Create both TCBs first**, set every field/queue, *then* register either.
2. **Register cooperating tasks under ONE `arch::IrqGuard`** (no tick can split
   the registration); the higher-priority task that must BLOCK runs first.
3. **Wait on observed state:** `while (t->state != BLOCKED) asm volatile("pause");`.
4. **Reclaim via `Scheduler::drain_zombie_list()`** — `terminate()` only marks
   TERMINATED + moves to the zombie list; never follow with manual free.
5. **Cleanup BEFORE assert** (assertions `return` on failure — asserting first
   leaks the TCB / hangs the class).
6. **Externally terminating a task blocked in `Semaphore::wait()` is SAFE
   since v0.3.12** — `TaskControlBlock::cleanup()` unlinks the task from the
   semaphore's waiter list via the `waiting_on_semaphore` back-pointer
   (v0.3.9 teardown gap closed).  Verify teardown with
   `semaphore_waiter_teardown_on_terminate`; do not re-add the old workaround
   (avoiding the scenario instead of testing it).

### Gate-blocked-lambda + post-wait BLOCKED spin (canonical pattern)
```cpp
static void overrun_then_block_body() {
    sync::Semaphore *gate = (sync::Semaphore*)Scheduler::current_task()->user_data;
    ... /* real work (e.g. busy-wait 40 real ticks = genuine overrun) */
    gate->wait();                                   // sets BLOCKED, returns (INV-4)
    while (Scheduler::current_task()->state == TaskState::BLOCKED)
        asm volatile("pause");                      // stay live until harness posts
}
// harness:
auto *t = TaskControlBlock::create(overrun_then_block_body, 11, 2);
t->user_data = &gate;
Scheduler::add_task(*t);
Scheduler::reschedule();
while (t->state != TaskState::BLOCKED) asm volatile("pause");
... assert on post-block state ...
gate.post();
while (t->state != TaskState::TERMINATED) asm volatile("pause");
Scheduler::drain_zombie_list();
```

### Scheduler test helpers (`test_sched_helpers.hpp`)
| Helper | Contract |
|---|---|
| `yield_as(t)` | IrqGuard + set_current + reschedule (atomic, ISR-proof) |
| `yield_to_task(t)` | steers next_task without dequeueing peer; **dequeues the harness** (a current task must never be queued) |
| `ScopedCurrentTask(t)` | RAII set-current; **no blocking ops inside** |
| `create_forever_task(prio,period,name)` | forever-loop task; must be `terminate_and_drain()`'d |
| `terminate_and_drain(t)` | terminate + drain_zombie_list |
| `wait_for_termination(t)` | spin `arch::hlt()` until TERMINATED |
| `trigger_deadline_monitor_scan()` | runs `scan_deadlines()` synchronously (not the fragile monitor wake) |
| `create_test_task(prio,period)` | registers a BLOCKED (non-runnable) TCB — the **only** sanctioned impersonation surface |

## 2. Snapshot Isolation

### 2.1 Contract
- `snapshot_create()`: ONE full-kernel snapshot before the suite; `guard +
  buffer + guard` contiguous PMM frames.
- `snapshot_restore(test_name)`: runs after every test, restores the pre-suite
  state (per-test isolation).
- `snapshot_destroy()`: frees the frames at suite end.

### 2.2 Captured state (layout)
PMM bitmap+owner+free-count · TCB pinning · MemPool PoolMeta+data · scheduler
tasks/id_table/misc/ticks · TaskFields (state, priorities, deadline/executed/
remaining ticks, TaskContext incl. rsp, kstack base/top, RQ pointers +
`in_ready_queue_` + `rq_priority_`) · ReadyQueuePOD · daemons + vfsd/iocd PIDs ·
BufferPool state · ResourceCounters · user pages (canaries `nu`/`nu_copy`) ·
kernel PML4[0..255] · kernel stacks · PtPoolSnapshot · HHDM PD (undo huge splits).

### 2.3 Restore order (binding)
```
1  canary check (nu region) + PTE-walk dump if corrupt
2  clear deferred-switch globals (load_rsp/load_cr3/next_task_id/save_rsp/nesting)
3  drain_zombie_list()
4  clear FPU owner, test-ctx dummy_save_rsp
5  scheduler_corruption_count → record_failure
6  ResourceTracker::check(baseline) then restore(baseline)
7  HHDM PD restore (only if hhdm_modified; guard pdpt[0]==0x5000; free split PT)
8  PMM bitmap + owner + free count
9  PtPoolSnapshot restore
10 PMM::rebuild_free_list()
11 kernel PML4[0..255] restore
12 user page content restore
13 MemPool restore (data then meta)
14 Scheduler: restore_state → set_shell_task → set_ticks_for_test → restore_task_fields
    → rebuild_all_tasks → restore_rqpod → rebuild_ready_queue
15 re-identify current task by RSP ownership (else set_current_index(0))
16 BufferPool restore
17 ResourceCounters restore
18 kernel-stack restore (skip current; fix live context.rsp to live RSP)
19 current-task state fix (force RUNNING; first switch AWAY, not TO)
20 conditional daemon restart: g_vfs_touched → reset_and_remount + tmpfs_reset_root
    + reload_daemon_tasks; else just rebuild_ready_queue
21 daemon restore_state; vfsd/iocd PIDs; reset_scan_requested; TSS rsp0 fixup
22 refresh snapshot (post-reload recapture)
23 post-check canaries; arch::sti()
```

## 3. ResourceTracker — Universal Leak Detector

- `ResourceCounters`: mempool per-pool, `pmm_pages_used`, tasks, bufpool
  entries, msg_queues, notifies, event_groups, drivers, pipe_buffers, vnodes,
  open_fds.
- `track_*_add`/`track_*_remove` called from the real allocators (PMM, MemPool,
  scheduler task add/remove, BufferPool, …).  Real counters under
  `CONFIG_DEBUG`; no-op stubs in release.
- **`check()` semantics:** `any_leak()` flags only **positive deltas**
  (current > baseline); on leak prints the live task list + per-field
  before/after rows + a 5-frame backtrace.
- **WARN vs FAIL:** `snapshot_restore` calls `check()` and **ignores the return
  value** — a leak delta is a **WARN-level diagnostic**, not a direct FAIL.
  Test FAIL comes from `JARVIS_ASSERT*` and the expect `FAILED:` count.
  Counters are re-synced via `restore(baseline)` so the next test's delta is
  accurate.  (Leaks still bite indirectly: a leaked TCB can wedge the next test
  → watchdog/TIMEOUT.)

## 4. Registry / Class System

- `TestCase { suite, name, func, factory, flags }`; flags `TF_KERNEL=0`,
  `TF_RELEASE`, `TF_USER`, `TF_BENCH`.
- Macros: `JARVIS_TEST(name, "PRE:..|POST:..")`, `JARVIS_TEST_SUITE`,
  `JARVIS_ASSERT*`, `JARVIS_REGISTER_TEST`, `TEST_CLASS`+`REGISTER_CLASS`.
- `g_test_classes[]` maps a class name to `register_*_tests()` lambdas
  (`buffer_pool`, `o1_scheduler`, `ipc_blocking`, `vfs`, `memory`,
  `scheduler`, `safe`, `all`, `all-1`/`all-2`).
- `parse_test_config`: whitespace/`#`-separated class names; missing file →
  default `["safe"]`.
- Entry points: `run_filtered(flags, isolation)`, `run_all()` (no isolation),
  `run_safe()` (TF_RELEASE), `run_suite(name)`, `run_debug()`,
  `run_release()`.

## 5. Isolation Rules

- **No impersonation / fake ticks / direct `on_tick()`** — these bypass the ISR
  epilogue, strand context-switch globals, desync the ready queue, and force
  the snapshot to repair state the test never legitimately created.  The
  v0.3.10 SIMULATED→DRIVEN rework mandates real dispatched tasks (prio ≥ 11),
  real gates, real timer-based overruns.
- The **only** sanctioned impersonation surface is `create_test_task()`
  (registers a BLOCKED non-runnable TCB for pure field-math unit scenarios).
- **VFS fast-path:** touching VFS marks `g_vfs_touched` → restore pays the
  daemon-kill + ELF reload; otherwise daemons keep running and only the RQ is
  rebuilt.
- **Harness exemption (BUGS.md#021):** init (PID 1) is raised to priority 10
  for the suite (base 0 = background reaper).

## 6. Watchdog & QEMU Verdict

- `watchdog_arm(ms, name)`: TSC deadline + PIT channel-0 elapsed baseline +
  PIT channel-1 one-shot (~55 ms) + current task id + name.  Default
  **30 000 ms/test**.
- `watchdog_check_inline()`: on expiry with matching task → `[WATCHDOG]` block
  + `qemu_debug_exit(1)`.
- **QEMU exit:** `shutdown_kernel(result)` — drain serial TX FIFO, `outw`
  ACPI/shutdown ports, `qemu_debug_exit(result)`, keyboard reset, `hlt`.
- **Host verdict** (`tools/run-test.exp`): PASS iff `PLANNED==EXECUTED &&
  FAILED==0`; expected-panic patterns → PASS; unexpected `KERNEL PANIC:` → FAIL;
  timeout (`TEST_TIMEOUT_ALL=250s` / `CLASS=120s`) → TIMEOUT; eof → QEMU_EXIT;
  host stall-watchdog 220s of no serial growth → pkill.

## 7. Invariants (binding)

| # | Invariant |
|---|---|
| Priority | higher numeric = higher priority; idle 0, harness 10, daemons 20, deadline-mon 127 |
| INV-1..7 | scheduler invariants (see `specs/scheduler.md`) |
| D5/D6 | magic stays TCB_MAGIC until unlink; 0xDD poison only by MemPool::free; canonical termination sequence |
| RQ | a current task must never be queued |
| Snapshot | rebuild_ready_queue heals all ready-queue desyncs |
| H2 | deferred-switch race — RESOLVED 2026-08-13/15 (see `specs/ipc.md`) |
