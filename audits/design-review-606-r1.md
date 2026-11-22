# DESIGN REVIEW 606-r1 — CPUID-gated stac/clac emission (plan-606)

BASE: origin/main tip `e6c17fa47` confirmed (`git log --oneline -1`).
SCOPE: plan only (`audits/plan-606.md`). No code read beyond verification
samples; nothing edited, nothing staged, nothing committed. Committed #610
TEMP-DIAG (`998ccd15e`, scheduler_tick.cpp) and stashed #604 work present in
tree; neither touched by this review.

NORMATIVE ISSUE: #606 (S1, Sandy Bridge KVM `-cpu host` wedge; TCG masked).

## VERIFICATION METHOD

Spot-checked plan claims against the tree with grep + targeted reads:
wrappers (`x86_64/hal/io_impl.hpp:146-150`), CPUID helpers
(`cpuid_impl.hpp`), writer block (`kernel.cpp:1141-1165`), `ap_main`
(`x86_64/hal/smp.cpp:332-395`), boot order (`kernel.cpp:1509/1743/1798`),
aa64 precedent (`aarch64/hal/io_impl.hpp:128-166`,
`aarch64/early_init.cpp:39,72`), include gating (`arch/hal/io.hpp`),
meson glob (`src/meson.build`), harness (`tools/meson/qemu_test.py:154-216`),
fault handler (`kernel.cpp:2595-2597`), syscall AC detector
(`syscall.cpp:150-159`), CONFIG_SMAP comment (`nexios_config.h:1091-1094`),
test registry classes, LEARNINGS refs, raw-asm sweep.

## CHECK-BY-CHECK VERDICT

1. **Census — FAIL (see S2-1).** Raw emission: only `io_impl.hpp:148/150`
   (`asm volatile("stac"/"clac")`); no `.S`/`.asm`/`bytes` emission tree-wide.
   All call sites use `arch::` prefix (no bare `stac()` calls outside the two
   definitions). Wrapper gate therefore covers 100% — the design choice
   (option i) is correct. BUT the counts are wrong: tree has **18 production
   `arch::stac()`** (checked_ptr 8, elf 5, misc 2, fs 1, proc_svc 1,
   lifecycle 1 = 18), not 19; and **3 test-site `arch::stac()`**
   (test_waitpid:426 + aarch64 ×2 — test_cross_arch.cpp has NO textual
   `arch::stac()`, only `clac` at :714 plus a dynamic via safe_copy), not 4.
   Total tree-wide = 21, not 23. clac census (42 prod + 5 test = 47) verified
   exact per file. The plan's own line items sum to 18 while its header and
   step-7 gate say 19/4 — internally inconsistent and unpassable as written.
2. **Pairing — PASS.** Sampled checked_ptr (all 8 incl. early-return and
   KEEP-after-access `read()`), elf count/total/copy + envp/argv (arm→stac
   order kept at 419-420/436-437), proc_svc validate loop (4 interior
   clac-exits, each followed by return/disarm). Every normal path and every
   recover label routes through the wrappers with one shared predicate, so
   gated-stac can never meet ungated-clac. Recover-on-SMAP-less: AC is only
   ever set by an executed `stac`; skipped stac ⇒ AC=0 on entry, gated clac
   is a no-op — and the ungated-clac #UD this replaces on the cleanup path was
   never load-bearing (nothing depends on clac faulting). SMAP-host fault path
   (`regs[17]`-only redirect, `iretq` restores RFLAGS incl. AC) semantically
   unchanged; recover labels stay clac-first. No site needs reordering.
