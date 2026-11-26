# Liveness Guarantee (issue #546-D)

Positive liveness: denial and waiting are bounded, counted, and escalated —
never silent. This document states the complete guarantee; the scheduler
contract (§4, INV-LOCK) and IRQ contract (§9 failure policy) own the
enforcement points, `docs/lock-rank-ledger.md` owns rank truth.

## 1. What is bounded, by what constant

| Bound | Constant | Fail direction |
|---|---|---|
| Bitmap starvation window | `kStarvationBreakerTicks` = 20 denied ticks | exactly one forced dispatch per window (R4) |
| EDF relief | same shared 20-tick window (R-RELIEF) | one forced victim-band dispatch per window max, even with EDF pending (R5) |
| Covered wait | `kWaitCeilingTicks` = 1000 consecutive ticks | count + one dmesg WARN per task per boot (`kDmesgBase_SCHED + 26`); wait never mutated (R6) |
| Relief accounting | per-window `relief_fires` / `relief_deferred` | `relief_fires + breaker_fires ≤ ticks/20`, always |

Nobody is killed, no priority is mutated, no queue is reordered by the
liveness machinery. The only dispatch-affecting state is the pre-existing
breaker window (`starve_level_`/`starve_ticks_`); ages and wait counts are
derived diagnostics, never read by any dispatch, wake, or admit path.

## 2. Covered waits (closed list)

IPC receive/reply waits, join waits, lock park waits, sync-object waits
(mutex/semaphore/eventgroup/notify BLOCKED states). Excluded: WAITING
(sleep/delay — the duration *is* the request), admin-parked tasks,
teardown/reap paths, Boot/Isr contexts, the idle task. Adding a row
requires a plan amendment + auditor sign-off.

## 3. Failure policy (contract §9 home)

`CONFIG_LIVENESS_ACTION` (0/1/2, identical both builds): 0 = compiled out;
1 = shipped default (count + WARN, wait continues); 2 = certification
builds only (deferred halt via `irq_contract::flush_halt` at the next
safe point, one fire, never re-entered). C6-style exemption: none needed —
level 2 halts *before* any state change, so "wait continues" holds at
every level that continues.

## 4. Reset discipline (two families)

- **Derived** (ages, wait ticks, denial mirror): cleared by
  `reset_liveness_ledger()`, wired at `Scheduler::init()` and
  `restore_state()` (same block as the #242 breaker reset). Never
  inherited across tests or boots.
- **Monotone** (`wait_ceiling_count`, `relief_fires`, `relief_deferred`,
  `breaker_fires`, per-class escalation counts): never rewound by snapshot
  restore (oracles precedent, INV-ORA-5); reset only by the
  `reset_liveness_counters_for_test()` seam, called where
  `reset_sched_oracles_for_test()` is called.

## 5. Readout

`kernel::liveness::LivenessReadout` (schedoracles shape): `wait_ceiling_count`,
`relief_fires`, `relief_deferred`, `breaker_fires`, `max_ready_age`,
`wait_windows_max`, `holder_blocked_observed` (B probe). Plus
`grace_cycles`/`grace_source` from the tick-watchdog readout (Increment C
owns the numbers; D consumes them as the soak signal).

## 6. Non-goals

No admission/schedulability change; no killing; no priority mutation; no
`kStarvationBreakerTicks` change; no ACTION=2 wiring; no SMP; no second
forced-dispatch source.
