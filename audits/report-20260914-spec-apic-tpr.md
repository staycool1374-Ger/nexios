# AUDIT REPORT 2026-09-14T15-30-35Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/apic-tpr.md

## FINDINGS
- [S2] docs/specs/apic-tpr.md:§3.1 — timer-vector move edit list incomplete: pci.cpp:537 bitmap entry and test_cap_msix.cpp:212-220 expectations hardcoded 64 (allocator would hand the tick vector to a device; test would fail post-move)
  WHY: Verified by grep — claim_slot uses the symbol (auto-moves) but the pci bitmap and the msix test do not.
  STATUS: fixed in-spec before this report (edit list now enumerates all three sites + auto-move notes).
- [S2] docs/specs/apic-tpr.md:§3.6 — ISR prologue re-assert ungated: writing LAPIC MMIO/MSR with the APIC disabled (PIT fallback path) faults inside the ISR
  WHY: enable_local is APIC-only; the PIT path never maps LAPIC MMIO, so an unconditional re-assert is a guaranteed #PF-in-ISR on APIC-less boots.
  STATUS: fixed in-spec (gate on APIC::is_enabled(), x86_64 only).
- [S3] docs/specs/apic-tpr.md:§3.5 — TprGuard placed in irq_guard.hpp risks an include cycle (needs APIC + PerCpu decls; irq_guard.hpp is pervasively included)
  WHY: percpu.hpp pulls task.hpp; widening irq_guard.hpp's dependencies invites a cycle.
  STATUS: fixed in-spec (own header src/kernel/arch/hal/tpr_guard.hpp).
- [S3] docs/specs/apic-tpr.md:§3.4 — setter rejection mode unspecified; tree convention (send_ipi) is bool
  WHY: Unspecified error signalling drifts toward ENSURE-on-reachable-input (CODING_STYLE §5 violation risk).
  STATUS: fixed in-spec (returns bool, false = rejected).
- [S3] docs/specs/apic-tpr.md:§3.7 — snapshot wording asserted unverified PerCpu POD coverage
  WHY: C1 added SchedPerCpuPod; PerCpu-array snapshot coverage was not verified.
  STATUS: fixed in-spec (extend test_isolate POD with the 8 shadows).
- [S3] docs/specs/apic-tpr.md:§3.1 — planner's x2APIC TPR address 0x880 repeated nowhere in spec; verified correct 0x808 via x2apic_msr(off) = 0x800 + (off >> 4)
  WHY: A raw-0x880 implementation would program a reserved MSR (#GP in ISR context).
  STATUS: spec mandates the shared mode-gated wr()/rd() path, never raw MSRs.

Positive checks: check-7 retrieval artifacts present on #26 (graphify 117-node query + vault 0-match dispositions posted); class table honors INV-TPR5 (timer 0xE0 + IPI 0xEC above all raised classes ≤ 0xD0); EOI-before-restore ordered on all consuming paths; TPR-never-replaces-IF stated normatively; riscv64/aarch64 explicitly out of scope.

DECISION: APPROVED
