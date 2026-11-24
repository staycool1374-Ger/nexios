# PLAN: #611 — AP boot mirrors CR4.SMEP/SMAP (close the #156/#338 parity gap in ap_main)

Base: main @ 1f69f3f1 ("#605 libc ABI mirror: SYS_MAX 105 + VSPACE 103/104 wrappers"), tree clean apart
from scratch files + test-history rows (untouched). Issue #611 (S3, bug, milestone v0.6.1), parent #606.

## AFFECTED FILES
- src/kernel/arch/x86_64/hal/cpuid_impl.hpp — add CPUID_EBX7_SMEP = 1u << 7 next to CPUID_EBX7_SMAP
  (line 78) and a has_smep() inline probe next to has_smap() (lines 113-120). Needed because no SMEP
  feature constant/helper exists today; the BSP probe uses a bare (1u << 7) literal.
- src/kernel/arch/x86_64/hal/io_impl.hpp — add CR4_SMEP = 1ULL << 20 and CR4_SMAP = 1ULL << 21
  after CR4_FSGSBASE (line 86), before the INVPCID block (line 88). Only CR4_PCIDE/CR4_FSGSBASE
  exist today; SMEP/SMAP are bare literals in kernel.cpp only.
- src/kernel/arch/x86_64/hal/smp.cpp — ap_main() (lines 332-395): insert one SMEP/SMAP mirror block
  after the #endif at line 368 and before the start-gate at line 373. This is the whole defect.
