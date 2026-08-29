# Deadline Detection & Deadline-Monitor Specification

**Semantics:** binding contract for deadline tracking, miss detection, the
deadline-monitor task, WCET overrun detection, and the configurable miss
actions.  Synthesis of the deadline subsystem in `scheduler.cpp`/`task.cpp`,
the deadline test classes, and the resolved dangling-monitor bug.

```
          ┌──────────────┐   ┌─────────────────────────────────────┐
          │  APIC timer  │──▶│ Scheduler::on_tick() (timer ISR,     │
          └──────────────┘   │  scheduler_lock_ via try_lock)       │
                             └───────────────┬─────────────────────┘
                      ┌──────────────────────┼──────────────────────┐
                      │ CONFIG_DEADLINE_     │                      │
                      │ MONITOR_TASK==1      │ ==0 (inline)         │
                      ▼                      ▼                      ▼
             s_scan_requested_=1     trigger_deadline_     deadline_list_.pop_
             (atomic) + wake        monitor_scan() (tests, earliest_if_expired()
             monitor (magic-checked) direct scan)          loop
                      ▼                                     │
             ┌──────────────────────┐                       │
             │ [deadline-mon] task  │ prio 127, NO_PERIOD,  │
             │ monitor_task_entry() │ BLOCKED by default    │
             └──────────┬───────────┘                       │
                        ▼                                   │
             ┌──────────────────────────┐◀──────────────────┘
             │ Scheduler::scan_        │ holds scheduler_lock_, walks all_tasks_
             │ deadlines()             │ predicate: period>0 && dl!=0 && !missed
             └──────────┬───────────────┘              && ticks > deadline_ticks
                        ▼
             capture SS state; missed=true; count++
             deadline_miss_handler(task, now-dl)
             re-arm: deadline_ticks += period_ticks; missed=false
                        │ CONFIG_DEADLINE_ACTION
        ┌───────┬───────┼────────┬────────────┐
        ▼       ▼       ▼        ▼            ▼
   0 LOG_ONLY 1 PANIC 2 DEMOTE 3 KILL      4 NOTIFY_MONITOR
   (log)     (panic)  (prio>>=1+ (TERMINATED (SIGUSR1)
                       move_prio) +defer_kill)
```

## 1. Deadline Model (TCB fields)

| Field | Semantics |
|---|---|
| `period_ticks` | release period; `0` = aperiodic (untracked) |
| `deadline_ticks` | **absolute** deadline tick (init `ticks()+period_ticks`) |
| `deadline_missed` | per-period latch (cleared on re-arm; not a stable signal) |
| `deadline_miss_count` | monotonic miss counter (stable assertion target) |
| `executed_ticks` | ++ per tick while RUNNING/READY |
| `remaining_ticks` | per-period budget; reloaded to `period_ticks` at 0 (I-1) |
| `wcet_ticks` | static WCET config (`0` = implicit 100% load) |
| `wcet_overrun_fired` | one-shot latch per period |
| `ss_state_on_deadline_miss` / `ss_budget_on_deadline_miss` | SS snapshot captured at miss |

**Detection predicate (MC/DC):** a miss fires iff `period_ticks > 0`
`&& deadline_ticks != 0 && !deadline_missed && ticks() > deadline_ticks`
(strictly past — future deadlines never fire, I-3).

## 2. `scan_deadlines()` (scheduler.cpp)

- Runs **in task context** by the monitor task, or directly by tests via
  `trigger_deadline_monitor_scan()`.  (`CONFIG_DEADLINE_MONITOR_TASK=0` uses an
  inline `deadline_list_.pop_earliest_if_expired()` loop in `on_tick`.)
- Walks **`all_tasks_`, not `deadline_list_`** (a deadline can be reconfigured
  after `add_task()`; the list is the O(1) scan path, not the authority).
- On miss: capture SS context → `missed=true; count++` → `deadline_miss_handler`
  → **re-arm** `deadline_ticks += period_ticks; missed=false;
  wcet_overrun_fired=false` (exactly one handler call per period, I-2).
- Integrity: `__atomic_fetch_add(&deadline_detection_integrity,1)` after every
  completed scan (I-7); tests assert it advances by exactly 1.
- Locking: holds `scheduler_lock_` for the whole scan (task-context only — this
  is why a monitor task exists rather than scanning in the ISR).

## 3. `deadline_miss_handler()` — configurable action

