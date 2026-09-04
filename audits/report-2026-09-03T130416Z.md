# AUDIT REPORT 2026-09-03T130416Z
PATCH: audits/pending_patch.diff
FILES: Makefile, tools/qemu/trace_events.txt, src/kernel/test/test_stress_hrt.cpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_expected_counts.hpp

## FINDINGS

### Remediation verification (previous S2, iteration 2)
- [S2] RESOLVED — src/kernel/test/test_stress_hrt.cpp:374-378, 460-461 — failed send_sync samples are no longer silently dropped from the latency metrics.
  WHY: every `!ok` increments the per-phase counter `g_base_fail`/`g_stress_fail` (atomic, RELAXED, ordered before `g_rt_done` RELEASE so the harness read via ACQUIRE is coherent) and `continue`s; both counters are then asserted `== 0` via `JARVIS_ASSERT`, which records a failure and returns on violation — a failed IPC under stress can no longer pass silently, and the `base_nz`/`stress_nz >= k_iterations/8` guards remain only as sample-validity floors, not as failure sinks.  Fully addresses the finding.

### Critical checks
1. Dynamic allocations in critical paths: NONE. The RT measurement loop uses only stack locals, static log2 histogram arrays (`g_base_hist`/`g_stress_hist[32]`), and atomic ops. The hammerer's `MemPool::alloc`/`PMM::alloc_page` are the deliberate net-zero stress load, paired with frees under null checks.
2. Concurrency/IRQ boundaries: `Scheduler::add_task` triple is wrapped in `arch::IrqGuard`. No spinlock is held across `reschedule()`/`send_sync()`; the hammerer's per-iteration `SpinLock` is uncontended and released within the same iteration. `send_sync` runs with IF=1 intentionally (receiver dispatch).
3. Assertion masking: the new fail-counting is sound; the only excluded samples are `elapsed == 0` (TCG quanta artifact, documented in the canary test and covered by the nz floors). No Heisenbug path remains.
4. Double-free: tasks are created once and cleaned in strict order `rt → drain → hammer → drain → receiver → drain` after self-termination; hammerer alloc/free pairs cannot double-free (null/zero guards).
5. Kernel-invariant interference: hammerer self-roundtrip (`send(self)` + `recv`) never grows its queue (self-send on a full queue returns immediately, ipc.cpp:199), so no queue/resource drift.
6. Preprocessor/conditional asymmetry: NONE introduced. `QEMU_ICOUNT`/`QEMU_TRACE_EVENTS` default to empty on all arches (no change to default runs); `trace_events.txt` is committed so the `-trace` path exists only when opted in via `HRT_ICOUNT=1`. The new test file is picked up by the existing `TEST_SRC_FILES` glob on every arch, and `arch::rdtsc/hlt/interrupts_enabled/Timer::ticks` all resolve on aarch64 (io_impl.hpp:108/168/177). Registration is unconditional, consistent with the bench-* convention.

### Carried-forward S3 (accepted in iteration 1, unchanged by the corrective patch)
- [S3] src/kernel/test/test_stress_hrt.cpp:209-220 — `hist_p95` returns the bucket lower bound (1<<b), underestimating true p95 by up to 2x.
  WHY: compared against an ~8x-headroom relative bound, an underestimate cannot practically false-pass.
- [S3] src/kernel/test/test_stress_hrt.cpp:316-320 — the wedge/pathological cap and the new fail asserts are ordered AFTER `wait_for_termination_safe` calls that have no timeout; a wedged scheduler (or a `send_sync` failing with the request undelivered — ipc.cpp:185/248 — which starves the receiver's `while (!IPC::recv(msg))` loop of one of its `k_iterations` messages) surfaces as a 220s host-watchdog HANG instead of a clean assert.
  WHY: still a visible failure (not masking); consistent with the accepted iteration-1 disposition of the same class of issue, and the fail asserts remain reachable whenever all tasks terminate.
- [S3] src/kernel/test/test_registry.cpp:460/630/778 + src/kernel/test/test_expected_counts.hpp:209 — unconditional `stress_hrt` registration and x86_64-TCG-calibrated absolute caps (`k_abs_floor`=10M, `k_pathological_cap`=20M).
  WHY: consistent with the existing bench-* convention; a non-x86 TCG counter rate could false-fire the absolute bounds, but all relative bounds (avg-ratio) remain arch-agnostic.

## PATCH
No rejected_patch.diff written.

DECISION: APPROVED