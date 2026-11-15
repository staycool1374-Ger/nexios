# IPC Baseline — Deferred-Switch Per-Hop Cost Record (M0 / F17, issue #570)

**Doc ID:** NEX-SPEC-2026-11-15-570
**Status:** BASELINE RECORDED — TCG rows measured on `testbed` (debug
`bench`, runs 2026-11-15); HW rows pending (one real machine, owner decision).
No numeric target is frozen here; Q3 numeric targets are set against §4 at M3.
**Milestone target:** v0.6.1 Transition to full Microkernel (M0 gate).
**Related:** `docs/specs/microkernel-transition.md` §5.6 (latency model),
`docs/specs/ipc.md` §4 (deferred switch), `docs/specs/ipc-fastpath.md`
(48 B register tier — not exercised by the baseline workload, which uses
the full path), `src/kernel/test/bench_mk_ipc_baseline.cpp` (harness).

## 1. Purpose (M0 / F17)

Define the baseline workload and record the per-hop cost of the current
deferred-switch path (INV-MK-5: a blocking hop is applied at the next timer
interrupt, so each hop costs up to 1 tick at `CONFIG_TICK_HZ = 1000`) before
any target is set. This document is the measurement record the M3
direct-switch comparison is contracted against. TCG numbers are
non-normative (workload-dependent virtual clocks, no sub-second in-guest
wall reference); only HW rows gate Q3.

## 2. Workload descriptor (the defined workload)

| ID | Shape | Trips | Payload | Prios |
|---|---|---|---|---|
| WL-1 | relay chain, depth 1 (client ↔ R0) | 100 | 48 B (`CONFIG_IPC_FAST_PAYLOAD_BYTES`) | client 30, relays 29..26, daemons 20 (interference-free band) |
| WL-2 | relay chain, depth 2 | 100 | 48 B | same |
| WL-3 | relay chain, depth 3 | 100 | 48 B | same |
| WL-4 | relay chain, depth 4 | 100 | 48 B | same |
| WL-R | rate window, depth 2 | 200 | 48 B | same |

Rules (harness-enforced, fail-closed): every trip is one client
`send_sync` round trip; relays forward with `send_sync` (all but deepest)
and reply with the originator's payload echoed; request/reply type tags
are per-depth (`100+i` / `200+i`) so a misrouted message fails the run
instead of aliasing; all messages priority 0 (FIFO); relays bound
(500-poll trip cap); inbox drained per test (no cross-test leakage);
per-hop slope = (median(d=4) − median(d=1)) / 6 (fixed-overhead-free).

## 3. Measurement protocol

- Unit of record for hops: `arch::Timer::ticks()` (coarse scheduler ticks —
  the INV-MK-5 unit: one blocking hop costs an integer number of ticks).
- Wall unit: `arch::Timer::raw_counter()` converted with
  `raw_counter_freq_hz()` (fail-closed 0 on zero/invalid freq, saturating
  multiply); rewind-proof (never snapshot-rewound `ticks_`).
- Percentiles: insertion sort, `pXX = sorted[(n−1)*XX/100]` (`n = 100`,
  so p50 index 49, p99 index 98); `pmax` reported separately; mean direct.
- Outputs: single-line `[MK-BASELINE]` records greppable from the raw
  serial capture (Logger routes to debugcon in test mode).
- TCG is non-normative: numbers below are the record, not a gate; no
  test asserts a numeric bound tighter than the wedge ceiling
  (`p50 <= hops*8 + 8`) and exact hop accounting.

### 3.4 HW recipe (normative rows pending)

`meson setup build/meson-<arch> --cross-file cross/<arch>-elf.txt
-Darch=<arch> && meson configure build/meson-<arch> -Dtest_class=bench`
→ build the ISO → boot on the target machine → capture the raw serial
stream → grep `[MK-BASELINE]` → fill §4 R-HW rows. The normative Q3
record needs one agreed machine and date (owner decision).

## 4. Results

TCG, x86_64 debug, `bench` 39/39 (testbed, 2026-11-15; two consecutive
green runs; identical values unless noted):

| Row | Depth | Hops/trip | Trips | Ticks p50/p99/pmax | Per-hop ticks×100 | ns p50/p99/pmax |
|---|---|---|---|---|---|---|
| TCG-1 | 1 | 2 | 100 | 2/2/2 | 100 | 3788682–3793176 / 3917518–3944484 / 3950476–3953472 |
| TCG-2 | 2 | 4 | 100 | 4/4/4 | 100 | 7613318–7622307 / 7707698–7719683 / 7709196–7724177 |
| TCG-3 | 3 | 6 | 100 | 6/6/6 | 100 | 11418480–11421476 / 11482898–11494883 / 11488890–11521848 |
| TCG-4 | 4 | 8 | 100 | 8/8/8 | 100 | 15223641–15228135 / 15297048–15304538 / 15309032–15313527 |
| Slope | — | — | — | median_d1=2, median_d4=8, slope=100 (1.00 tick/hop) | — | — |
| R-HW | — | — | — | TBD (one real machine) | — | — |

Rate window (depth 2, 200 trips, 800 hops): `switches=805 source=diag
window_ns=1529348269 hops_per_s=523 switches_per_s=526` (run 2:
`805/1529713804/522/526`). Core locality (single-CPU gate):
`same_core hops_per_s=514–517 p50_ticks=2 up_cpus=1`;
`split=unavailable up_cpus=1 note=Q11/#525`.

Reading: under TCG each blocking hop costs exactly one tick
(p50 == p99 == pmax at every depth; slope 1.00 tick/hop) — the INV-MK-5
limit is directly visible. The M3 direct-switch comparison must beat
this record on HW.

## 5. Target contract

No target is frozen in M0. Q3 numeric targets (per-hop and end-to-end,
direct-switch scope per the Q3 decision) are set against §4 at M3, using
R-HW rows once recorded. The TCG rows above are the comparison method
reference, never the gate.

## 6. Threats to validity

- TCG quantization: tick granularity (1 ms at 1000 Hz) dominates; ns
  values jitter run to run (see the two recorded ranges) while tick
  medians are exact.
- Counter rates differ per arch (riscv64 `rdtime` 10 MHz → 100 ns
  resolution); cross-arch rows must be recorded per arch, never compared
  across arches.
- Single-CPU gate: the split-core leg reports `unavailable` by design
  (Q11/#525); SMP numbers need an `-smp N` harness leg (not in M0 scope).
- Daemon interference is excluded by construction (client/relays above
  daemon prio 20); a loaded host can still stretch TCG wall time —
  tick counts, not wall, are the stable comparator.
