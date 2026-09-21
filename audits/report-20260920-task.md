# AUDIT REPORT 2026-09-20T09-30-00Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/kernel.cpp, src/kernel/syscall/syscall_handlers_misc.cpp, src/kernel/task/scheduler.cpp, src/kernel/task/scheduler.hpp, src/kernel/task/scheduler_errors.hpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_sched_helpers.hpp, src/kernel/test/test_smp_sched.cpp

## FINDINGS
- [S3] src/kernel/test/test_expected_counts.hpp:327 — count bump without in-patch tests
  WHY: deadline 115→125 adds no deadline tests in this patch (smp 15→17 exactly matches the 2 new tests), so the +10 relies on out-of-patch justification (issues #173/#76); direction is stricter (not masking), but the coupling should be recorded.
- [S3] src/kernel/task/scheduler.cpp:4580 — deferred-kill spare drops the kill intent instead of retaining it
  WHY: spared entries are abandoned when s_deferred_kill_count is reset to 0, so retry depends on future re-detection re-arming the kill rather than the "leave for a later tick" the comment claims; fail-safe (live task survives, no UAF/wedge), but compact-and-retain would match the drain/flush retry pattern.

DECISION: APPROVED