| Action | Behavior |
|---|---|
| `0 LOG_ONLY` (default) | `[DMD]` log; optionally "budget exhausted" if captured SS state == EXHAUSTED |
| `1 PANIC` | kernel halt |
| `2 DEMOTE` | `priority >>= 1` (floor 1) + `move_priority` via `effective_priority()` (INV-6) |
| `3 KILL` | TERMINATED + `defer_kill` (reclaimed by `process_deferred_kills`) |
| `4 NOTIFY_MONITOR` | SIGUSR1 to `CONFIG_DEADLINE_MONITOR_PID` / test override |

## 4. The Deadline-Monitor Task

- **Creation:** `Scheduler::init()` → `ensure_monitor()` — spawns
  `monitor_task_entry` at **priority 127**, NO_PERIOD; stores `s_monitor_task_`;
  immediately BLOCKED + dequeued under `scheduler_lock_`.
- **Loop:** `dequeue_ready(me); state=BLOCKED; reschedule();` then if
  `s_scan_requested_` exchange → `scan_deadlines()`.
- **Wake:** `on_tick` (when `!is_test_active()`): `s_scan_requested_=1`, then
  `READY + enqueue_ready` — **gated on `magic == TCB_MAGIC`**.
- **Dangling-pointer hazard (RESOLVED):** `reboot_from_table()` kills every
  non-idle/non-current task — including the monitor (not in `g_task_defs`) — so
  `s_monitor_task_` would dangle into a freed/reused MemPool block.  Four
  load-bearing fixes:
  1. `cleanup()` clears the pointer (`reset_monitor_task()`) when the monitor's
     TCB is freed (task.cpp).
  2. `on_tick` wake re-validates `magic` before writing state/enqueue.
  3. `trigger_deadline_monitor_scan()` calls `scan_deadlines()` synchronously
     (the block/wake handshake can strand the monitor READY-but-not-in-runq
     under deferred-switch + snapshot, so tests never rely on it).
  4. `snapshot_restore` resets `s_scan_requested_`; `reload_daemon_tasks` skips
     the live monitor; `ensure_monitor()` re-spawns on the next valid path.

## 5. WCET Overrun Detection

```
on_tick: if (wcet_ticks > 0 && !wcet_overrun_fired && executed_ticks > wcet_ticks) {
             wcet_overrun_fired = true;
             wcet_overrun_handler(task, executed_ticks - wcet_ticks);   // weak, LOG_ONLY
         }
```
Distinct from deadline miss: a blocked task past its real deadline fires the
**deadline** handler but **not** the WCET handler (`DeadlineMissWithinWcet`).

## 6. Invariants (binding)

| # | Invariant |
|---|---|
| I-1 | `remaining_ticks` reloads to `period_ticks` exactly once per period at 0 (`timer_period_reload`) |
| I-2 | One handler invocation per period: re-arm after each miss (`DeadlineRearmOnPeriodRollover`, `timer_deadline_miss_only_once`) |
| I-3 | Strict expiry: `now > deadline_ticks`; future deadlines never fire (`timer_deadline_miss_skips_future`) |
| I-4 | Untracked exclusion: `period_ticks==0` or `deadline_ticks==0` never scanned (`timer_deadline_miss_skips_zero`) |
| I-5 | TERMINATED tasks skipped by the scan |
| I-6 | DEMOTE floor at priority 1; every change re-bucketed via `move_priority` |
| I-7 | Scan integrity counter advances exactly 1 per completed scan |
| I-8 | **Liu-Leyland is advisory** (warning-only at add_task) — no admission gate; the real guarantee is bounded per-tick cost (O(n_tasks)) |
| I-9 | Monitor `dequeue+BLOCKED` and on_tick `READY+enqueue_ready` are mutually exclusive under `scheduler_lock_` (no INV-5 violation) |
| I-10 | No dangling monitor pointer (cleanup clear + magic check + direct-scan test hook) |

## 7. Gaps / Non-Goals

- **`deadline_rush` is NOT implemented** — deadline-awareness is **post-facto
  detection only**; there is no deadline-aware *pre-emptive* scheduling
  (rush-preemption was archived in `_archive/rms-rework-plan.md`).
- **SS exhaustion as deadline** (`CONFIG_SPORADIC_SERVER_EXHAUSTION_IS_DEADLINE`)
  is off by default.
- **`CONFIG_DEBUG_IPC_SCHED`** (`ipc_sched_trace.hpp`) must be **undefined** for
  release gates: the `[TICK]`/`[SW]`/`[APPLY]`/`[RMS]` traces perturb
  timing-sensitive tests.
