# DESIGN REVIEW #611 r1 — AP boot mirrors CR4.SMAP/SMEP

Base: main @ 1f69f3f1d (verified `git log --oneline -1`; branch `main`).
Plan: audits/plan-611.md. Normative issue: #611 (OPEN, bug, milestone v0.6.1,
parent #606). Scope: DESIGN review of the plan only. Tree untouched, nothing
committed. #613 verified OPEN (aa64 PAN twin, filed, kept out — check 5).

## CHECKS (the 8 load-bearing items)

1. Fix-hunk exactness — PASS. Insertion after smp.cpp:368 `#endif`, before the
   line-370 start-gate comment, is the only correct slot: after the FSGSBASE
   write (fresh `read_cr4()` in INV-611-1 preserves bits 9/10/17/16), above
   the `scheduler_ready_` gate (INV-611-3, IF=0, no observer of half-armed
   CR4). Nesting mirrors the BSP ladder exactly (kernel.cpp:1141 outer
   `#if CONFIG_SMEP || CONFIG_SMAP` + nested `#if CONFIG_SMEP` / `#if
   CONFIG_SMAP` at 1156/1163); `#if CONFIG_SMEP` without `defined()` matches
   the BSP idiom and is safe because nexios_config.h:1083-1104 always defines
   both to 0/1. Single RMW gated on `guard_bits != 0` = fail-closed on
   feature-less silicon. INV-611-2's refusal to consolidate into the #156/#338
   writes is correct: rewriting the audited #338 hunk for 2 boot-path MOVs
   would invalidate prior approval for zero benefit.
2. AP-path probe safety — PASS (strict). Proposed `has_smep()` is
   instruction-identical to the in-tree `has_smap()` (cpuid_impl.hpp:118-120)
   and `has_fsgsbase()` (:128-130): inline, `cpuid(7,0).ebx & CONST`, no
   lock/alloc/global, no log. `cpuid()` itself (:44-50) is a bare `asm
   volatile("cpuid")` with register constraints only — no memory clobber, no
   fence beyond CPUID's own serializing semantics; side-effect-free as
   claimed. Callable with IF=0 (CPUID needs no interrupts), before
   Scheduler::init, before the start-gate — same contract as the
   `has_fsgsbase()` call at smp.cpp:364, whose acceptance in
   audits/report-2026-10-13T12-25-18Z.md check 2 was verified verbatim in the
   report file. Headers already included (smp.cpp:29 cpuid_impl.hpp, :32
   io_impl.hpp) — no include/link change. The max-leaf<7 residual is honestly
   carried as S3 with the exact prior-art cite; it is shared with all existing
   callers, so no new caller class is introduced.
3. BSP-literal collapse (step 4) — IN SCOPE, values VERIFIED. Line-by-line:
   `:1157 (1u << 7)` vs new `CPUID_EBX7_SMEP = 1u << 7` (EBX[7] SMEP feature)
   identical; `:1158 (1ULL << 20)` vs `CR4_SMEP = 1ULL << 20` identical;
   `:1164 (1u << 20)` vs `CPUID_EBX7_SMAP = 1u << 20` (EBX[20] SMAP feature)
   identical; `:1165 (1ULL << 21)` vs `CR4_SMAP = 1ULL << 21` identical. The
   added `!= 0` is a no-op in boolean context; types (u32 feature / u64 CR4)
   preserved. This is not scope creep: the two-literal-sets divergence IS the
   defect class that produced #611, and the substitution is confined to the
   same `#if CONFIG_SMEP || CONFIG_SMAP` block with log lines and the #606
   latch store untouched. Regression cover named (arch class:
   smap_cr4_bit_set + SMEP/SMAP PF tests) is the right class.
4. SMEP+SMAP breadth — REQUIRED, not expansion. Issue title names
   "CR4.SMAP/SMEP" and the body states "never SMAP/SMEP — no CR4_SMAP/CR4_SMEP
   arm". Mirroring SMEP is inside the filed defect, and the nested-`#if`
   structure keeps SMEP-only / SMAP-only / neither configs exact.
