# AUDIT REPORT 2026-09-14T16-12-10Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/arch/x86_64/hal/apic.hpp, src/kernel/arch/x86_64/hal/apic.cpp, src/kernel/arch/x86_64/hal/percpu.hpp, src/kernel/arch/x86_64/hal/percpu.cpp, src/kernel/arch/hal/tpr_guard.hpp, src/kernel/arch/x86_64/hal/timer.cpp, src/kernel/arch/x86_64/hal/smp.cpp, src/kernel/arch/x86_64/isr_stubs.asm, src/kernel/kernel.cpp, src/kernel/task/scheduler.cpp, src/kernel/arch/pci.cpp, src/kernel/test/test_apic_tpr.cpp, src/kernel/test/test_cap_msix.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_isolate.cpp, src/kernel/test/test_registry.cpp, src/kernel/arch/riscv64/timer.cpp, docs/specs/apic-tpr.md

## FINDINGS
- [S2] src/kernel/arch/hal/tpr_guard.hpp:ctor — raise/shadow pair not atomic vs interrupts unless the caller holds IF=0; a nested ISR prologue could re-assert HW from the stale shadow mid-raise (HW/shadow diverge until the next ISR)
  WHY: TPR narrowing without IF=0 admits a preempting vector between the HW write and the shadow write, defeating the guard's own bookkeeping.
  STATUS: fixed pre-report — ctor now ENSUREs !interrupts_enabled() on x86_64 (INV-TPR2 enforced in code, fail-stop on misuse).
- [S3] src/kernel/arch/x86_64/hal/apic.cpp:enable_local — set_tpr_class() is fail-closed while !enabled_ but init() sets enabled_=true only after enable_local(), silently skipping the ACCEPT_ALL write (deviation from the unconditional raw write it replaced)
  WHY: Verified by reading init() order; QEMU reset TPR happens to be 0 so this was latently harmless, but the commit-point semantics must not depend on reset values.
  STATUS: fixed pre-report — enable_local uses reg_wr directly; spec §3.4 amended.
- [S3] src/kernel/arch/x86_64/isr_stubs.asm:nested-apply guard — cmp rax, 64 hardcoded the old timer vector; with the tick at 0xE0 a task blocked in a syscall hlt-loop strands its voluntary arm (RMS early-returns on the pending arm every tick: total boot stall, reproduced as safe-class TIMEOUT, bisected to the constant)
  WHY: The deferred-switch design relies on nested-tick apply for hlt-blocked tasks; the move silently removed it.
  STATUS: fixed pre-report — cmp rax, 0xE0 with a comment documenting the hlt-loop dependency; safe 133/133 re-verified with 0xE0 live.
- [S3] src/kernel/kernel.cpp:handle_interrupt_c tail — IrqDelivery/IrqThread/0x80 early returns bypass the tail TPR restore
  WHY: Audited safe — none of those paths raise TPR (guards self-restore; isr_entry/ack never raise), and the next ISR prologue heals any future violation.
  STATUS: accepted, no change.
- [S3] test/cap_msix + pci bitmap + claim_slot timer-64 references — updated to the 0xE0 symbol/literal; vector 64 becomes an ordinary claimable MSI-X vector (in-window, unreserved)
  WHY: Verified claim_slot/irq_delivery use the symbol (auto-move); only pci.cpp:537 and the msix test hardcoded 64.
  STATUS: fixed pre-report; cap_msix 13/13 green.
- [S3] planner STEP-15 (idt.cpp vector reservations) superseded — IDT installs all-256 gates from __isr_vector; reservations live in the pci bitmap + claim_slot, both updated
  WHY: No idt.cpp change exists or is needed; stub table covers 32-255 via %rep.
  STATUS: accepted, noted for the issue thread.
- [S3] drive-by (rule-mandated, pre-existing): scheduler.cpp validate_iret_frame f unused on riscv64 (2 sites → [[maybe_unused]]); riscv64/timer.cpp handle_irq() arity + unused ip
  WHY: riscv64 -Werror build failed at HEAD-independent code; AGENTS.md forbids dismissing failures as pre-existing.
  STATUS: fixed pre-report; riscv64 + aarch64 + x86_64 builds green.

Positive checks: zero dynamic allocation on touched paths; no lock-order change (no new locks; TPR accessors lock-free HW); no test-assertion weakening (msix assert strengthened to the symbol); preprocessor symmetry verified per file (non-x86 snapshot layout only grows 64 B, save/restore loops x86-guarded); check-7 retrieval artifacts on #26; EOI-before-restore ordered after PIC EOI; MODE_NONE paths no-op without touching unmapped MMIO (tpr_raise/restore early-return on !enabled_).

DECISION: APPROVED