- src/kernel/kernel.cpp — lines 1157-1165 (BSP SMEP/SMAP arm inside #if CONFIG_SMEP || CONFIG_SMAP):
  substitute the new named constants for the bare 1u<<7 / 1u<<20 / 1ULL<<20 / 1ULL<<21 literals.
  Behavior-identical; removes the second source of truth for the bit positions — the exact divergence
  class that produced #611.
- docs/specs/per-cpu-smp.md — AP bring-up contract section 3.4.3 bullet list, after the FPU-tripwire bullet
  at lines 200-205: add the protection-bit mirror bullet.
- docs/specs/memory.md — REQ-MP-05 row (line 203): note that BSP *and* AP arming is HW-enforced on x86_64
  (per-CPU parity), so the requirement no longer overstates UP-only.
- TEST-SIDE, QE/testbed only — developer lands NOTHING here (test sanctity):
  src/kernel/test/test_smp_bringup.cpp (new test), src/kernel/test/test_expected_counts.hpp
  (smp_bringup 5→6, smp 81→82, smp_multicpu 22→23).

## INVARIANTS & CONCURRENCY BOUNDARIES
- INV-611-1 (additive-only): the AP write may only *add* CR4_SMEP/CR4_SMAP to the value it reads back; it
  must never clear OSFXSR/OSXMMEXCPT (bits 9/10), PCIDE (17) or FSGSBASE (16) already written at lines 359
  and 366. Enforced by read-modify-write on a fresh read_cr4() taken *after* the FSGSBASE block.
- INV-611-2 (write ordering): three sequential CR4 writes remain (OSFXSR|OSXMMEXCPT|PCIDE → FSGSBASE →
  SMEP|SMAP), each a fresh read-modify-write; write_cr4() carries a "memory" clobber so they cannot be
  reordered or coalesced. Same shape the auditor already accepted for #338
  (audits/report-2026-10-13T12-25-18Z.md check 2: "AP mirror in ap_main pre scheduler_ready gate with
  pure-CPUID re-probe; no locks required, none added"). No consolidation into a single write: that would
  rewrite the audited #338 hunk for zero benefit (2 extra MOVs per AP, once, boot path).
- INV-611-3 (no window): the block sits above the scheduler_ready_ start-gate (line 373), i.e. while the AP
  still has IF=0 and cannot dispatch any task, so no ISR/task on the AP can observe a half-armed CR4 and no
  AP-side user-memory deref can precede the arm.
- INV-611-4 (probe safety, item b): has_smap()/has_smep() are inline, allocate nothing, take no lock, touch
  no global, and are a pure cpuid(7,0) (leaf 7 subleaf 0). Callable with IF=0, before Scheduler::init,
  before scheduler_ready_ — *identical* instruction-level contract to has_fsgsbase(), which has been called
  at this exact site since #338 (smp.cpp:364) and which the auditor accepted in the report cited above.
  cpuid_impl.hpp and io_impl.hpp are already included at smp.cpp:29/32 — no include or link change, no new
  critical section, no new IrqGuard, no arch::pause() loop.
- INV-611-5 (config parity with the BSP): the block is gated #if defined(CONFIG_ARCH_X86_64) &&
  (CONFIG_SMEP || CONFIG_SMAP) with nested #if CONFIG_SMEP / #if CONFIG_SMAP, mirroring the BSP structure at
  kernel.cpp:1141-1173. A CONFIG_SMAP=0 build must not arm SMAP on APs (that would invert the asymmetry and
  fault AP-only).
- INV-611-6 (item h — no #606 regression): arch::g_smap_supported is a CPUID fact written once by the BSP at
  kernel.cpp:1155 before SMP bring-up and read-only afterwards; stac()/clac() legality is decided by CPUID,
  not by CR4. On a SMAP-less CPU the AP re-probe is false (same silicon), nothing is written, and the AP
  runs the gated no-op wrappers exactly as today — #UD remains impossible. On a SMAP-capable CPU the BSP
  already has the bit set before smp::bring_up() runs, so the AP bit is purely additive and the wrappers were
  already emitting stac/clac.
- INV-611-7 (no user-memory hazard on the AP): arming SMAP tightens AP enforcement. The pre-gate AP body
  (smp.cpp:335-343) touches only percpu/LAPIC/TPR state — no user memory. Every post-gate AP path that derefs
  user memory is the same arch-neutral C++ (syscall/copy/elf) that already runs from the BSP with the same
  stac/clac wrappers, so an unwrapped deref would already fault on the BSP (UP-visible). #611 adds no new
  class of fault; it makes APs match the BSP.
- INV-611-8 (item e — release identity): boot-path code, no #ifdef CONFIG_DEBUG, no new log line (the #338 AP
  block is silent too — a concurrent AP UART write buys nothing and can interleave with BSP serial),
  CONFIG_SMEP/CONFIG_SMAP are build-type-independent (nexios_config.h). Debug and release therefore reach the
  identical CR4 end state; nothing here changes control flow, failure policy or state transitions
  (CODING_STYLE section 6).
- INV-611-9 (item d — cross-arch parity):
  - riscv64: nothing to mirror. SUM is a plain sstatus field with no enable gate (io_impl.hpp:103-109) and
    stac()/clac() are unconditional, so RV64 APs already behave identically to the BSP.
  - aarch64: the task brief premise is incomplete — recorded here as evidence, not scope-creep. The AA64
    analog is *not* "per-task only": SCTLR_EL1.PAN (bit 23) is the per-CPU enable gate and it is NOT armed on
    APs — ap_entry.S:68-74 only ORs M|I (0x1001) into the reset SCTLR, and arch::pan_init()
    (early_init.cpp:52-74, which sets bit 23 then clac()) is BSP-only (kernel.cpp:1208-1217). So AA64 SMP
    runs with PAN inert on APs: the exact structural analog of #611. Disposition per the house rule
    (LEARNINGS #523/#542 style re-face: newly exposed arch bugs get filed issues, not scope-creep fixes):
    keep the #611 diff x86-only (there is no x86 CR4 surface to mirror on aa64) and file a tracked follow-up
    issue for "AP boot never arms SCTLR_EL1.PAN" in the same cycle, referenced from the commit message. The
    selftest/arch_aarch64 legs below prove this change does not perturb aa64.

- [retrieval] graphify query "AP bring-up ap_main CR4 write mirror PCIDE FSGSBASE SMAP SMEP" +
  graphify path ap_main -> write_cr4 — USED: confirmed the single x86 AP entry (smp.cpp:332), the
  write_cr4/cpuid homes, the three arch ap_main nodes (x86 332 / aa64 464 / rv64 541) that fix the parity
  surface to inspect, and the MP-4 spec node; IGNORED: the PCI/terminal nodes BFS dragged in.
- [retrieval] obsidian search_vault "SMAP" / "SMEP" — USED: audits/design-review-606-r1.md:108-111 (the S3
  finding that ordered this issue filed), audits/report-2026-10-13T12-25-18Z.md:13 (#338 AP-mirror
  probe-safety precedent), audits/report-20261122-224539-606.md:10 (CPUID max-leaf S3, pre-existing arm
  pattern), audits/plan-606.md:190-210 (latch contract). obsidian search_vault "AP bring-up CR4" — 0 matches:
  recorded gap (no vault note covers the AP CR4 contract), so the local docs/specs/per-cpu-smp.md and
  docs/specs/memory.md:203 get the doc step.

## STEP-BY-STEP CHANGES
1. src/kernel/arch/x86_64/hal/cpuid_impl.hpp: after CPUID_EBX7_SMAP (line 78) add
   inline constexpr uint32_t CPUID_EBX7_SMEP = 1u << 7; with a /// comment naming SMEP as the CR4.SMEP
   arming gate for BSP + AP (#611). After has_smap() (ends line 120) add
   inline bool has_smep() { return (cpuid(7, 0).ebx & CPUID_EBX7_SMEP) != 0; } carrying the same
   "boot-path use only" doc note has_smap() has (lines 113-117).
2. src/kernel/arch/x86_64/hal/io_impl.hpp: after CR4_FSGSBASE (line 86) add CR4_SMEP = 1ULL << 20 and
   CR4_SMAP = 1ULL << 21, each with a /// comment stating (a) the probe that gates it, (b) that it is armed
   on both the BSP and every AP, (c) issue #611. Do not touch the raw (1ULL << 9) | (1ULL << 10)
   OSFXSR/OSXMMEXCPT literals in smp.cpp:359 (not probed, out of scope).
3. src/kernel/arch/x86_64/hal/smp.cpp, ap_main(), insert immediately after the #endif at line 368 (below the
   FSGSBASE block) and before the start-gate comment at line 370:

```cpp
#if defined(CONFIG_ARCH_X86_64) && (CONFIG_SMEP || CONFIG_SMAP)
    // Issue #611: mirror the BSP SMEP/SMAP arms so this AP enforces
    // supervisor execute/access prevention identically (the #156 PCIDE and
    // #338 FSGSBASE mirrors above cover the other two probed bits).  Both
    // probes are pure CPUID — no locks, no globals, safe before the start
    // gate.  Bits accumulate into ONE write; nothing is written when the
    // CPU does not advertise the feature (fail-closed, matches the BSP).
    uint64_t guard_bits = 0;
#if CONFIG_SMEP
    if (arch::has_smep())
        guard_bits |= arch::CR4_SMEP;
#endif
#if CONFIG_SMAP
    if (arch::has_smap())
        guard_bits |= arch::CR4_SMAP;
#endif
    if (guard_bits != 0) {
        uint64_t cr4_guard = arch::read_cr4();
        arch::write_cr4(cr4_guard | guard_bits);
    }
#endif
```

   Constraints: keep it above the start-gate (INV-611-3); keep the fresh read_cr4() *after* the FSGSBASE
   write (INV-611-1); guard_bits/cr4_guard satisfy CODING_STYLE 10.6 (>=3 chars, descriptive). Indentation
   4 spaces, 80-column limit, braces on single-statement bodies (MISRA), per style 8 / 11.
4. src/kernel/kernel.cpp: in the #if CONFIG_SMEP || CONFIG_SMAP block replace, with no other edit:
   line 1157  if (ebx & (1u << 7))        -> if ((ebx & arch::CPUID_EBX7_SMEP) != 0)
   line 1158  cr4 |= (1ULL << 20);       -> cr4 |= arch::CR4_SMEP;
   line 1164  if (ebx & (1u << 20))      -> if ((ebx & arch::CPUID_EBX7_SMAP) != 0)
   line 1165  cr4 |= (1ULL << 21);       -> cr4 |= arch::CR4_SMAP;
   Keep the existing [BOOT] log lines and the g_smap_supported store at 1155 verbatim (bit-for-bit identical
   semantics: EBX[7]=SMEP feature, EBX[20]=SMAP feature, CR4[20]=SMEP, CR4[21]=SMAP — confirmed against
   nexios_config.h:1081-1104).
5. docs/specs/per-cpu-smp.md 3.4.3: after the FPU-tripwire bullet (lines 200-205) add one bullet —
   "Protection-bit mirror (issue #611): ap_main also mirrors CR4.SMEP/CR4.SMAP from the same pure-CPUID
   probes the BSP used (has_smep()/has_smap()), above the start-gate; the #606 g_smap_supported latch is a
   CPU-global CPUID fact, so a SMAP-less CPU keeps the bit clear on both CPUs and stac()/clac() stay no-ops
   (no #UD)."
6. docs/specs/memory.md REQ-MP-05 row (line 203): append "; armed on BSP *and* every AP (per-CPU parity,
   #611)" to the x86_64 enforcement cell.
7. File the tracked follow-up issue required by INV-611-9 ("aarch64: AP boot never arms SCTLR_EL1.PAN — PAN
   inert on APs"), body citing early_init.cpp:52-74 / ap_entry.S:68-74 / kernel.cpp:1208-1217, and reference
   its number in the #611 commit message. Record the riscv64 "nothing to mirror" disposition (SUM has no
   enable gate) in the same issue body.
8. Mirror the doc deltas into Obsidian (preserve frontmatter/wikilinks) and run graphify update . after the
   implementation lands (PROMPT-dev section 5).
9. Post the work-begun comment on issue #611 (branch main, plan path audits/plan-611.md, the two retrieval
   artifacts above) BEFORE the first source edit — check-7 evidence.

## TEST STRATEGY
- Developer: NO test files touched (test sanctity; git status must show zero paths under src/kernel/test/).
  QE owns the test half on testbed.
- New test (QE, src/kernel/test/test_smp_bringup.cpp, class smp_bringup — the harness passes -smp 2 for it,
  qemu_test.py:255): smp_bringup_ap_cr4_guard_parity. Doc-block "PRE: iocd | POST: none" (same markers as
  smp_sched_ap_runs_pinned). Whole test AND its registration inside #if defined(CONFIG_ARCH_X86_64) (aa64/
  rv64 read_cr4() are stubs returning 0). Body, copying the pinned-task shape from test_smp_sched.cpp:120-197:
  1. const uint64_t bsp_cr4 = arch::read_cr4();   (the BSP is the running CPU)
  2. If smp::ap_count() != 1 -> JARVIS_TEST_PASS(); return;   (UP leg: parity vacuous, records a pass)
  3. Create a task whose entry stores g_ap_cr4 = arch::read_cr4(); g_ap_cpu = arch::cpu_index();
     g_ap_done = 1; then Scheduler::terminate(*Scheduler::current_task(), 0) + arch::hlt() loop.
  4. Scheduler::set_affinity(*t, 0x2); Scheduler::add_task(*t); poll g_ap_done (2000 ms TSC budget —
     reuse/duplicate poll_until from test_smp_sched.cpp).
  5. Assert exactly these four things:
     a. g_ap_cpu == 1 (the read really happened on the AP);
     b. if (arch::has_smap()) JARVIS_ASSERT(g_ap_cr4 & arch::CR4_SMAP);
        else              JARVIS_ASSERT(!(g_ap_cr4 & arch::CR4_SMAP));
     c. the same iff-CPUID pair for has_smep() / CR4_SMEP;
     d. parity (the actual #611 claim, also regression-covers #156/#338):
        const uint64_t mirrored = arch::CR4_SMEP | arch::CR4_SMAP | arch::CR4_FSGSBASE | arch::CR4_PCIDE;
        JARVIS_ASSERT((g_ap_cr4 & mirrored) == (bsp_cr4 & mirrored));
  6. Assert the AP is back on idle (reuse ap_parked_on_idle(2000)), drain_zombie_list(),
     JARVIS_ASSERT(Scheduler::find_task(id) == nullptr) — zero ResourceTracker delta.
  Add the includes test_smp_sched.cpp uses (kernel/task/task.hpp, kernel/task/scheduler.hpp, ...).
- Count rows (QE): test_expected_counts.hpp — smp_bringup 5→6, smp 81→82, smp_multicpu 22→23
  (lines 126/325/326); all three move together (LEARNINGS #337: bump fine + aggregate rows).
- QE control leg (mandatory, has-teeth proof): run the new test against the pre-fix binary (stash the four
  production files) on -cpu max; it must FAIL at 5b/5d with the AP bit clear. Then run it fixed -> PASS.
- Existing classes to run (dev, after each change):
  - arch (x86_64) — arch_cross carries smap_cr4_bit_set (test_cross_arch.cpp:678) and the SMEP/SMAP PF
    tests; proves the kernel.cpp constant substitution did not disturb BSP CR4.
  - smp_bringup — the changed function own class (AP boots with the new write).
  - smp_sched — AP dispatch/affinity/drain AFTER the arm (13 tests; a SMAP-armed AP must still run tasks).
  - smp_multicpu — full 2-CPU aggregate (22 tests) — the strongest single AP signal.
  - core_basic, ipc, elf_loader, selftest — header blast radius (cpuid_impl.hpp/io_impl.hpp are included
    widely) plus ipc/elf_loader are the user-copy-heavy paths a mis-armed CR4 would break.
  - Trailing bash scripts/run_all_classes.sh x86_64 debug before DONE.
  - aa64/rv64: arch_aarch64 and arch_riscv64 + selftest on both (INV-611-9: prove no perturbation).
  - Release identity: --arch x86_64 --build release --class all (84/84 baseline).
- Gates (item g), exact entry points only: tools/meson/build.sh --arch {x86_64,aarch64,riscv64} (3-arch
  green), make check-style (Errors: 0), then the class legs above via python3 tools/meson/qemu_test.py
  --arch <arch> --build <build> --class <class>, then the QE matrix
  bash scripts/run_all_classes.sh x86_64 {debug,release} on testbed. Append one test-history.txt row after
  EVERY class run (mandatory, incl. selftest).
- KVM SMP boot leg (QE): the harness auto-selects -accel kvm -cpu host when /dev/kvm is usable, so
  --class smp_bringup on the QE station IS the KVM SMP boot check; it must report smp_bringup 6/6 with
  "[qemu] KVM active" in the log, plus smp_multicpu 23/23. Manual negative evidence: one hand-run with
  -cpu qemu64 -smp 2 (no SMEP/SMAP advertised) asserting both CPUs clear (5b/5c else-branches).

## SIL 3 / SAFETY RISKS
- [S1] Arming SMAP on the AP before the start-gate could fault a pre-gate AP path that derefs user memory
  without a stac window — new AP-only #PF. Mitigation: the pre-gate body is audited to touch only
  percpu/LAPIC/TPR (smp.cpp:335-343); all post-gate user copies are the shared stac/clac-wrapped C++ that
  already runs on the BSP, and any unwrapped deref is already UP-visible (INV-611-7). Verified by
  smp_multicpu + ipc + elf_loader + trailing all.
- [S1] Config divergence: if the AP block were gated differently from the BSP (e.g. unconditionally), a
  CONFIG_SMAP=0 build would arm SMAP on APs only -> AP-only faults, inverting the asymmetry. Mitigation:
  identical (CONFIG_SMEP || CONFIG_SMAP) + nested #if CONFIG_SMEP/#if CONFIG_SMAP structure (step 3),
  declared an audit checkpoint; reviewer diffs the two #if ladders line by line.
- [S2] Homogeneous-CPUID assumption: has_smap()/has_smep() are re-probed per AP, so a virtualized asymmetric
  CPUID would make an AP arm a bit the BSP does not have. Mitigation: the same assumption the #338 AP mirror
  and the #606 CPU-global latch already rest on (INV-611-6); no cross-CPU comparison is introduced. Residual
  accepted and documented in the step-3 comment.
- [S2] Silent-failure surface if the write did not land (wrong gate bit -> #GP at mov cr4). Mitigation: any
  #GP here is pre-start-gate, so the BSP rendezvous times out and panics "SMP: AP start timeout" — loud and
  bounded, never a half-woken AP. No new panic path is added; deliberately NO read-back ENSURE on the AP
  (it would change boot failure policy for an invariant that cannot currently be violated without a #GP
  first, and the QE leg proves the hardware state without any production hook).
- [S3] Duplicate bit-position knowledge in kernel.cpp was the divergence class that produced #611 (two
  paths, two literal sets, one updated). Mitigation: step 4 collapses it to the shared constants
  (behavior-identical, verified by the arch class).
- [S3] has_smap()/has_smep()/has_fsgsbase() on a CPU whose max basic leaf < 7 returns max-leaf EBX instead of
  feature bits (pre-existing, audits/report-20261122-224539-606.md:10). Mitigation: unchanged pattern, no new
  caller class; a max-leaf guard belongs inside the probes for all callers and is a separate hardening item,
  not part of #611.
- [S3] AA64 analog gap (INV-611-9) is documented but not fixed here; mitigation: tracked as #613 (filed 2026-11-24, kept out of this lane per file-don't-creep), referenced from the commit message, with the riscv64 disposition proven.

## OPEN QUESTIONS
- none. The AA64 SCTLR_EL1.PAN disposition is decided in INV-611-9 per the LEARNINGS #523 house rule (file
  the follow-up, do not scope-creep). If the operator prefers strict same-cycle shipping of the aa64 arm, it
  is a separate design change (a latch-free arch::pan_arm_ap() called from aarch64 ap_main before the
  start-gate) needing its own audit and arch_aarch64 gates; it is deliberately not specified here.
