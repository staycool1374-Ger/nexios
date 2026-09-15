# WCET Re-audit — v0.4.5 (post-balancer)

**Doc ID:** NEX-SPEC-2026-09-15-002
**Status:** MEASURED (issue #62)
**Milestone target:** v0.4.5
**Prior art:** `docs/_archive/wcet_analysis.md` (1/10/40-task SIMULATED
table — superseded by driven measurements below, kept frozen).

## 1. Why re-audit

Issue #61 put `Scheduler::balancer_tick()` in the BSP `on_tick` tail
(every `BALANCER_TICK_PERIOD` = 10 ticks, production runs only,
try_lock-skip). Every pre-#61 tick bound is therefore stale until the
balancer's worst case is bounded jointly with the scan.

## 2. Method

Driven (real task populations, no simulation), rdtsc max over 300
iterations, QEMU-virtualized TSC (same caveat as the archive: relative
numbers, not silicon). Classes: `wcet_scheduler` (scan + balancer),
`bench_wcet_memory` (mempool + vmm). Regression contract I-1..I-10
(deadline/timing classes) re-run green — see §4.

## 3. Bounds (2026-09-15, debug build, -smp 2 QEMU)

| Path | Population | Worst (cyc) | Note |
|---|---|---|---|
| `scan_deadlines` | 30 genuinely-overrun tasks | 2,030,000 | `wcet_scan_deadlines`, kIters=300 |
| `balancer_tick` | 8 queued aperiodic, max spread | 8,000 | `wcet_balancer_tick_worst`, kIters=300 |
| `mempool_alloc_free` | — | max 101,000 (avg 950) | `bench_wcet_memory` |
| `vmm_map_unmap` | — | max 222,000 (avg 165,795) | `bench_wcet_memory` |

Balancer verdict: worst 8k cyc ≈ 0.4% of the 30-task scan bound,
well clear of tick budget headroom; it runs at most
every 10th BSP tick, is skipped under lock contention (try_lock), runs
only outside test mode, moves at most 2 tasks per tick, and allocates
nothing (moves queued TCBs). No tick-budget table changes required.

## 4. Regression gates (all green 2026-09-15)

timing_core 18/18, deadline_miss 5/5, deadline_action 1/1,
deadline_recovery 4/4, deadline_ss 3/3, wcet_overrun 2/2,
memory_pmm 9/9, memory_determinism 4/4, cache_coloring 4/4,
wcet_scheduler 2/2, bench_wcet_memory 2/2. Full gates: debug `all`
1360/1360, release `all` 85 (trace OFF) — see test-history.txt.
