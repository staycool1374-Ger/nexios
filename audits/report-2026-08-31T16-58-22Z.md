# AUDIT REPORT: /Users/arnold/jarvis/audits/report-2026-08-31T16-58-22Z.md

Audit iteration 2 of `audits/pending_patch.diff` (13 files). READ-ONLY review
against prompts/CODING_STYLE.md, prompts/AGENTS-KERNEL-BRIEFING.md, and the
audit focus (ENSURE-vs-error, spinlock-across-reschedule, BLOCKED-dequeue
invariant, TCB lifecycle, arch consistency).

## FINDINGS

- [S2] src/kernel/arch/hal/context.hpp:123 — 12.x/§10 API consistency —
  changing `init_stack` to `uint64_t *&stack_top` decrements the caller's
  pointer by 36 slots, so the aarch64 branch of `test_cross_arch.cpp:220-229`
  (class `arch_cross`, unchanged by this patch) now reads only zero slots at
  `stack_top[-1..-7]` and its `written` assertion fails; the test passed under
  the prior by-value signature (entry pointer sat at `stack_top[-4]`), and the
  re-verified gates (`arch_aarch64` 22/22, x86_64 all 85/85) do not exercise
  `arch_cross` on aarch64, so this regression is unverified.

- [S3] src/kernel/arch/aarch64/boot.S:177 — stale-comment S3 item is
  incomplete — `// L1[1] = L2 table at 0x40007000` still references the old
  address while the code at line 178 loads `0x40027000`; all other
  0x4000[0-8]000 table-address comments were updated (verified by grep, only
  line 177 remains).

- [S3] src/kernel/arch/aarch64/hal/pci_impl.hpp:34 + pci.hpp:149 — the S2 ECAM
  base fix (0x3f000000) is internally consistent (pci.hpp:50 comment matches,
  within the identity-mapped 0-1GB Device window, test reads real vendor/device
  config space), but the ECAM window is not mapped in the TTBR1 kernel table
  (boot.S only maps UART 0x9000000/GIC 0x8000000), so raw-physical PCI config
  access faults once a user TTBR0 is active — pre-existing access pattern, not
  introduced by this edit, but the S2 fix only guarantees boot-time identity-map
  access.

## DECISION: REJECTED

Blocking item: S2 at context.hpp:123 — the aarch64 `init_stack` by-reference
signature change introduces a previously-passing test regression in the
`arch_cross` class (test_cross_arch.cpp aarch64 branch). Fix (update
test_cross_arch.cpp's aarch64 branch to the by-ref frame semantics, or revert
the signature change) and re-audit. All S2 ECAM-base/TLBI items verified
resolved; remaining items are S3.