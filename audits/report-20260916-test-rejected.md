# AUDIT REPORT 20260916-133544
PATCH: audits/pending_patch.diff
FILES: src/kernel/task/scheduler.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_isolate.cpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_timer_wheel.cpp, src/kernel/test/test_weak_stubs.cpp, src/kernel/time/timer_wheel.cpp, src/kernel/time/timer_wheel.hpp

## FINDINGS
- [S2] src/kernel/time/timer_wheel.cpp:460 — arm() returns success for any cpu < CONFIG_MAX_CPUS, but production only ever services CPU 0: the single on_tick call site (scheduler.cpp:2004) sits after the AP early-return (ap_tick() has no wheel hook, spec dispatch-only), so an AP-armed timer reports armed yet silently never expires.
  WHY: Success-without-service breaks the arm contract and the patch's own fail-closed convention, a silent liveness hole for timeout/watchdog producers.
- [S3] src/kernel/time/timer_wheel.cpp:515 — on_tick holds arch::IrqGuard across the callback loop (guard not scoped to the locked section; lock released manually at :568 but IRQs stay off through up to 8 callbacks), extending the tick IRQ-off window by unbounded callback time.
  WHY: IRQ latency in ISR context should cover only the bounded scan, not arbitrary callbacks.
- [S3] src/kernel/task/scheduler.cpp:2004 — wheel callbacks fire while the outer scheduler_lock_ is held (call site inside the lock_acquired region), so the header's "lock released before callbacks" guarantee covers only the wheel lock and the must-not-reschedule/block callback contract is documented-only and statically unenforced.
  WHY: Any future #18 producer callback needing scheduler services deadlocks the tick on the non-recursive lock.
- [S3] src/kernel/time/timer_wheel.hpp:670 — Handle stores cpu/slot as uint8_t with no static_assert binding CONFIG_MAX_CPUS <= 255 / kMaxTimersPerCpu <= 256, so a future config bump silently truncates the narrowing casts in arm().
  WHY: Valid today (CONFIG_MAX_CPUS=8) but the invariant is unchecked at compile time.
- [S3] src/kernel/time/timer_wheel.cpp:568 — on_tick releases the spinlock via manual lock_.unlock() instead of an RAII guard released before callbacks, fragile on panic/early-exit paths (all other entry points use IrqSpinLockGuard).
  WHY: Non-RAII unlock risks a permanently held lock with IRQs disabled if the path ever gains an early exit.

## PATCH
audits/rejected_patch.diff was written (applies on top of audits/pending_patch.diff): makes arm() fail-closed for cpu != 0 with BSP-only rationale, updates the arm doc, and reworks wheel_cpu_isolation to assert the rejection.

DECISION: REJECTED
