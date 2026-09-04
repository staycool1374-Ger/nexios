# AUDIT REPORT 20260903-165844
PATCH: audits/pending_patch.diff
FILES: docs/specs/fpu-context.md, src/kernel/core/global_state.cpp, src/kernel/kernel.cpp, src/kernel/task/scheduler.hpp, src/kernel/task/task.cpp, src/kernel/task/task.hpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_fpu_inv.cpp, src/kernel/test/test_isolate.cpp, src/kernel/test/test_registry.cpp

## FINDINGS
- [S2] src/kernel/test/test_fpu_inv.cpp:52-203 — cross-arch build break: the file ships unconditional x87 inline asm (`fnop`/`finit`/`fldl`/`fstpl`) and `arch::read_cr0`/`write_cr0`+CR0.TS semantics, yet is NOT in the mk/rules.mk:29 filter-out list, so `SRC_CXX_GENERIC` (rules.mk:15) compiles it on aarch64/riscv64.
  WHY: Verified `aarch64-elf-g++ -c` and `riscv64-elf-g++ -c` fail ("unknown mnemonic `fnop'`/`finit'`/`fldl'`/`fstpl'"), breaking the supported `make debug ARCH=aarch64|riscv64` / `renode-test` builds; corrective patch guards the x86-only bodies with `#if defined(CONFIG_ARCH_X86_64)` (register_fpu_inv_tests stays defined on all arches, registering 0 tests off-x86).
- [S3] src/kernel/kernel.cpp:1431-1434 — the handler clears CR0.TS unconditionally; if a deferred switch armed in a prior ISR applies AFTER a prev==current #NM in the window, the switched-to task runs with TS=0 and its first FPU op does not #NM, so the outgoing owner's live FPU state is never saved to its TCB (the S1 fix correctly leaves the registers untouched, but the lazy-save trigger is lost; identical TS-clearing exists in the old code, so this is pre-existing, not a regression).
  WHY: docs/specs/fpu-context.md §0 "CR0.TS-on-switch-away: verified NOT a gap" overstates coherence for the cross-ISR arm path — worth a spec note, not a blocker for #93.
- [S3] src/kernel/test/test_fpu_inv.cpp — missing trailing newline at EOF.
  WHY: cosmetic lint defect only, no functional impact.

## Verified OK
- **S1 stale-restore fix (CRITICAL CHECKS #1):** old code conditionally fxsave'd (`prev != current`) but UNCONDITIONALLY fxrstor'd `current->fpu_state` when `current->fpu_used` — in the armed-switch window (prev==current) it therefore clobbered live registers with stale/zero TCB state. New `if (prev == current)` branch returns after clearing TS with registers untouched; `fpu_owner` is stored (redundant no-op) and no state must persist; the init path (prev==nullptr) is intact; the "registers always hold owner's live state" invariant is sound because the kernel is `-mgeneral-regs-only` (Makefile:131) so no other code writes FPU regs.
- **INV-FPU2 (cli, CHECKS #2):** vector 7 is an interrupt gate `0x8E` (idt.cpp:66) → IF cleared on entry, valid kernel stack via TSS.RSP0; `arch::cli()` (io_asm.asm:88) runs before TS-clear so no IRQ can nest into the swap; `isr_common` cli's after the C handler (isr_stubs.asm:156) and `iretq` restores the interrupted RFLAGS — no path returns to user with wrong IF.
- **INV-FPU1 (CHECKS #3):** #NM path performs only CR0 RMW, atomic loads/stores, fxsave/fxrstor/fninit/ldmxcsr, and integer compares — no allocation.
- **fpu_state_gen (CHECKS #4):** bumped exactly at the two fxsave sites (#NM kernel.cpp:1446, clone task.cpp:1386), ctor-init 0, copied to child on clone (all arches), unconditional in both debug and release.
- **alignas(64) (CHECKS #5):** no `offsetof`/layout consumer of `fpu_state` in the tree (grep: name-accessed only); ctor init-list order matches declaration order (fpu_used, fpu_state_gen, fpu_state); MemPool blocks are ≥64-byte aligned (page-aligned base + power-of-two block_size ≥ sizeof(TCB), a multiple of 64) so the runtime alignment assert is deterministic, and TCB pool capacity is unchanged (2048/8192 class, 64 blocks = MAX_TASKS).
- **Debug/release symmetry (CHECKS #6):** all production changes are unconditional; no `#ifdef CONFIG_DEBUG` control-flow divergence.
- **Test robustness (CHECKS #7):** `fpu_nm_own_arm_no_clobber` arms TS before its first FPU op so the first #NM is the init path (owner=harness) despite snapshot_restore's owner=null reset, then `force_nm()` genuinely hits prev==current; the x87-stack round-trip (euler→r1, pi→r2) is exact-bit, so the test fails deterministically on the old code (fxrstor of a never-saved buffer) and passes on the new code.
- **Determinism (CHECKS #8):** memory-operand x87 + exact 64-bit constants (no rounding), fxsave/fxrstor fully emulated by TCG; `fpu_nm_depth_max <= baseline + 1` is deterministic because the interrupt gate + cli exclude a nested timer inside the swap; the 200-iteration storms complete far below the 1 ms tick window.

## PATCH
audits/rejected_patch.diff was written: adds `#if defined(CONFIG_ARCH_X86_64)` around the x86-only test bodies/helpers of test_fpu_inv.cpp (register_fpu_inv_tests remains defined on all arches) so the aarch64/riscv64 builds no longer assemble x87 mnemonics. Verified `git apply`-clean against the patched tree and re-verified compilation on aarch64/riscv64/x86_64.

DECISION: REJECTED