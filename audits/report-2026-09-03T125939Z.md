# AUDIT REPORT 2026-09-03T125939Z
PATCH: audits/pending_patch.diff
FILES: Makefile, tools/qemu/trace_events.txt, src/kernel/test/test_stress_hrt.cpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_expected_counts.hpp

## FINDINGS
- [S2] src/kernel/test/test_stress_hrt.cpp:256 — Heisenbug-masking: failed IPC samples are silently dropped
  WHY: `elapsed = (ok && t1 > t0) ? t1 - t0 : 0` excludes every failed `send_sync` from avg/p95/max/hist, and the only guard (`stress_nz >= k_iterations/8` = 250/1000, lines 333-334) lets up to 75% of phase-B IPC calls fail while the test still passes — for a hard-RT class whose purpose is to detect IPC degradation under stress, failing IPC is the strongest signal and must be counted and asserted, not discarded.
- [S3] src/kernel/test/test_stress_hrt.cpp:93-104 — hist_p95 returns the bucket lower bound (1<<b), underestimating the true p95 by up to 2x
  WHY: the p95 assert compares an underestimate against an ~8x headroom bound, so it cannot practically false-pass, but the estimate should be documented as a lower bound or use the bucket midpoint.
- [S3] src/kernel/test/test_stress_hrt.cpp:299-312 — the k_pathological_cap "wedge detector" is unreachable in the worst case; a wedged scheduler surfaces as a 220s host-watchdog HANG instead of a clean assert
  WHY: wait_for_termination_safe has no timeout and the drive-loop cap is 30000 ticks, so a wedged scheduler hangs the harness until WATCHDOG_STALL (220s) kills QEMU — still a visible failure (not masking), but the issue's wedge-detector claim is overstated.
- [S3] src/kernel/test/test_registry.cpp:455/630/778 — stress_hrt registration is unconditional while test_expected_counts.hpp lists aarch64/riscv64=0 (count validation skipped) and the absolute caps (k_abs_floor=10M, k_pathological_cap=20M) are x86_64-TCG-calibrated
  WHY: consistent with the existing bench-*/unconditional-registration convention and all cross-arch `arch::rdtsc/hlt/Timer` APIs exist, but a non-x86 TCG counter rate could false-fire the calibrated absolute bounds; arch-gating or per-arch calibration would be safer.

## PATCH
REJECTED — audits/rejected_patch.diff written; intent: count per-phase send_sync failures (g_base_fail/g_stress_fail) and assert both == 0 so IPC failures under stress are surfaced instead of silently excluded from the latency metrics.

DECISION: REJECTED