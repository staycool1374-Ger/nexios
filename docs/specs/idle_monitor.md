# Idle-Task Safety Monitors (v0.6.x design paper)

**Doc ID:** NEX-SPEC-2026-10-03-001
**Status:** DRAFT (pending SIL3 design audit; no implementation scheduled)
**Milestone target:** v0.6.x (design); implementation phases span v0.6.x per §5
**Issues:** #43 (primary: idle-task safety monitors)
**Related:** `docs/specs/debugd.md` (design-paper precedent),
`docs/specs/watchdog.md` §6 (boot-only disposition), #41 (per-task watchdog,
escalation sink), #191 (server supervision/restart, out-of-scope consumer),
#45 (deterministic boot; §1 shares the budget posture),
#46 (RO-authenticity; shares the idle slice)

## 0. Corrections to the issue text (binding)

- **#43 `float` percentage fields do not compile into this kernel.**
  Zero `float`/`double` type uses exist in `src/kernel/` (all grep hits
  are `double-free`/`enqueue` compounds). Precedent is allocation-free
  integer math (`scheduler.hpp:143-146`), `LOADAVG_SCALE=1000000` with
  per-mille accessors (`scheduler.hpp:849-870`), and
  `top_task_per_mille` (`shell.cpp:1112`). All
  `*_percent` fields in the issue become `uint32_t …_per_mille`.
- **#43 "magic-pattern verification" misnames the mechanism.** No
  `STACK_MAGIC_PATTERN` exists. Stack integrity in-tree is
  `CANARY_MAGIC` + per-segment before/after canaries +
  `canary_verify_user_segments`/`canary_verify_kernel_stack`
  (`task.hpp:397-415,958-961`; `task.cpp:806-908`), plus
  `kstack_low_water_` sampled in the switch path
  (`scheduler.cpp:4057-4059`). The monitor reuses these; no new magic
  constant is introduced.
- **#43 "`rdtsc`" is the wrong name.** The portable primitive is
  `arch::rdtsc()` (x86 TSC / aarch64 cntpct / riscv rdtime,
  `arch/hal/timer.hpp:126`). Cycle-accounting precedent is
  `exec_ns_total`/`exec_period_ns`/`exec_stamp_ns`
  (`task.hpp:333-339`) + `TaskTimes` — utilization derives from
  these, not from raw TSC deltas taken in idle.
- **#43 "extend idle-task" understates the existing load.** The idle
  task already does four jobs in `integrity::idle_task_main`
  (`integrity.cpp:155-169`): `cleanup_step` + `check_section_markers`
  + `crc_process_chunk` + `auth_verify_poll`, then `hlt`. Every
  monitor slice MUST be budgeted per pass exactly like
  `crc_process_chunk` — bounded work, never starving cleanup/reap.
  The AP idle (`ap_idle_main`, `integrity.cpp:213-217`) is halt-only
  by design (single-core allocators) — monitoring is BSP-only.
- **#43 March C-, PMU, and RDPMC are assumed, not available.**
  `rdpmc`/`RDPMC`/`read_pmc` have zero kernel hits. The ALU test and
  cache-miss counters are new code gated on a proof-of-safety, not
  assumed infrastructure.
- **#43 "unit tests … integration tests …" pre-claim test seams that
  do not exist.** `test_idle_task.cpp` holds 10 tests covering
  boot/yield/markers/CRC-incremental only; the six metric structs are
  claimed by issue text alone. This spec defines the test seams
  (`auth_verify_poll`-style split functions, §1).
- **#43 re-specifies sinks that already exist.** Deadline:
  `deadline_missed`/`deadline_miss_count` + `deadline_miss_handler`
  (`task.hpp:327-328`, `scheduler.cpp:5435`),
  `CONFIG_DEADLINE_MONITOR_TASK` + the `[deadline-mon]` task
  (`scheduler.hpp:421-523`). Watchdog: per-task
  `wdog_armed`/`wdog_period_ticks`/`wdog_expiry_tick`/`wdog_gen`
  + `watchdog_expiry_handler` + `CONFIG_WATCHDOG_ACTION` 0–4
  (`task.hpp:548-566`, `scheduler.cpp:5531-5603`). The monitor only
  *feeds* these paths; it defines no new escalation semantics.