3. **Hot path — PASS, sound.** `stac` sits once per call outside the copy loop
   at every sampled site (proc_svc's multiple `clac`s are all terminal exits).
   O(1) cached-bool branch per call; no hoisting argument needed or possible.
   Cost framing (~1-2 vs ~20-30 cycles) is plausible and immaterial.
4. **Flag storage — PASS.** Meson globs `kernel/arch/<arch>/**/*.cpp`, so
   `x86_64/early_init.cpp` is in every x86 build. Writer block
   (`kernel.cpp:1141-1165`, x86-gated, BSP) runs before `Scheduler::init`
   (:1509), `smp::bring_up` (:1743), daemon spawn (:1798); IDT loads after
   (:1197), so no ISR/task copy can pre-date the write — stronger than the
   plan's "IRQs off" claim. APs read post-`scheduler_ready_` acquire-gate.
   Plain-bool shape is byte-identical in risk to the SIL3-approved
   `g_pan_supported` (decl/def/write/gate all mirror it, verified); even a
   hypothetical pre-write reader fails closed (CR4.SMAP equally unset).
   `CONFIG_SMEP=0 && CONFIG_SMAP=0` ⇒ no writer, latch stays `false`, copies
   run unlocked with enforcement off — consistent, link-safe.
5. **Cross-arch — PASS.** Both edited headers reachable only under
   `CONFIG_ARCH_X86_64` (`hal/io.hpp:27,61-67`); latch TU x86-only via glob.
   aa64 precedent accurately described (gated stac/clac/read_rflags,
   `= false` def, `pan_init` writer); deliberate x86 `read_rflags` non-gating
   is documented. rv64 SUM needs no gate. Parity gates (both arches build +
   `arch_cross`) adequate; SMAP tests compile out at exactly :673/:777.
6. **Validation — PASS with condition (see S3-2).** Control is falsifiable:
   `smap=off` must produce the `[BOOT] SMAP not supported` line AND (if TCG
   enforces CPUID-legality #UD) `FIRST-CHANCE vec=0x6` in `safe_copy_to_user`;
   message-without-#UD routes honestly to the R720 KVM leg. Hook absent today
   (3 `-cpu cpu_model` sites at :178/:194/:216, no env override) — correctly
   assigned to QE, not smuggled into dev scope.
7. **Sanctity + gates — PASS with gap (see S3-1).** Dev lands zero tests, edits
   zero test files — clean. QE owns 2 new tests + 2 capability-aware repairs
   (polarity assertions, no blanket skips — including the correct degrade
   analysis that the deliberate-deref test cannot #PF when SMAP is off).
   Targeted class list covers checked_ptr, syscall, lifecycle, IPC paths but
   omits `elf_loader`/`proc_elf` despite elf.cpp ×5 sites + exec path;
   trailing `all` mitigates, add them to the targeted list.
8. **Check-7 retrieval — REAL.** 8 queries with used/ignored dispositions and
   file:line anchors; independently verified: `invpcid_emit` early-return gate
   (`io_impl.hpp:129-131`), aa64 PAN precedent, LEARNINGS #307 (apt for
   rejecting option ii) and #45. Not boilerplate.
9. **Stacking — PASS.** Plan's 5 files ∩ {committed DIAG `scheduler_tick.cpp`}
   = ∅; ∩ stashed #604 set (scheduler*, global_state*, ipc, docs) = ∅.
   Pre-existing `M test-history.txt` noted — dev stays append-only per plan.

## FINDINGS

- [S2] audits/plan-606.md (census + §step 7) — PRODUCTION stac COUNT WRONG.
  WHY: tree has 18 production `arch::stac()` (8+5+2+1+1+1) and 3 test-site
  (total 21); plan claims 19+4 (=23) and its step-7 acceptance criterion
  ("19 production … plus 4 test-site") cannot pass — dev either stalls or
  hunts a nonexistent 19th site. Fix the header, the §INV census line, and
  the step-7 numbers to 18/3 before implementation; the fix shape is
  unaffected (wrapper gate is count-insensitive).
- [S3] audits/plan-606.md (gate list) — `elf_loader` + `proc_elf` classes
  missing despite elf.cpp ×5 production sites and the proc_svc exec path.
  WHY: targeted signal should cover every touched file's class; trailing
  `all` mitigates but the list should name them.
- [S3] Issue #606 closure — require the QE negative leg (pre-fix #UD control
  + fixed-binary SUMMARY with flag false on `smap=off`) before closing.
  WHY: dev-lane evidence (review + SMAP-host regressions) cannot prove the
  S1 fix; the plan implies this ordering — make it an explicit close gate.
- [S3] AP CR4.SMAP asymmetry (`smp.cpp:351-368` mirrors PCIDE/FSGSBASE, never
  SMAP/SMEP) — correctly scoped out, but "must be filed separately" needs an
  owner + issue number before #606 work starts so it is not lost.
  WHY: process hygiene for a known pre-existing enforcement gap on APs.

## PATCH

`audits/rejected_patch.diff` not written: design-stage review, no code diff
to correct — the S2 is a plan-text fix the developer applies to
`audits/plan-606.md` directly (18/3 census + step-7 numbers), then proceeds.

DECISION: REJECTED
