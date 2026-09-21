# AUDIT REPORT 2026-09-21T14-10-15Z
PATCH: audits/pending_patch.diff
FILES: linker/linker_aarch64.ld, mk/rules.mk, src/kernel/memory/mempool.cpp, src/kernel/task/task.cpp, userspace/fork-marker.c (NEW, untracked — absent from the diff, audited from worktree)

## FINDINGS
- [S2] src/kernel/test/test_memory_safety.cpp:68-74 — CHECK 6 (#ifdef asymmetry): patch raises aarch64 pool8 to 16384 but this arch-independent test asserts MemPool::alloc(8193)==nullptr
  WHY: On aarch64 alloc(8193) now deterministically returns a pool8 block, so `memory_safety_mempool_alloc_large_rejected` fails under `make test-full aarch64` (which runs all classes, not just arch_aarch64).
- [S2] issue #104 thread — CHECK 7 (retrieval artifacts): work-begun comment shows a vault artifact with disposition (obsidian, 0 hits) but no graphify query/path artifact
  WHY: The pipeline mandates both graphify and vault retrieval evidence pasted with dispositions; graphify is absent.
- [S3] src/kernel/task/task.cpp:175-177 (pre-existing, out of patch scope) — aarch64 clone loop pushes regs ascending (regs[0] first), mirroring x1-x30 vs the save layout; x86 pushes descending and riscv copies directly
  WHY: Untouched by this patch and invisible to the fork-marker smoke test (only x0/SP/ELR/SPSR matter there), but the child's x1-x30 are permuted — recommend a follow-up issue, not a blocker here.
- [S3] linker/linker_aarch64.ld — markers emit 8-byte values (markers.cpp), not empty sentinels, so .vectors et al. shift intra-section by 8-16 B
  WHY: Benign — each output section re-ALIGNs at 4K, VBAR/symbols are relative, CRC is re-patched at link; mirror positions match linker_x86_64.ld exactly (incl. rodata end-marker after the CRC quad, data end-marker after .got).
- [S3] mk/rules.mk — `-Wl,-z,max-page-size=0x1000` is not literally a no-op on x86 (LOAD alignment 2M→4K changes x86 userspace ELFs, which share the generic %.c.elf rule)
  WHY: Benign direction — denser files, loader is p_align-agnostic (arch::PAGE_SIZE rounding only), tmpfs-cap-safe; the comment overstates but the flag is safe.
- [S3] mk/rules.mk:374 — FORK_MARKER_OBJ is linked into KERNEL_DEBUG only, unlike VERIFY_IMG_OBJ which is in both KERNEL_DEBUG and release KERNEL
  WHY: No consumer today (smoke test reverted pending #208), but a release-gated aarch64 fork test would miss the symbols — flag for the #208 follow-up.
- [S3] packaging — userspace/fork-marker.c is untracked and missing from pending_patch.diff while rules.mk references its .elf product
  WHY: Committing only the 4 diff files breaks the aarch64 build (missing FORK_MARKER_SRC); the new file must be `git add`ed with the commit. TEMP-token sweep of diff + new file: 0 hits.

## VERIFIED CLAIMS (no finding)
- Clone indices regs[31/32/31→33]: triangulated against vectors.S save_all (x0-x30@0-30, SP_EL0@248, ELR@256, SPSR@264), syscall_entry.S layout comment, and restore_all offsets (248/256/264, x0 from [sp,#0]) — old regs[17/19/20] were user x17/x19/x20, fix correct; stack[0]=0 zeroes exactly the slot restore_all reads as x0.
- static_assert fail-safe: any TCB growth past pool8 fails the build, not the boot; pool counts unchanged (64); x86/riscv size path (#else 8192) untouched.
- Checks 1/2/4/5: no new heap allocation in critical paths (static_assert is compile-time; clone pushes to the preallocated kstack; init runs once under the existing lock), no concurrency-boundary change, no free-path change, no test assertion masked (no test file touched).
- fork-marker.c uses only in-tree libc surface (fork/write/waitpid/_exit/pid_t, all in src/libc/unistd.h); userspace-only, cannot affect the kernel build.
- Embed rules mirror the VERIFY_IMG pattern (strip + objcopy binary + 3 symbol renames); AARCH64_TRIPLET/OBJCOPY_FMT/OBJCOPY_ARCH defined for aarch64; aarch64-gated ifeq/else-empty mirrors the VERIFY precedent; FORK_MARKER_OBJ present in both KERNEL_DEBUG prereqs and link recipe.

## PATCH
`audits/rejected_patch.diff` was written (git apply --check clean) and gates the `memory_safety_mempool_alloc_large_rejected` bound per arch (16385 on aarch64, 8193 elsewhere), following the established `#if defined(CONFIG_ARCH_AARCH64)` test convention. The CHECK-7 finding needs no code patch — post the graphify query plus disposition on issue #104.

DECISION: REJECTED
