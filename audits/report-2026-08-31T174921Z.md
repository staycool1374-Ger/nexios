# AUDIT REPORT 2026-08-31T174921Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/arch/x86_64/isr_stubs.asm, src/kernel/kernel.cpp, src/kernel/test/test_exc_table.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_weak_stubs.cpp, test-history.txt, userspace/de-probe.c, userspace/ud-probe.c

## FINDINGS

- [S3] src/kernel/kernel.cpp:1204-1205 — log-only mislabel: `case 20` is printed "Virtualization Exception (#VE)", but vector 20 is #XM (SIMD FP Exception) per the spec table (exception-table-audit.md §2); #VE is vector 21 (case 21 already labels #CP/#VE). A real #XM panic would be misattributed to #VE. No behavioral effect (log string on the fail-stop path only; vector number is also printed), but it contradicts the spec's own §3.3 "vector-name map for panics" rationale.
  WHY: Diagnostic forensics accuracy only — control flow, dispatch, and the reserved/fail-stop decision are unaffected.

- [S3] src/kernel/kernel.cpp:1452-1458 — deliberate, spec-mandated fault-isolation → fail-stop policy shift: reserved/vendor vectors (15, 22-26, 29, 31) previously reached the generic `vector < 32` path (user-mode → exception_to_signal default → SIGSYS, task terminated, kernel survived; kernel-mode → generic CPU EXCEPTION panic); the new branch panics for ALL modes via `reserved_exception_hook`. Spec §3.4 requires this and none of these vectors fire from normal hardware (15 is reserved; 29 only under AMD SEV-ES, unsupported; 22-26/31 reserved), so no legitimate path is lost. Two corollaries to record: (a) the branch sits AFTER the `g_user_access_recover_ip` redirect, so a reserved-vector fault during a safe_copy still recovers (copy returns false) rather than fail-stopping — arguably correct fault isolation, but the fail-stop is intentionally bypassed inside the copy window; (b) vectors 21/28 (now ERR) and 29 (still NOERR, AMD #VC pushes an error code) only ever iretq through isr_common on hosts that raise them — for 29 the frame would be misparsed, but the fail-stop branch panics before the epilogue, so the misparse is log-noise, not a fault.
  WHY: Documented change; safety posture is correct for a SIL kernel (an unexpected reserved vector indicates an invariant/VMM misconfiguration). Flagging the policy implication as required.

- [S3] src/kernel/kernel.cpp:1426 — the probe hook is invoked on EVERY interrupt and syscall dispatch (weak default returns false). A single well-predicted function call per ISR/syscall; negligible, but it is an unconditional addition to the timing-sensitive dispatch hot path.
  WHY: Performance note only; production semantics are byte-identical (probe is a no-op).

- [S3] src/kernel/test/test_exc_table.cpp:86 — `initrd::find(name + 2)` fallback is broken dead code: callers pass bare names ("ud-probe.c.elf"), so `name+2` yields "-probe.c.elf" (never present); the intended "./"-strip fallback is unnecessary because `initrd::find` normalizes "./" on both sides (src/initrd/initrd.cpp:100-103). The primary `find(name)` always succeeds (probes are built by the `userspace/%.c.elf` pattern rule and copied into the cpio root). If the primary find ever failed, the probe gate would silently skip (JARVIS_TEST_PASS branch) — the fallback would not save it.
  WHY: Harmless today (target absent), but dead buggy code that weakens the "real-fault probes always run" guarantee if the primary lookup path ever changes.

- [S3] src/kernel/test/test_registry.cpp:392 — `register_exc_table_tests()` is registered only as the standalone `exc_table` class; it is NOT added to `register_all_tests()` (line 469+). The `%assign` frame audits pin per-vector push DEPTH (verified: they fire on drift) but cannot catch a silent 21/28 classification swap (frame sizes are identical either way) — only `err_macro_consistency` (the exported `__isr_vectors_err_mask` gate) catches that, and it does not run in the debug `all` / release `all` / selftest gates. Spec §4 asks for regression validation in `all`/selftest.
  WHY: Test-coverage placement only; a future reversion of the #91 fix would go unnoticed by the standard gates.

- [S3] src/kernel/test/test_exc_table.cpp:44-56,143-150 — the armed-latch discipline relies on IF=0 (IrqGuard) so no maskable interrupt can be swallowed while armed; disarming happens inside the guard scope before IF is restored. Verified sound. The one theoretical hole: NMIs ignore IF, and the probe latches ANY vector while armed, so an NMI arriving in the microsecond armed window would be swallowed — test-mode only, and it would surface as a `g_probe_vec` assert failure (self-detecting) rather than silent corruption.
  WHY: Confirms the requested "can never swallow a REAL interrupt" check; residual risk is theoretical and test-scoped.

- [S3] src/kernel/test/test_expected_counts.hpp:20,23 — the `safe`/`selftest` 132→133 and `all` 990→993 corrections (the latter attributed to memory_pmm 5→8 from #100) are stale-count fixes, not test additions, and the gate is warn-only (test_registry.cpp:779 ignores the return value). I could not statically re-derive the `safe` registered count (spans 9 registration functions), so this rests on the developer's measured count.
  WHY: Count-table hygiene; a wrong entry cannot fail a run, only emits a [TCOUNT] MISMATCH warning.

## VERIFIED (no findings)

- Frame layout: both macro classes reach `isr_common` with [rsp]=vec, [rsp+8]=err (NOERR synthetic 0 / ERR CPU code); `isr_common` GPR/vec/err/RIP indexing (`[rsp+15*8]`/`[rsp+16*8]`/`[rsp+17*8]`), the `add rsp,16` epilogue, and the `__isr_vector` label table are untouched and correct. Standalone NASM verification: the `%assign`/%if audits assemble, evaluate the mask to exactly {8,10,11,12,13,14,17,21,28,30} matching `k_expected_err_mask`, and fire `%error` on frame-depth drift.
- Probe/reserved hooks: weak defaults in kernel.cpp mirror the proven stack_overflow_hook pattern; strong overrides (linked even in release) are exact no-ops/panic when unarmed, so production behavior is identical with or without the test TU. No locks, scheduler calls, or allocations in either hook; reserved branch runs IF=0 like the surrounding panic paths.
- No dynamic allocation or ResourceTracker leak paths introduced; probe tasks use the existing load/terminate/drain pattern, and the probe ELFs are built into the initrd (pattern rule `userspace/%.c.elf`), so the real #UD/#DE gates actually run.

DECISION: APPROVED