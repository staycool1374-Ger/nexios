# AUDIT REPORT 2026-08-31T175512Z

RE-AUDIT of report-2026-08-31T174921Z (DECISION: APPROVED, 7×S3). Scope: the
deltas applied since that approval — (1) case-20/#XM label, (2) `name+2`
initrd fallback removal, (3) exc_table added to the standard gates +
count-table updates. Audited against the regenerated patch at
audits/pending_patch.diff (git diff main, full #91 change set).

PATCH: audits/pending_patch.diff
FILES (deltas re-audited): src/kernel/kernel.cpp, src/kernel/test/test_exc_table.cpp,
src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_registry.cpp,
src/kernel/test/test_weak_stubs.cpp, test-history.txt
FILES (carried from prior APPROVED audit, unchanged): src/kernel/arch/x86_64/isr_stubs.asm,
userspace/de-probe.c, userspace/ud-probe.c

## FINDINGS

### Delta 1 — case-20 label correction (cosmetic, no new issue)
- kernel.cpp:1204-1207 now returns "SIMD FP Exception (#XM)" for vector 20 and
  "Control Protection (#CP/#VE)" for vector 21 (plus new names for 28/29). The
  mislabel from the prior S3 is gone.
- `exception_name()` is used only on fail-stop log paths
  (kernel.cpp:1455, 1515, 1552 — all panic/fatal branches; vector number is
  also printed in the same message). String-only change: no control-flow,
  dispatch, or frame impact. Confirmed no behavioral delta.

### Delta 2 — initrd `name+2` fallback removal (cleanup, no new issue)
- test_exc_table.cpp:84 `run_fault_probe` now performs a single
  `initrd::find(name)`; on miss returns false → `err_macro_consistency` takes
  the `JARVIS_TEST_PASS()` skip path (mask gate still enforced).
- The removed fallback could never match ("-probe.c.elf" for a bare name), so
  removal is behaviorally neutral. Probes remain guaranteed present: built by
  the `userspace/%.c.elf` pattern rule (mk/rules.mk:179) and copied to the
  cpio root (mk/rules.mk:192); `initrd::find` normalizes "./" on both sides
  (src/initrd/initrd.cpp:96-98). Real #UD/#DE probes therefore still run.

### Delta 3 — registry + count-table (verified sound)
- **No double-registration within `all`:** `register_exc_table_tests()` is
  declared (test_registry.cpp:88), registered as the standalone `exc_table`
  class (test_registry.cpp:392), and invoked exactly ONCE in
  `register_all_tests()` (test_registry.cpp:513) and once in
  `register_all_tests_first_half()` (test_registry.cpp:662) — NOT in
  `second_half`, NOT in `safe`. Each gate executes exactly one class
  (test-config), and `Registry::init()` (kernel.cpp:954) runs once at boot
  before the class loop; each class invocation starts from a fresh registry,
  so a single `all` run registers the 3 exc_table tests exactly once.
- **Weak stub** (test_weak_stubs.cpp:53) keeps `register_all_tests()`
  linkable on non-x86_64 builds where test_exc_table.cpp is compiled out
  (`#if CONFIG_ARCH_X86_64`) — same pattern as register_idt_tests.
- **Count arithmetic consistent with measured runs (test-history.txt):**
  `all` = 990 (pre-#100) + 3 (memory_pmm 5→8, #100) + 3 (exc_table, #91) =
  996; debug `all` measured 981 executed + 15 TF_BENCH filtered = 996
  registered (row 2026-08-31 19:52:53). `safe`/`selftest` = 133 (85 TF_RELEASE
  + 48 TF_KERNEL); release selftest measured 133 registered / 85 executed / 0
  failures (19:41:00). `exc_table` = 3, measured 3/3/0 (19:51:23).
  `validate_class_count` is warn-only and the header values match the
  measurements. No [TCOUNT] MISMATCH claimed; values are self-consistent.
- **Armed-latch discipline holds under the `all` shared environment:** the 3
  tests are all TF_KERNEL and execute among the 981. `err_macro_consistency`
  never arms any latch — the real #UD/#DE fault probes run with the probe
  DISARMED and terminate via the normal v<32 signal path (SIGILL/SIGFPE),
  confirmed by the 3/3 probe evidence. `ve_cp_frame_layout` and
  `reserved_vec_panics` arm only inside `arch::IrqGuard` scopes (cli(), IF=0)
  and disarm before scope exit — `IrqGuard` restores IF in its destructor
  even on a JARVIS_ASSERT early-return, so IF cannot be left disabled and no
  maskable interrupt can be swallowed while armed. No persistent armed state
  crosses a test boundary on the passing path (981/0). Residual (unchanged,
  carried from the approved audit): the NMI-ignores-IF theoretical hole in a
  microsecond armed window is self-detecting (surfaces as a `g_probe_vec`
  assert failure) and test-scoped only.

## VERIFIED (no findings)

- Probe/reserved hooks and the reserved-vector fail-stop branch (kernel.cpp
  §3.3/§3.4) are byte-identical to the approved patch — weak defaults are
  production no-ops/panics; strong overrides latch only when armed.
- isr_stubs.asm uniform-frame `%assign` audits, 21/28 ISR_ERR classification,
  and the exported `__isr_vectors_err_mask` are unchanged from the approved
  state (mask still resolves to {8,10,11,12,13,14,17,21,28,30}).
- No new allocation, scheduler, or ResourceTracker leak paths; probe teardown
  uses the pre-approved load/terminate/drain pattern.

## PATCH

Diff-patch protocol not triggered: no REJECTED findings; decision below.

DECISION: APPROVED