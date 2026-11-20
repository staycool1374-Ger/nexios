# AUDIT REPORT 2026-11-20T10-05-00Z — F9 values-only code audit, issue #562

PATCH: /tmp/pending_patch_562.diff (verified byte-identical to `git diff` of the 4 files in-tree)
FILES: src/kernel/nexios_config.h, docs/specs/microkernel-transition.md, docs/specs/configuration.md, docs/specs/iommu.md

Normative: audits/plan-562.md + audits/design-review-562-r1.md (APPROVED, 4×S3).
Scope: 4 config lines (2 `#define` values + 2 paired `#error` messages) + 8 doc cells. No test files, no TU changes, no knob changes. test-history.txt + graphify-out/ deltas excluded from the production patch.

## VERIFIED (load-bearing, evidence-backed)

- V1 exact-4-lines: hunks change only `CONFIG_IOPB_MAX_TASKS 4→8` (:704), `CONFIG_IOMMU_MAX_MAPPINGS 32→128` (:864), and the two paired `#error` defaults (:1701, :1734). Zero adjacent-line drift, zero reflow; patch file IDENTICAL to worktree diff.
- V2 paired guards: `#if … < 1` fail-closed semantics unchanged; messages now read "(default 8)"/"(default 128)" — consistent, no lie. INV-B1 (pair rule, LEARNINGS #ifndef+#error) honored.
- V3 mask headroom: `g_iopb_claimed` is `uint32_t` (iopb.cpp:45); every shift is `1u << slot`/`1u << i` (:74,:80,:82,:96,:135,:169,:202) with slot ≤ 7 at N=8 — safe, 24 bits of UB margin to the 32 ceiling. `iopb_slot_` uint8_t + `IOPB_SLOT_NONE=0xFF` (task.hpp:977-978) unaffected. Grep: no `static_assert` on IOPB exists anywhere — S3-3 punt ships no dependency on the missing assert (values-only contract forbids new code). Acceptable.
- V4 IOMMU sizing: `IoMmuMapping` = 8+8+4+1(+3 pad) = 24 B (iommu.hpp:42-47); 8 domains × 128 × 24 B = 24 KiB (+18 KiB over 32). All scans bounded by the macro with `size_t` casts (iommu.cpp:573,616,628,675,799); domain-index guards use untouched `CONFIG_IOMMU_MAX_DOMAINS` (8) throughout; `static_assert(BUSES<=8)` (:90) unaffected. `CAP_MAX_IOMMU` (16, live-cap bound — a different quantity) correctly left alone in code (:850-851, guard :1727-1728) and docs. 8-domain interplay unchanged.
- V5 consumer asserts: `kCapMaxDerivations >= 4×CSLOT` = 256 ≥ 256 exact (derivation.hpp:70; CSLOT untouched — do-not-touch correct); TCB `static_assert`s (task.hpp:1474-1484) unaffected (no TCB field added); F4 tripwires (tcb_control.cpp:37,42) untouched (no mem-mgr row added — consistent with deferring per-row MAX_PER_TASK_OBJECTS to M4/M11); no other assert references the two changed values.
- V6 .bss/arch parity: x86_64 ≈ +50 KiB (32 KiB IOPB x86-only TU + 18 KiB IOMMU); aa64/rv64 +18 KiB. riscv64 fail-stop is symbol-derived (`_boot_stack_rv − 2 MiB`, boot.S:55-58), not hardcoded — #510 headroom ~17.3 MiB vs +18 KiB ≈ 1000× margin; riscv64 selftest 72/0 post-patch (row 7705) proves boot survived the growth. IOPB TU is x86-only by design; off-arch dispatch stays fail-closed; §11 row preserves arch scoping. 3-arch gates run: selftest 157/188/72 + release 87.
- V7 doc sync exact, no transposition: §7.3 UNTYPED 64 / DEATH_WATCHES 32 / IOMMU 8/128 / IOPB 8 (:461-464); §11 "IOPB, 8 tasks" (:788, other columns untouched); configuration.md :136→8, :139→128 (prose carries no hardcoded counts); iommu.md :40 MAPPINGS 128 with CAP_MAX_IOMMU 16 / DOMAINS 8 / BUSES 8 preserved.
- V8 probes at new bounds: cap_iommu 12/12 x86 + 10/10 ×2 off-arch (128+1 overflow probe green at 128); configuration_build 10/8/8 green; ioport 9-TCB probe self-adapts via macro (test_cap_mmio.cpp:499) with capability counts identical pre/post (167/3 ×3 runs) and #593 audit convicting the same F2 set — no new failure.
- V9 failure credibility (stash-proof): core_basic 92/6 first appears 11-18 (row 7519), predating this patch, identical post-patch; core_memory 128/1 = MaxBuffersExhaustion with BASE-CONTROL-b251b7b8 STATUS rows (7613-7614); capability trio F2 identical pre/post per design-review-593-code-r1:59-61. Buffer-pool paths reference neither changed macro (`POOL_PAGES = CONFIG_BUFFER_POOL_PAGES`, untouched; zero macro hits in memory/ + buffer-pool tests) — a values-only diff cannot change those code paths, so identical-count failures are definitionally pre-existing. Claim credible.
- V10 evidence: test-history.txt rows 7684-7706 (full 3-arch matrix + release selftest 87/0); style Errors: 0 confirmed by auditor execution (`validate_style.py` rc=0, 563 files); `check-config.py` PASSED rc=0 (3 unrelated pre-existing WARNs: PIT/TSC, HHDM note, APIC-timer).
- V11 design-verdict S3s all honored: S3-1 (Q-F9-1 land-128 default) shipped as 128; S3-2 (Q7 bookkeeping edits) match Q7 exactly; S3-3 (mask tripwire) punted with no shipped dependency; S3-4 (CAP_MAX_IOMMU) checked and correctly excluded.
- V12 audit checks: no dynamic allocs (pure .bss resize); no lock/critical-section/IRqGuard changes (scans lengthen under the same leaf `g_lock_`); no test edits (assertion-masking N/A — git status confirms zero src/kernel/test changes); no PMM/BufferPool touch; no new cross-domain channels; preprocessor pairs consistent; no knob added/removed (§3.1 closed set untouched).

## FINDINGS

- [S3] Issue #562 thread carries only the work-begun comment — no pasted graphify/vault retrieval artifacts (check 7), no gate evidence, no Q-F9-1/Q-F9-2 owner confirmations, no `kernel_phys_end` record (INV-B6 record step).
  WHY: Substance is evidenced in-repo (plan-562 Appendix C: 9 queries + dispositions; design-review-562-r1 VERIFIED; test-history rows 7684-7706; #593 F2 conviction), so this is a traceability gap, not a code defect — precedent design-review-558-code-r1:7 (compensating in-tree evidence recorded as limitation, APPROVED). Paste Appendix C + gate summary + `kernel_phys_end` to #562 before merge.
- [S3] Owner confirmations Q-F9-1 (IOMMU-in-F9 scope) and Q-F9-2 (§7.3/§11 normative-edit authority) outstanding on the thread.
  WHY: Merge conditions for the owner per dispatch terms — recorded here as merge conditions, not findings; code already implements the Q-F9-1 default (128 landed) and Q-F9-2 bookkeeping exactly.

## PATCH

Omitted (APPROVED — no rejected patch written).

DECISION: APPROVED
