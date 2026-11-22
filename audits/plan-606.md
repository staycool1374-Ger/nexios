# PLAN: CPUID-gated `stac`/`clac` emission — issue #606 (S1, arch/x86_64)

## BASE & SCOPE RECORD (read before implementing)

- Base observed: origin/main tip `e6c17fa4` (2026-11-22T22:12:48Z, "Cookbook Rule 7:
  pin FIXED for rendezvous choreography (#610)"). This planner session had **no
  shell tool**, so `git log --oneline -1` was not executed — the developer MUST
  confirm the local tip and clean-tree state before staging anything.
- DO NOT TOUCH: the committed TEMP-DIAG block in
  `src/kernel/task/scheduler_tick.cpp` (#610, commit `998ccd15`), the stashed
  #604 work, any file under `src/kernel/test/` or `test_*.cpp`, `test-history`
  rows other than the append-only gate rows, and `audits/pending_patch.diff` /
  `audits/rejected_patch.diff` scratch. This plan changes **4 production files
  only**; nothing else may appear in `git status`.
- Line anchors below are from the tree as read in this session.

## RETRIEVAL EVIDENCE (audit check-7)

| Query | Result | Disposition |
|---|---|---|
| `graphify query "stac clac SMAP user copy checked_ptr safe_copy"` | 46 nodes; `safe_copy_to_user`/checked_ptr cluster in community 1717; `stac()`/`clac()` split across x86_64 (community 10), aarch64 (community 40), riscv64 (community 21) | **used**: confirmed three per-arch wrapper definitions exist and that a single wrapper edit is arch-local |
| `graphify query "CPUID feature detection boot CR4 SMAP enable"` | 154 nodes; `cpuid()`/has_* helpers community 127; boot_sdf/cpuid helpers surfaced | **used**: located the CPUID helper home for the new capability constant |
| `graphify query "invpcid_emit has_invpcid CPUID gate instruction emission precedent"` | `invpcid_emit()` (io_impl.hpp:129) and `has_invpcid()` (cpuid_impl.hpp:106) directly adjacent in community 1471 | **used**: in-tree precedent for "gate the emission inside the wrapper, return early" |
| `graphify get_neighbors "stac()"` | single `contains` edge to io_impl.hpp | **used**: no hidden callers outside the wrapper; greps are authoritative for call sites |
| `graphify shortest_path "stac()" -> "g_user_access_recover_ip"` | no path | **ignored**: label mismatch in the graph; the pairing relation was established from source (checked_ptr.hpp + kernel.cpp vector-14 handler) instead |
| `obsidian search "SMAP"` | `audits/design-review-588-r1.md`, `audits/plan-588.md`, `audits/report-*` | **used**: #588 r1 establishes the arm→stac→clac→disarm protocol, the mandatory clac-first recover labels, and the AC-leak consequence at syscall entry |
| `obsidian search "stac clac"` | `audits/plan-588.md:96` (§6.1/§6.2 external-pager retrieval precedent) | **used**: read `docs/specs/external-pager.md` §6.1-6.2 to confirm no spec asserts unconditional emission |
| `obsidian search "PAN stac"` | empty | **ignored**: aarch64 PAN precedent found in-tree (`arch/aarch64/early_init.cpp`, `hal/io_impl.hpp`) instead |

## AFFECTED FILES

- `src/kernel/arch/x86_64/hal/io_impl.hpp` — `stac()`/`clac()` (lines 146-150)
  gain a single cached-capability predicate; adds the `extern bool
  g_smap_supported;` latch declaration with its write-once contract. This is the
  one gate every call site inherits.
- `src/kernel/arch/x86_64/hal/cpuid_impl.hpp` — new `CPUID_EBX7_SMAP` constant
  (leaf 7.0 EBX bit 20, next to `CPUID_EBX7_INVPCID`) + `has_smap()` helper, for
  symmetry with the existing `has_invpcid()/has_fsgsbase()` probes. Boot-time
  use only.
- `src/kernel/arch/x86_64/early_init.cpp` — single definition
  `bool arch::g_smap_supported = false;` (mirrors `arch::g_pan_supported`). This
  TU is already in every build: `src/meson.build:125-134` globs
  `kernel/arch/<arch>/**/*.cpp`, so no meson/gen.mk edit and no new TU.
- `src/kernel/kernel.cpp` — the existing CPUID leaf-7 block (lines 1141-1165)
  becomes the single writer of the latch from the same `ebx` it already reads;
  the two boot log lines are extended (prefix preserved) to name the gate.
- `src/kernel/nexios_config.h` — `CONFIG_SMAP` doc comment (lines 1091-1094)
  currently implies unconditional stac/clac emission; correct it (comment only).

No other production file changes. No test file changes.

## INVARIANTS & CONCURRENCY BOUNDARIES

- **INV-SMAP-1 (legality):** `stac`/`clac` are emitted **iff** the CPU advertises
  CPUID.(EAX=7,ECX=0):EBX.SMAP. CR4.SMAP being off does NOT make them legal —
  that is the bug (issue text confirmed against `kernel.cpp:1157-1163`); the
  boot-time CR4 decision and the emission decision must derive from the same
  CPUID read.
- **INV-SMAP-2 (symmetry):** `stac()` and `clac()` use the *identical* predicate.
  Both instructions are #UD without SMAP; a gated stac with an ungated clac
  moves the #UD to the failure/cleanup path. One hunk, one predicate.
- **INV-SMAP-3 (pairing preserved):** every production site keeps
  `arm recover_ip → (KEEP edge) → stac → user accesses → clac → disarm`, and
  **every recovery label begins with `clac()`** — the 42 production
  `arch::clac()` sites are one per normal exit plus one per recover label
  (checked_ptr.hpp ×18, elf.cpp ×10, proc_svc.cpp ×6, syscall_handlers_misc.cpp
  ×4, syscall_handlers_fs.cpp ×2, scheduler_lifecycle.cpp ×2). Rationale: a #PF inside the window is taken
  with AC=1 in the pushed RFLAGS; the x86 vector-14 handler redirects only RIP
  (`regs[17]`) and `iretq` restores RFLAGS including AC — hence clac-first.
  The gate preserves this exactly: on a SMAP-less CPU AC was never set and the
  label's gated clac is a no-op; on a SMAP host nothing changes.
- **INV-SMAP-4 (single writer, BOOT_ONLY):** the latch is written exactly once,
  in the boot CPUID block, on the BSP, before SMP bring-up and before any user
  task or user copy can exist; read-only afterwards. No setter is exported, no
  test/daemon may write it. Plain `bool` on both the write and the read side
  (never plain-read + atomic-store: CODING_STYLE §11.6); ordering is supplied by
  boot sequencing, documented in the declaration.
  Placement rationale (b): an arch feature latch bound to the instruction
  wrapper stays **module-owned**, exactly like the SIL3-approved
  `arch::g_pan_supported` (aarch64) and the module-owned filesystem
  singletons — it is not a cross-TU service global, so it does not join a
  `global_state` group. A `gs::` accessor would be an out-of-line call on the
  `safe_copy` path in a NO_LTO release build for zero benefit. The discipline
  that `global_state` would have imposed is kept explicitly: single definition
  in one TU, no exported setter, write class BOOT_ONLY, documented ordering,
  and the explicit `= false` fail-closed initializer. If an auditor insists on
  the central registry, the acceptable variant is a `FeatureState` getter with
  a plain-`extern` fast-path reader in the wrapper — never a call in the
  wrapper.
- **INV-SMAP-5 (no hot-path escape):** the wrappers may name only
  `g_smap_supported`. No CPUID, no function call, no atomic, no lock, no
  allocation inside `stac()`/`clac()`.
- **INV-SMAP-6 (no debug/release divergence):** the gate is unconditional in
  both builds (CODING_STYLE §6); only pre-existing log text differs.
- Concurrency boundary: none introduced. Existing IRQ/IRQ-off windows and the
  #588 recover-arm protocol are untouched; `g_smap_supported` is a boot-time
  constant read.
- ResourceTracker: unaffected — no new kernel resource, no allocation.
- Call-site census (proves the "one gate" reach): **18 production `stac` sites
  in 6 files** — checked_ptr.hpp (8: 142/163/181/203/254/282/316/341), elf.cpp
  (5: 131/153/173/420/437), syscall_handlers_misc.cpp (2: 481/540),
  syscall_handlers_fs.cpp (1: 293), proc_svc.cpp (1: 433),
  scheduler_lifecycle.cpp (1: 628) — plus 3 test sites (test_waitpid.cpp:426,
  aarch64/test_aarch64.cpp:976/982) that the gate covers
  without editing (and must not be edited). (test_cross_arch.cpp holds a
  `clac` but no `stac` — counted in the clac census below, not here.)

## STEP-BY-STEP CHANGES

### 0a. Fix shape — options compared (decided input to steps 1-7)

| Option | Shape | Per-call cost | Coverage / audit surface | Verdict |
|---|---|---|---|---|
| **(i) cached CPUID latch inside the wrappers** | `if (!g_smap_supported) return;` in `arch::stac()`/`arch::clac()` | one RIP-relative byte load + one branch that can never mispredict (value immutable after BOOT, L1-resident, no atomics/false sharing) | **2 functions, 1 file**; all 18 production sites + 3 test sites inherit the gate; no site can forget it | **CHOSEN** |
| (ii) gate each call site | 18 hand-written predicates in 6 production files | same load+branch, written 18x — **zero saving** | 18 recovery-label pairings to keep right; test files (which the dev may not edit) would stay unguarded and #UD on a SMAP-less boot; repeat-pattern edits drift (LEARNINGS #307) | rejected |
| (iii) boot-time text patching | rewrite `stac` (`0F 01 CB`, 3 B) to 3x NOP once at boot | zero per call | requires writable kernel text → breaks the W^X hardening of #376 (text mapped exec-then-RO), is self-modifying code (barred by the project's MISRA C++:2023 / ASIL-D posture), needs i-cache coherence + SMP-safe publication, no test path that does not need the same SMAP-less boot, zero existing infrastructure | rejected |

Hot-path analysis that decides it: the AC window already spans the whole copy
loop at every site (one `stac` + one `clac` per call, plus one `clac` on the
recover path) — the gate therefore adds **O(1) work per copy call, not per
byte**, and `safe_copy_to_user`/`safe_copy_from_user` are per-syscall entry
points, not inner loops. On a SMAP host the added work is ~1-2 cycles against
`STAC`/`CLAC` themselves (~20-30 cycles, microcoded flag/CR4-coupled — the
branch is cheaper than the instruction it guards, <10% of the pair). On a
SMAP-less CPU the gate **removes** the instruction, so the fixed path is
strictly faster than today's #UD. Debug builds keep the same shape (-Og, and
`-fno-inline` already turns the wrapper into a call, so the branch adds nothing
structural). Rejected micro-alternatives: function-pointer dispatch per copy
(indirect call + Spectre surface on a user-copy boundary), a build-time
constant (capability is a runtime property), duplicated always-SMAP /
never-SMAP copy bodies (forks a security boundary and doubles the test
matrix).

Exception safety (why the gate cannot break the pairing): the arm order
`arm recover_ip → stac → access → clac → disarm` is untouched at all 18 sites,
and AC is only ever set by an executed `stac`; both directions share the same
predicate, so a SMAP-less CPU cannot enter a recover label with AC set (it was
never set) and its gated `clac` is a no-op. A fault between `stac` and `clac`
on a SMAP host already leaves AC=1 — the #PF frame pushes RFLAGS and `iretq`
restores it — which is exactly why every recover label's first statement is
`clac()`; that behavior is byte-identical before and after the change
(INV-SMAP-3).

### 0b. Cross-arch parity — proof of zero impact (mandatory, same cycle)

- **aarch64**: already shipped exactly this shape for the SMAP analogue —
  `stac()`/`clac()` return early on `!arch::g_pan_supported`
  (`arch/aarch64/hal/io_impl.hpp:139-156`), latch defined once in
  `arch/aarch64/early_init.cpp:39` and written by `pan_init()`. That is the
  same failure class this issue fixes on x86_64: the PAN sysreg access is
  UNDEF when FEAT_PAN is absent, so aarch64 already had to gate the *emission*
  on a boot-detected capability, not on SCTLR_EL1.PAN. **Zero lines change.**
- **riscv64**: `sstatus.SUM` is architectural and unconditional
  (`csrs/csrc sstatus, bit 18`, `arch/riscv64/hal/io_impl.hpp:110-117`) — there
  is no SMAP/stac concept and no gate is needed. **Zero lines change.**
- Mechanical proof: both edited headers are reachable only through
  `#if defined(CONFIG_ARCH_X86_64)` in `src/kernel/arch/hal/io.hpp`, and the
  latch definition TU lives under `kernel/arch/x86_64/` which
  `src/meson.build:125-134` adds to `arch_srcs` only when `arch == x86_64`.
- Parity gate: `tools/meson/build.sh --arch aarch64` and `--arch riscv64` both
  configure+build+link green, and `arch_cross` runs green on both (the SMAP
  tests compile out at `test_cross_arch.cpp:673/777`).

### 0c. Release-build identity

- The gate is unconditional in both builds — no `#ifdef CONFIG_DEBUG` anywhere
  in it (CODING_STYLE §6); the only difference is the pre-existing boot log,
  which both legs already print (`debug_write` is not debug-gated).
- The latch cannot be folded in release: its single writer is in another TU
  (`kernel.cpp`) and release is `-O2` with no LTO.
- Exercise the release leg for real before DONE (LEARNINGS #45 — debug-green
  never implies release-green): build and run `--build release --class
  arch_cross`, `memory_checked_ptr`, `core_syscall`. The 2x3 release-version
  matrix stays a separate release-process gate.

1. `src/kernel/arch/x86_64/hal/cpuid_impl.hpp`: inside the leaf-7 EBX constant
   block, directly after `CPUID_EBX7_INVPCID`, add `inline constexpr uint32_t CPUID_EBX7_SMAP = 1u << 20;` with a
   Doxygen line noting it gates instruction *legality* of stac/clac (issue
   #606). After `has_invpcid()` add
   `inline bool has_smap() { return (cpuid(7, 0).ebx & CPUID_EBX7_SMAP) != 0; }`
   documented as **boot-path only** ("hot paths must read the cached
   `arch::g_smap_supported` latch — CPUID is ~100+ cycles").
2. `src/kernel/arch/x86_64/hal/io_impl.hpp`: immediately before `stac()`
   (line 146) insert, mirroring the aarch64 PAN declaration at
   `arch/aarch64/hal/io_impl.hpp:125-128`:
   `extern bool g_smap_supported;` plus a Doxygen block stating — written once
   by the boot CPUID probe (kernel.cpp SMEP/SMAP block) while the system is
   single-CPU with IRQs off, read-only thereafter; plain read is safe from any
   context (task, ISR, fault handler); it gates *instruction emission*, not
   CR4; issue #606.
3. `src/kernel/arch/x86_64/hal/io_impl.hpp`: rewrite the two wrappers in one
   hunk —
   `inline void stac() { if (!g_smap_supported) { return; } asm volatile("stac" ::: "cc"); }`
   and the identical shape for `clac()` /
   `asm volatile("clac" ::: "cc")`. Keep the `"cc"` clobber. Extend both
   `@brief` blocks: "No-op when the CPU lacks CPUID.SMAP — stac/clac are #UD on
   such CPUs, and CR4.SMAP off does not make them legal (issue #606). The AC
   window therefore exists only where the hardware defines it."
4. `src/kernel/arch/x86_64/early_init.cpp`: inside `namespace arch`, next to
   `g_early_irq_done` (line 37), add
   `bool g_smap_supported = false;` with the write-once comment (the explicit
   `false` initializer is what makes a `CONFIG_SMAP=0`/`CONFIG_SMEP`-only
   build fail closed instead of emitting an illegal instruction).
5. `src/kernel/kernel.cpp`: in the leaf-7 block (lines 1141-1165), directly
   after the `cpuid` asm, add the single store
   `arch::g_smap_supported = (ebx & arch::CPUID_EBX7_SMAP) != 0;` — **outside**
   the nested `#if CONFIG_SMAP` so an SMEP-only build still latches the CPU
   fact. Leave the `#if CONFIG_SMAP` CR4 branch as-is; change only its message
   strings: supported arm → `"[BOOT] SMAP enabled (stac/clac active)\n"`
   (new line, same prefix), unsupported arm → keep the existing
   `"[BOOT] SMAP not supported by CPU — leaving off"` text verbatim and append
   `" (stac/clac emission gated, #606)\n"` so QE expect-patterns that match the
   old prefix keep working.
6. `src/kernel/nexios_config.h`: extend the `CONFIG_SMAP` comment
   (lines 1091-1094) with one sentence: the CR4 bit and the stac/clac emission
   are both gated at boot; emission follows the CPUID capability via
   `arch::g_smap_supported` (issue #606), and a CPU without SMAP runs the copy
   paths unlocked rather than faulting.
7. Verification pass before commit (no code change): count
   `arch::stac()` and `arch::clac()` over `src/kernel` before and after — both
   totals must be identical. Pre-change census: **18 production `arch::stac()`**
   (6 files) and **42 production `arch::clac()`**, plus 3 test-site stac
   (test_waitpid.cpp:426, test_aarch64.cpp ×2) and 5 test-site clac
   (47 `arch::clac()` matches total today). Then `git diff --stat`
   must list exactly the 5 files above; confirm
   `scheduler_tick.cpp`, `src/kernel/test/**` and the stash are untouched.

## TEST STRATEGY

**Sanctity:** the developer lands **no tests** and edits **no** test file
(`src/kernel/test/**`, `src/kernel/arch/*/test_*.cpp`, `test_isolate.*`,
`initrd` test config, `test_expected_counts` rows). Everything below labelled
QE is owned by the QE lane on `testbed`.

- New tests (QE, `testbed` only):
  - `smap_emission_gated_when_cpu_lacks_smap` — on a SMAP-hidden boot:
    `arch::g_smap_supported == false`, `read_cr4() & (1<<21)` == 0, AC bit
    (RFLAGS[18]) == 0 after a `safe_copy_to_user` roundtrip, and a full boot
    SUMMARY with 0 failures. Asserts the degrade is silent and safe, not a #UD.
  - `smap_emission_live_when_cpu_has_smap` — on a normal TCG/KVM boot:
    `g_smap_supported == true`, CR4.SMAP set, and a kernel-context deref of a
    U/S=1 page with AC=0 still #PFs and is redirected (proves `clac` still
    bites; the existing `smap_kernel_deref_user_va_without_ac_pf` shape).
  - Capability-aware repair of two existing tests (systemically wrong after
    the fix, doc-block + body changed together per PROMPT-dev §3):
    `smap_cr4_bit_set` → assert `(read_cr4() & (1<<21)) != 0` **iff**
    `g_smap_supported`; `smap_kernel_deref_user_va_without_ac_pf` → pass
    through (documented degrade) when `!g_smap_supported`. A blanket skip is
    not acceptable: the SMAP-host assertion must stay.
  - Harness knob (QE tooling): `tools/meson/qemu_test.py:154`
    (`cpu_model = "host" if kvm else "max"`) must honour an env override
    (e.g. `NEXIOS_QEMU_CPU`) as the whole `-cpu` argument for the three
    x86_64 branches (lines 178/194/216), forcing TCG when the value is set and
    is not `host` (`max,...` is TCG-only). Without this knob there is no
    sanctioned way to hide SMAP — raw QEMU invocations are forbidden.
- Negative-boot validation path (the R720 substitute), in this order:
  1. **Control, pre-fix binary**, `NEXIOS_QEMU_CPU="max,smap=off"` debug run:
     expect `[BOOT] SMAP not supported ...` then `[FIRST-CHANCE] CPU-EXCEPTION
     vec=0x6` inside `safe_copy_to_user` and no TEST SUMMARY. This is the
     falsifiable check that TCG honours the feature removal (QEMU raises #UD for
     `0F 01 CB` when CPUID.SMAP is clear). If TCG does **not** #UD, record that
     and use the R720 KVM `-cpu host` (Sandy Bridge) run as the negative leg.
  2. **Fixed binary**, same command: boots to a full SUMMARY with 0 failures
     and `g_smap_supported == false`.
  3. **SMAP-host regression**, default `-cpu max`: unchanged from today
     (`g_smap_supported == true`, `arch_cross` SMAP tests green).
- Existing classes to run (x86_64 debug, test-history-derived timeouts,
  last-known rows in parentheses):
  `arch_cross` (24/0) — the only class with the SMAP tests;
  `memory_checked_ptr` (4/0, ~200 ms) and `memory_checked_ptr_api` (9/0) —
  `copy_from/copy_to/read/write` recover labels;
  `core_syscall` (89/0, ~3.6 s) — dominant `safe_copy_to_user` consumer;
  `process_waitpid` (11/0) — the `scheduler_lifecycle.cpp:628` stac site;
  `ipc_core` — IPC user-copy paths (`proc_svc.cpp`, `syscall_handlers_*`);
  `core_memory` — contains both checked_ptr groups plus PMM/VMM isolation;
  `elf_loader` + `proc_elf` — the 5 `elf.cpp` stac sites (ELF user-copy
  paths; r1 omitted them);
  `selftest` — CI gate class;
  full `all` only after every class above is 0-failure.
- Close gate (r1 S3 — explicit): the issue closes ONLY when (a) all classes
  above are 0-failure on the fixed tree, (b) the QE negative leg is green
  (SMAP-hidden boot to SUMMARY with `g_smap_supported == false`, no #UD;
  QE-owned), and (c) the SMAP-host regression leg is green (default boot
  unchanged, `arch_cross` SMAP tests green). No partial close on (a) alone.
- Baseline discipline: the tree has in-flight red work (`scheduler_core 40/1`
  ×3, `selftest 164/1` in recent rows). Record the same class set on the
  pre-change binary and triage any surviving failure against those rows — no
  "pre-existing" dismissal.
- Style/static gates: `make check-style` must report `Errors: 0`
  (blob-triggering gate: errors only, warnings do not fail), the 5 touched files
  clang-formatted (`.clang-format`, ColumnLimit 80), and clang-tidy output
  carries no new warning on the touched lines.
- History rows: append exactly one `test-history.txt` row per class invocation
  (date, class, PASSED/FAILED, measured TIME, `STATUS:` when abnormal).

## SIL 3 / SAFETY RISKS

- [S1] Asymmetric gate (gated `stac` + unconditional `clac` or the reverse) —
  reintroduces #UD on the SMAP-less failure path or an AC leak on SMAP hosts —
  mitigation: both wrappers in one hunk, INV-SMAP-2, grep-verify the pair.
- [S1] An AC leak into `sysret` (privilege hole) if any recovery-label `clac()`
  is removed or reordered while adding the gate — mitigation: the 18 sites and
  their labels are outside the diff; post-change grep counts of `arch::stac()`/
  `arch::clac()` must be byte-identical to pre-change; the debug detector
  (`syscall.cpp:150-159`) stays armed.
- [S1] Latch written after user copies can run (any second writer) — produces a
  nondeterministic #UD-vs-OK split — mitigation: single writer inside the boot
  CPUID block, no exported setter, BOOT_ONLY documented (INV-SMAP-4).
- [S1] Per-TU copy of the latch (e.g. `static`/`inline` variable in the header)
  — TUs would latch independently and some would see `false` forever —
  mitigation: `extern` declaration + exactly one non-static definition; a
  duplicate definition is a link error, which is the guard.
- [S2] Hot-path regression if the coder routes the predicate through an
  out-of-line `global_state` accessor in a NO_LTO release build — mitigation:
  direct `extern bool` read only; the wrapper names nothing else.
- [S2] `has_smap()` (CPUID) accidentally called from the wrapper — ~100+ cycles
  per stac/clac — mitigation: `has_smap()` is documented boot-only and the
  wrapper's only referenced symbol is `g_smap_supported` (review check).
- [S2] `CONFIG_SMAP=0` or SMEP-only configuration links an undefined symbol —
  mitigation: unconditional definition with `= false` initializer in the arch
  TU, and the store placed outside `#if CONFIG_SMAP`.
- [S2] QE over-relaxes the two capability-specific tests while fixing them
  (blanket skip hides a real SMAP-host regression) — mitigation: polarity
  assertion in both directions (`== g_smap_supported`), SMAP-host branch kept
  strict.
- [S3] APs never mirror CR4.SMAP (`smp.cpp:351-366` sets OSFXSR/OSXMMEXCPT/
  PCIDE/FSGSBASE only), so user copies on APs run without SMAP enforcement —
  pre-existing asymmetry, explicitly out of scope for #606, filed as #611
  (do not lose it: #606's latch keeps APs #UD-free, but enforcement parity
  is the #611 lane).
- [S3] Boot-log text is a QE contract: keep the `[BOOT] SMAP not supported`
  prefix verbatim and only append, so existing expect windows still match.
- [S3] `read_rflags()` stays ungated (pushfq is legal everywhere); do not
  "symmetrise" it.
- [S3] Documentation drift is the root cause of this bug class: the
  `CONFIG_SMAP` comment is corrected in the same commit so the next reader
  cannot re-derive "CR4 implies emission".

## OPEN QUESTIONS

- none — the fix shape, storage, writer and validation path are decided above.
  Coordination note (not blocking): the `NEXIOS_QEMU_CPU` harness knob belongs
  to the QE lane; the developer proceeds with the code-level and disassembly
  evidence plus the green regression classes, and the negative boot leg closes
  on the QE station.