- **#43 `CONFIG_IDLE_MONITOR_*` names are novel.** None exist in
  `nexios_config.h`. Conventions (cf. `CONFIG_DEADLINE_MONITOR_TASK`,
  `CONFIG_WATCHDOG_GRACE_TICKS`, `CONFIG_IDLE_HOOK`,
  `CONFIG_STACK_OVERFLOW_HOOK`): `#ifndef` guard + default, `0`/`1`
  feature flags, `_MS`/`_TICKS`/`_US` suffixed tunables.

## 1. Monitor-loop contract (normative)

- M1: BSP-idle only. The slice runs inside `integrity::idle_task_main`
  AFTER `cleanup_step`, never on APs, never in tick/IRQ context.
- M2: Bounded work per pass. Each metric scanner processes at most
  `CONFIG_IDLE_MONITOR_CHUNK` TCBs per pass (chunk budget in the
  style of `crc_process_chunk`), then returns; a cursor resumes next
  pass. No pass may skip `hlt`.
- M3: No blocking, no allocation. The slice is `noexcept`,
  touches MemPool-or-static memory only, takes no lock it does not
  already own (registry iteration is lock-free snapshot or short
  guarded walk — the implementation picks one and the audit checks
  it against INV-2).
- M4: Skip invalid TCBs via the `is_valid` + `is_idle_task` guards;
  never inspect a task being torn down (generation check against the
  registry entry).
- M5: Test seams. Every scanner is a free function of the form
  `idle_scan_<metric>(cursor, budget) -> progress`, unit-callable
  from kernel tests exactly like `auth_verify_poll`.
- M6: The monitor never kills, reaps, reprioritizes, or rebudgets.
  It sets flags and calls the existing handlers (§3). Violation of
  M6 is an S1 audit finding.

## 2. Per-metric sections (normative data sources)

### 2.1 Stack monitoring — reuse, publish low-water

Source (existing): `CANARY_MAGIC` before/after canaries +
`canary_verify_*` + `kstack_low_water_`. New TCB field: none for
canaries; one `uint32_t stack_low_water_bytes` published snapshot
(the switch path already samples it — idle only publishes).
Scanner: `idle_scan_stack` runs `canary_verify_*` on the chunk's
TCBs; a failed canary calls the existing stack-overflow hook path
(`CONFIG_STACK_OVERFLOW_HOOK`), never a new fault path.

### 2.2 CPU accounting — derive, do not sample TSC

Source (existing): `exec_ns_total`/`exec_period_ns` + context-switch
counters + `TaskTimes`. No `arch::rdtsc()` calls in the slice;
utilization is computed from accounted time, which is valid even
when idle never runs (under load the counters still accumulate in
the tick path — this answers §6 Q2 structurally).

### 2.3 Heap / memory — budget pages yes, MemPool attribution no

Source (existing): per-task `memory_budget_pages_` /
`memory_used_pages_` + PMM counters. New: per-task alloc/free
*counts* (cheap integers bumped at the allocator call sites, not a
walk of allocator internals — walking PMM/MemPool per pass is
forbidden by M2/M3). Leak heuristic: `allocs - frees` growth over
N passes with `memory_used_pages_` flat is reported, never acted
on. MemPool blocks carry no owner tag, so cross-task attribution
beyond the budget counters is a §6 open question, not a promise.

### 2.4 Utilization (per-mille) — from existing counters

New TCB fields (P1a, §5): `uint32_t util_per_mille`,
`uint32_t block_per_mille`, `uint32_t preempt_per_mille`,
`uint32_t deadline_meets`, all updated by `idle_aggregate_util`
from `exec_*` + already-existing `deadline_miss_count`.
Saturation rule: `util_per_mille > 950` with a critical task
missing deadlines logs a warning via the dmesg catalog (new code
only if no fitting catalog entry exists). No float anywhere (§0).

### 2.5 RT / WCET-observed — compare, never overwrite design values

Source: design `wcet_ticks` (admission) vs observed maxima.
New TCB field: `uint64_t wcet_observed_ticks` (max only, bumped in
the tick path where execution time is already known — not
measured by idle). `wcet_observed > wcet_design` logs; it never
re-buckets, reprioritizes, or fails admission retroactively (that
would rewrite scheduling history — forbidden).

