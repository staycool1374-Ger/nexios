# AUDIT REPORT 2026-09-03T13-35-08Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/test/test_scheduler_hrt.cpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_expected_counts.hpp, src/lib/test.hpp

## FINDINGS
- [S3] src/kernel/test/test_scheduler_hrt.cpp:636 — missing trailing newline at EOF
  WHY: Trivial style nit only; the build flags are `-Wall -Wextra -Werror` without `-Wnewline-eof`, and the object is already built clean (`build/kernel/test/test_scheduler_hrt.o`), so no functional impact.
- [S3] src/kernel/test/test_scheduler_hrt.cpp:309-328 (and 435-453, 564-592) — create-then-assert ordering can leak a created-but-never-added TCB on OOM
  WHY: If a later `TaskControlBlock::create` returns nullptr, the `JARVIS_ASSERT(... != nullptr)` returns before `Scheduler::add_task`, orphaning any earlier successfully-created TCB; identical to the approved #101 stress_hrt baseline and reachable only under OOM, so not blocking.
- [S3] src/kernel/test/test_scheduler_hrt.cpp:247-248 — `stress_avg <= base_avg * k_avg_ratio || stress_avg < k_abs_floor` absolute-floor waiver
  WHY: With base avg 2.53M the 10M floor caps stress at ~4x base regardless of the 8x ratio, slightly weakening the relative bound, but this matches the #101 methodology, the wedge cap (20M) still bounds max, and fail counters are asserted ==0 — not masking.
- [S3] src/kernel/test/test_scheduler_hrt.cpp:447-449 — RELAXED `g_acked` vs RELEASE `g_recv_time` ordering relies on x86 TSO
  WHY: The acquire-read of `g_acked` then `g_recv_time` is not formally synchronized (RELAXED store does not establish happens-before), but on x86 TSO program-order draining makes the measured value correct in practice and the measurement is best-effort with >=3x headroom — informational only, no memory-safety impact.

No S1/S2 findings. Verified against the six critical checks:
1. No dynamic allocation in measurement paths (fixed 32-bucket histograms, statics; hammerer MemPool/PMM/IPC churn is net-zero stress injection).
2. `Scheduler::add_task` blocks wrapped in `arch::IrqGuard`; hammerer SpinLock is lock/unlock-only; `Semaphore::wait`/`post` release their spinlock before `reschedule` (C-1/C-2); no lock held across send_sync/wait/post.
3. Timing bounds calibration-derived (given 2026-09-03 data: stress/base ratio ~1.0 vs k_avg_ratio=8, max 3.81M vs cap 20M, p95 2.1M vs 16.8M bound — all >=3x headroom); per-phase fail counters asserted ==0; zero-elapsed samples are counted in `nz` with `nz >= k_iterations/8` sufficiency guard, not silently dropped.
4. No double-free risk: net-zero hammerer, self-terminating tasks reclaimed via `wait_for_termination_safe` + `drain_zombie_list` (magic-checked against 0xDD-poisoned reaps), no manual delete on drained blocks.
5. No kernel-invariant interference: atomic cross-task flags, atomically-registered task triples, `user_data` read only by test code, cleanup strictly before asserts (cookbook Rule 5).
6. Preprocessor symmetry: registered in all/all-2 consistent with stress_hrt precedent (not all-1); expected counts internally consistent (all 1026->1030, scheduler_hrt=4); MAX_TESTS 1026->1032 gives 2-slot headroom over the 1030-test "all" class (previously zero headroom).

DECISION: APPROVED