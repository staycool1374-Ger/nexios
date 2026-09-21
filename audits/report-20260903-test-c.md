# AUDIT REPORT 20260903-170414

PATCH: audits/pending_patch.diff (iteration 2 — corrective patch for the S2 finding of report-20260903-165844)
FILES: docs/specs/fpu-context.md, src/kernel/core/global_state.cpp, src/kernel/kernel.cpp, src/kernel/task/scheduler.hpp, src/kernel/task/task.cpp, src/kernel/task/task.hpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_fpu_inv.cpp, src/kernel/test/test_isolate.cpp, src/kernel/test/test_registry.cpp

## PREVIOUS FINDINGS — REMEDIATION VERIFIED

- **[S2] Cross-arch build break — REMEDIATED.** test_fpu_inv.cpp:52 `#if defined(CONFIG_ARCH_X86_64)` wraps the entire anonymous namespace (helpers `force_nm`/`ts_set`) AND all four JARVIS_TEST bodies; `#endif` at :196. Registration calls are guarded separately (:200–:205) and only fire on x86_64. `register_fpu_inv_tests()` is defined unconditionally on ALL arches (:198), so the registry linkage (declaration test_registry.cpp:189, `fpu_invariants` class :316, calls :649/:800) stays intact off-x86.
  - VERIFIED (a): guard placement correct — whole test bodies incl. helpers, registration only on x86_64, registry function defined on all arches.
  - VERIFIED (b): actual cross-compiles with `-Wall -Wextra -Werror` (the tree's CXXFLAGS):
    - `aarch64-elf-g++ … -DCONFIG_ARCH_AARCH64 -march=armv8-a` → clean
    - `riscv64-elf-g++ … -DCONFIG_ARCH_RISCV64 -march=rv64imafdc` → clean
    - `x86_64-elf-g++ … -mgeneral-regs-only` → clean
    The unused `FPU_PI_BITS`/`FPU_EULER_BITS` constexprs (file scope, outside the guard) do NOT warn on aarch64/riscv64 (GCC C++ does not enable `-Wunused-const-variable` under `-Wall/-Wextra`), confirmed by the clean compiles.
- **[S1] Stale-restore fix — RE-CONFIRMED correct.** kernel.cpp:1448–1454: `if (prev_fpu_owner == current)` → store `fpu_owner` (redundant no-op), record depth, `return` with registers untouched. The fxsave at :1445 fires only when `prev && prev != current`. No path fxrstor's when prev==current. CR0.TS was already cleared at :1432–1434 before the branch, so the owner continues with live registers and no further #NM. Invariant `fpu_owner != nullptr ⇒ fpu_owner->fpu_used` holds (owner is only ever set in the #NM handler after fpu_used is established), so skipping `fpu_used=true` in the early-return is safe.
- **[S3] docs "NOT a gap" overclaim — NON-BLOCKING, now code-verified.** scheduler.cpp:2555–2557 (deferred-switch arm) and :2816–2818 (reschedule) BOTH set CR0.TS on switch-away; the doc's §0 claim matches the code. The residual cross-ISR subtlety (handler clears TS in the armed window; a switch applied later by the ISR epilogue would run with TS=0) is PRE-EXISTING — the old handler cleared TS identically — and the S1 fix strictly improves that window (registers now hold live state, not clobbered state). Not introduced, not worsened, not blocking.
- **[S3] EOF newline — RESOLVED.** test_fpu_inv.cpp ends `}\n` (hexdump verified); no `\ No newline at end of file` marker in the diff.

## CRITICAL CHECKS (re-run on full patch)

1. **S1 stale-restore (no fxrstor when prev==current):** PASS — kernel.cpp:1448–1454 early-returns before any fxrstor; fxsave gated on `prev && prev != current`.
2. **INV-FPU2 cli safety:** PASS — `arch::cli()` first statement of the #NM branch; vector 7 is an interrupt gate (idt.cpp:66 `0x8E` → IF cleared on entry, verified); kernel is `-mgeneral-regs-only` (Makefile:131) so kernel ISR code can never raise #NM; `isr_common` re-cli's after the C handler (isr_stubs.asm:156); `iretq` restores the interrupted RFLAGS so no `sti` needed. SMP-proof, no IF inversion on any exit path.
3. **INV-FPU1 no allocation:** PASS — handler executes only CR0 RMW, atomic loads/stores, fxsave/fxrstor/fninit/ldmxcsr, integer compares, and a depth max update. `fpu_nm_no_alloc` asserts PMM `pool_used_pages` and full ResourceTracker deltas == 0 across a 200-iteration forced-#NM storm.
4. **fpu_state_gen unconditional bump:** PASS — bumped at exactly the two fxsave sites (#NM kernel.cpp:1446, clone task.cpp:1386), ctor-initialized to 0 (task.hpp:220), copied to child on clone (task.cpp:1393, all arches); not CONFIG_DEBUG-gated → debug/release symmetry.
5. **alignas(64) layout shift:** PASS — no `offsetof(TaskControlBlock, fpu_state)`/hardcoded-offset consumer in the tree (name-accessed only; the write-log tracer's `offsetof(TaskControlBlock, field)` macro computes offsets dynamically and is unaffected); MemPool TCB blocks are guaranteed ≥64-aligned (page-aligned base + power-of-two block sizes ≥1024, mempool.cpp:40–41); ctor init-list order matches declaration order (fpu_used, fpu_state_gen, fpu_state).
6. **Debug/release symmetry:** PASS — all production changes (gen, depth_max, cli, stale-restore) are unconditional; no `#ifdef CONFIG_DEBUG` control-flow divergence.
7. **Test robustness / no Heisenbug:** PASS — `fpu_nm_own_arm_no_clobber` arms TS before its first FPU op so the first #NM is the init path (owner=harness) despite snapshot_restore's owner=null reset, then `force_nm()` genuinely hits prev==current (step B). The x87 round-trip is exact-bit (fldl m64 double → 80-bit x87 → fstpl, lossless; fldl only ever loads 64-bit doubles, never a double-rounding path). Without the fix, the premature fxrstor loads a never-saved (zeroed or stale) buffer, so r2 != pi and the test fails deterministically; with the fix it passes. The nesting assert `fpu_nm_depth_max <= baseline + 1` is deterministic: interrupt gate + cli exclude a nested timer inside the swap, and kernel ISR code contains no x87, so #NM only ever enters from non-ISR context (handler-visible depth = interrupted depth + 1 ≤ baseline + 1).
8. **Registry/counts consistency:** PASS — `all` 1040→1044 (`1040 + fpu_invariants(+4,#93)`), `fpu_invariants` = 4; `register_fpu_inv_tests` is called exactly once per class path ("all" → full `register_all_tests()`:649; "all-2" → `register_all_tests_second_half()`:800 — separate runs, no double registration).

## VERIFIED BUILD STATE

All changed translation units compile clean under the tree's exact CXXFLAGS (`-Wall -Wextra -Werror`, debug) on all three targets:
- x86_64: test_fpu_inv.cpp, kernel.cpp, global_state.cpp, task.cpp, test_isolate.cpp, test_registry.cpp
- aarch64: test_fpu_inv.cpp, global_state.cpp, task.cpp, test_isolate.cpp, test_registry.cpp
- riscv64: test_fpu_inv.cpp, global_state.cpp, task.cpp, test_isolate.cpp, test_registry.cpp

No warnings, no errors. The S2 build break is gone on all supported arches.

## PATCH

Corrective patch as submitted (guards + registration hoisting). No rejected_patch.diff written — no S1/S2 findings remain. The two S3 notes are non-blocking: the docs claim is now code-verified (TS set at both publish sites) and the EOF newline is present.

DECISION: APPROVED