### 2.6 Diagnostics / stall — flag, feed existing handlers

New TCB fields: `uint64_t last_progress_tick` (bumped on state
change in the paths that already know it),
`bool stuck_suspected`. Scanner: `now - last_progress_tick >
CONFIG_IDLE_MONITOR_STALL_THRESHOLD_TICKS` sets the flag and arms
the task's own watchdog (`SYS_WATCHDOG_CREATE` self semantics are
unchanged — the arm is requested through the existing expiry
handler path, §3). The monitor never synthesizes a kill.

## 3. Escalation interface (normative)

- E1: The monitor's only outputs are TCB flags + calls into
  `deadline_miss_handler` / `watchdog_expiry_handler` /
  `watchdog_arm` equivalents. No new kill/reap/reprioritize path.
- E2: Disposition of every escalation follows the existing
  enumerations (`CONFIG_WATCHDOG_ACTION` 0–4,
  `CONFIG_DEADLINE_ACTION`) — the spec adds no action value.
- E3: #191 restart is a consumer, not a dependency: the monitor
  flags; supervision decides. #41 semantics are reused verbatim;
  any extension to watchdog authority (cross-task arm) is #41
  follow-up scope, not this spec.

## 4. Configuration (normative conventions)

New `CONFIG_IDLE_MONITOR_*` block follows §0 conventions:
`CONFIG_IDLE_MONITOR_ENABLED` (0/1, default 1 on DEBUG, 0 on
release until qualified), per-metric kill-switches
(`..._STACK_CHECK`, `..._UTIL`, `..._STALL` — each 0/1),
`CONFIG_IDLE_MONITOR_CHUNK` (TCBs per pass),
`CONFIG_IDLE_MONITOR_STALL_THRESHOLD_TICKS`, and
`CONFIG_IDLE_MONITOR_ALU_TEST_PERIOD_MS` (reserved for the
deferred March phase — defined but inert until P3 lands).
Ranges enforced by `static_assert` (cf.
`admission_selftest.cpp:34-40`).

## 5. Phased delivery (sub-issue slicing)

1. P1a + P2 (first sub-issue): per-mille utilization + stall fields
   (P1a) + registry iteration + canary verify + low-water publish +
   test seams (P2 reduced). Gate: new `idle_monitor` test class
   green; no scheduler-behavior delta (determinism regression
   class stays green).
2. P4: per-mille aggregation + WCET-observed compare + miss/meet
   ratios. Gate: reads existing sinks only; no new escalation.
3. P5a: alloc/free counts + budget-pages leak heuristic (report
   only). Gate: allocator-path overhead bounded and measured.
4. P6-thin: stall detector feeding existing wdog/deadline
   handlers. The monitor never arms a watchdog that the owning task
   did not itself arm (cross-task fresh arms are #41 follow-up
   scope, §3 E3). Gate: gated on #41 semantics reuse (no authority
   extension); escalation matrix test.
5. P3 March C- ALU (SPEC-ONLY, no sub-issue scheduled): design
   section deferred until burst-budget proof + fault-injection
   test story exist. Deferred, not denied.
6. P5b RDPMC/cache-miss (SPEC-ONLY): deferred until a PMU driver
   is proven. No normative counter sentence may appear outside
   this pointer until then.

## 6. Open questions (for audit, not blockers)

- Q1: TCB bloat vs sidecar table — six structs inline in the TCB
  vs an external array indexed by task id (cache + memset-site
  cost vs indirection)?
- Q2: Sample-in-idle vs aggregate-in-idle — under sustained load
  idle never runs; §2.2/§2.5 answer by construction (tick
  accumulates, idle aggregates), but stall detection itself
  degrades when idle starves. Acceptable, or does stall need a
  tick-path heartbeat?
- Q3: March C- burst budget in µs per idle pass without
  perturbing the 1 ms tick (needs measurement, not assumption)?
- Q4: MemPool block ownership — tag blocks (memory cost) or
  accept budget-counters-only attribution (§2.3)?
- Q5: Anomaly sink for certification evidence — dmesg catalog vs
  a dedicated append-only audit buffer (consumer decision)?
