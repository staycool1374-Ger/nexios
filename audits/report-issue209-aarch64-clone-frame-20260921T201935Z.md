# AUDIT REPORT 20260921T201935Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/task/task.cpp, src/kernel/arch/aarch64/test_aarch64.cpp, src/kernel/test/test_expected_counts.hpp

## FINDINGS
- [S3] src/kernel/task/task.cpp:clone kstack else-fallback — riscv64 kernel-parent clone behavior also changes (raw-phys VA -> HHDM VA) with no riscv64 readback test; same bug class, correct direction, but uncovered on that arch
  WHY: The #else branch covers all non-x86 so riscv64 inherits the fix, yet only aarch64_clone_frame_readback pins it — a riscv64 counterpart test or follow-up issue is warranted.
- [S3] src/kernel/task/task.cpp:canary_install_kernel_stack — any claimed kernel-stack-canary side benefit for kernel-parent clones is FALSE; the installer is only reached inside the `if (is_user_task)` block (task.cpp:~1674) and kernel-parent clones still skip it post-fix
  WHY: Verified canary_install_kernel_stack(tcb) call sites are create() (~1079), create_user (~1269), and clone() user-branch only (~1674) — the fixed fallback changes VA validity, not canary coverage (canary_installed bit stays 0 so canary_verify short-circuits true, no latent fault either way).
- [S3] Check 7 (graphify + vault retrieval artifacts on issue #209) — unverifiable by static auditor; developer asserts artifacts are pasted on the issue thread
  WHY: Auditor has no GitHub/vault channel in this environment, so check 7 is recorded as asserted-unverified rather than independently confirmed.

## VERIFIED (no finding)
- Fix shape: non-x86 kernel-parent branch now HHDM_OFFSET+kstack_phys (mirrors create() task.cpp:1047-1052); x86 branch byte-identical to HEAD (same TCB_WRITE + kstack_phys+STACK_SIZE lines); user-parent HHDM branch and alloc_kslot branch untouched.
- Root-cause plausibility: EL1-sync default_exception (vectors.S:63-75) logs ESR/FAR then ELR+=4 and erets, so faulting frame-build stores silently no-op — consistent with the claimed 36-no-op / stale-readback symptom.
- Teardown symmetry: fallback sets kstack_slot_va_=0/size=0, so cleanup() takes the else page-count ((STACK_SIZE+4095)/PAGE_SIZE, identical to clone's stack_pages) and skips unmap/free_kslot via the kstack_slot_va_ guard; destroy() REAPED-guard prevents double-cleanup — no leak/double-free introduced.
- Test exactness: asserts all 36 slots (f[0]==0 forced X0, f[1..30] echo 0x1000+i, f[31]=SP marker, f[32]=ELR, f[33]==SPSR 0, f[34..35] padding zero) plus top-sp==288 (36*8), matching the vectors.S save-area build order; success path tears down via terminate_and_drain+drain_zombie_list; failure-path early returns (JARVIS_ASSERT `return`, test.hpp:300-352) leak at most the unregistered clone child, rewound by snapshot_restore (test_isolate.cpp:687+, PMM/MemPool rewind + zombie drain + ResourceTracker baseline check) per accepted precedent.
- Arch gating + counts: whole file under #if CONFIG_ARCH_AARCH64 (test_aarch64.cpp:22) with registration inside; expected-counts 27->28 single guarded row; no x86 production change (x86 lines under #if CONFIG_ARCH_X86_64, identical semantics).
- TEMP-token sweep: zero matches for TEMP/PROBE/g_debug/dump/printf/serial/raw_write in the added diff lines; no probe leftovers.
- Critical checks 1-6: no new heap allocation in critical paths (static regs[37] only); no concurrency-boundary change (clone-from-harness without IrqGuard matches test_fpu_clone precedent; clone does not enqueue); no assertion masking (strict exact-value asserts); no critical-section interference; preprocessor blocks symmetric with no uninitialized variables.

DECISION: APPROVED
