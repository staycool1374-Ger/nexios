# AUDIT REPORT 20260916T205448Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/elf/elf.cpp, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_process.cpp, src/kernel/task/scheduler.cpp, src/kernel/task/scheduler.hpp, src/kernel/task/task.cpp, src/kernel/task/task.hpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_weak_stubs.cpp, src/kernel/test/test_task_metering.cpp

## FINDINGS
- [S3] src/kernel/task/scheduler.cpp:2782 — tick landing between stamp_exec(next) and the actual dispatch bills the descheduled interval at the next charge
  WHY: bounded by switch latency (sub-us, negligible vs admission granularity) and structurally identical to the pre-existing tick-quantum error
- [S3] src/kernel/task/scheduler.cpp:3692 — capture_task_fields/restore_task_fields omit the new exec fields
  WHY: stamp+totals stay mutually consistent (all three untouched), so no double-charge or loss; monotonic clocks are intentionally preserved across test restores, matching lifetime-total semantics
- [S3] src/kernel/task/scheduler.cpp:1134 — read_times charge-before-read on SMP bills via the local stamp if the task migrates mid-call
  WHY: cross-CPU accounting accuracy is explicitly deferred to #23 (SMP admission extension); uniprocessor behavior is exact
- [S3] src/kernel/test/test_task_metering.cpp:389 — Ring-3 probe tests are x86_64-only (#else vacuous PASS)
  WHY: same precedent as issue #143 (no Ring-3 probe fixture on other archs); kernel-side metering paths are arch-independent and covered everywhere

Checks passed: no dynamic allocation on any path (TCB alloc is sizeof-parameterized, +24B trivial); all charge sites run under IrqGuard+scheduler_lock_ or the pre-existing IRQ-off tick discipline with no new lock order (ns_monotonic is lock-free and already called from on_tick); tests are failure-sensitive (each fails with metering disabled: zero totals, -1 rets); no PMM/pool ownership changes; no #ifdef asymmetry; issue #21 thread carries graphify query + vault search dispositions.

DECISION: APPROVED