5. Cross-arch — PASS. #613 exists (OPEN, "twin of #611", body cites the same
   disposition) and the plan keeps aa64 OUT — correct, there is no x86 CR4
   surface on aa64 and an SCTLR_EL1.PAN arm needs its own audit. RV64
   nothing-to-mirror PROVEN at src/kernel/arch/riscv64/hal/io_impl.hpp:108-109
   ("SUM is a standard sstatus field — no enable gate required") with
   unconditional stac()/clac() at :110-115. BSP-only `pan_init()` +
   reset-SCTLR premise (early_init.cpp:52-74, ap_entry ORs M|I) taken as cited;
   aa64 legs (arch_aarch64 + selftest) prove non-perturbation.
6. #606 no-regress — SOUND. Latch written once by the BSP at kernel.cpp:1155
   before SMP bring-up, read-only after; wrappers gate on the latch, not CR4
   (io_impl.hpp:160/170 `if (!g_smap_supported)` — verified). SMAP-less CPU:
   per-AP re-probe false on the same silicon, zero write, wrappers stay
   no-ops, #UD impossible. SMAP-capable: BSP bit already set pre-bring-up, AP
   bit purely additive. The AP-start→mirror-write window is acknowledged, not
   hidden: INV-611-3 (IF=0, pre-gate, no task/ISR observer) + INV-611-7
   (pre-gate body touches only percpu/LAPIC/TPR at smp.cpp:335-343; post-gate
   user copies are the shared stac/clac-wrapped C++ already exercised from the
   BSP). The window is pre-existing exposure being shrunk, not new exposure —
   the plan states this correctly via "APs ran unenforced for their whole
   lifetime" logic in the issue and INV-611-7.
7. Test sanctity + gates — PASS. Developer lands zero paths under
   src/kernel/test/ (stated + enforceable via `git status`). QE owns the new
   `smp_bringup_ap_cr4_guard_parity` test (x86-guarded whole-test +
   registration, UP-leg vacuous pass, g_ap_cpu==1 placement proof, iff-CPUID
   asserts 5b/5c, 4-bit parity assert 5d covering #156/#338 too, idle/drain/
   zero-delta teardown), the 5→6 / 81→82 / 22→23 count rows moved together
   (LEARNINGS #337 discipline), the pre-fix FAIL control (5b/5d must fail with
   AP bit clear), the KVM leg (`[qemu] KVM active` + 6/6 + 23/23) and the
   qemu64 no-feature negative leg. Gate list is complete: 3-arch build.sh,
   check-style Errors:0, arch/smp_bringup/smp_sched/smp_multicpu/core_basic/
   ipc/elf_loader/selftest, trailing run_all_classes.sh x86_64 debug, release
   `--class all`, off-arch arch_aarch64/arch_riscv64 + selftest, per-class
   history rows.
8. Retrieval + creep — REAL, NO CREEP. Graphify query+path with concrete
   USED/IGNORED dispositions; obsidian "SMAP"/"SMEP" cites verified real
   (design-review-606-r1 S3 ordering this issue; 2026-10-13T12-25-18Z check 2
   quoted verbatim; plan-606 latch contract), and the 0-match "AP bring-up
   CR4" gap honestly recorded with the doc-step consequence. Step 9 posts the
   work-begun comment with both artifacts BEFORE the first edit — check-7
   evidence satisfiable. No creep into #613 (filed, referenced), #568
   (untouched), or intake consolidation (explicitly refused, INV-611-2).

## FINDINGS

- [S3] audits/plan-611.md step 7 — "File the tracked follow-up issue" is
  stale: #613 (the aa64 PAN twin) already exists and is OPEN; filing again
  would create a duplicate.
  WHY: Step 7 predates #613 (created 2026-11-24T08:02Z); the implementation
  must reference #613's number in the commit message instead of filing anew.

DECISION: APPROVED
