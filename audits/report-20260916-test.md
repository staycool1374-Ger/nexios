# AUDIT REPORT 20260916-134406
PATCH: audits/pending_patch.diff
FILES: src/kernel/task/scheduler.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_isolate.cpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_timer_wheel.cpp, src/kernel/test/test_weak_stubs.cpp, src/kernel/time/timer_wheel.cpp, src/kernel/time/timer_wheel.hpp

## FINDINGS
- None. All five iteration-1 findings verified fixed in-tree (patch lines cited):
  - (1) S2 AP-arm fail-closed: timer_wheel.cpp:478-485 (`if (cpu != 0) return false;` + rationale comment), hpp doc 711-712, test wheel_cpu_isolation (test_timer_wheel.cpp:305-332) asserts `!arm(1,...)`, live_count 0, silent tick. Single-CPU fallback branch (CONFIG_MAX_CPUS<2) uses out-of-range cpu, also fail-closed via the cpu>=CONFIG_MAX_CPUS guard. WHY: an armed-but-never-serviced AP timer can no longer report success.
  - (2) S3 IrqGuard scope: timer_wheel.cpp:538-598 guard lives inside the detach block; fire loop at 599-601 runs after scope close, and lock_.unlock() at 597 precedes both. WHY: no IRQ-off window or spinlock is held across callbacks.
  - (3) S3 scheduler_lock_ contract: scheduler.cpp:17-22 call-site comment (callbacks non-blocking, must not take scheduler services or reschedule); header contract at timer_wheel.hpp:670-672,695-696,728-729 already documented. WHY: residual is documentation-only enforcement, S3 at most per brief.
  - (4) S3 static_assert: timer_wheel.hpp:705-706 (`CONFIG_MAX_CPUS <= 255`, `kMaxTimersPerCpu <= 256`). WHY: Handle uint8_t cpu/slot widths are compile-time bound.
  - (5) S3 manual unlock pairing: comment at timer_wheel.cpp:543-545; verified no `return` between try_lock success (547-549) and lock_.unlock() (597) — only `break` out of the selection loop; the two `return 0` paths (532-534 pre-lock bounds check, 547-549 try_lock failure) hold no lock. WHY: the manual unlock cannot leak.
  - Test-only born-expired fix: future_base() = ns_monotonic() + 10s (test_timer_wheel.cpp:140-142); all 8 tests derive base from it (171,197,218,243,275,307,341,367); no fixed 1e9/1000000000 base remains in the diff (only the 10000000000ULL offset); +10s is unreachable by real ticks in a ms-scale test while production on_tick uses live now_ns. WHY: armed timers can no longer be born-expired by suite-position clock drift.

DECISION: APPROVED
