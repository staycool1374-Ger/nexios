# AUDIT REPORT 20260920T140512Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/irq-early-init.md, src/kernel/arch/aarch64/early_init.cpp, src/kernel/arch/aarch64/hal/gic.hpp, src/kernel/arch/aarch64/interrupt_controller.cpp, src/kernel/arch/aarch64/timer.cpp, src/kernel/arch/early_init.hpp, src/kernel/arch/hal/early_init.hpp, src/kernel/arch/hal/interrupt_controller.hpp, src/kernel/arch/hal/timer.hpp, src/kernel/arch/riscv64/early_init.cpp, src/kernel/arch/riscv64/interrupt_controller.cpp, src/kernel/arch/riscv64/timer.cpp, src/kernel/arch/x86_64/early_init.cpp, src/kernel/arch/x86_64/hal/timer.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_irq_early_std.cpp, src/kernel/test/test_registry.cpp

## FINDINGS
- [S3] src/kernel/arch/x86_64/early_init.cpp:680, src/kernel/arch/aarch64/early_init.cpp:96, src/kernel/arch/riscv64/early_init.cpp:409 — g_early_irq_done is a plain non-atomic bool written without IrqGuard (carried over, still S3 at most)
  WHY: Safe only under the unstated single-core/IRQs-masked early-boot precondition; a concurrent or IRQ-context re-entry could tear the latch check.
- [S3] src/kernel/arch/x86_64/early_init.cpp:694 — early_irq_init() registers the production scheduler-calling timer handler with no explicit global-IRQ mask in the sequence (carried over, still S3 at most)
  WHY: Safe only because the caller environment holds IRQs masked until the scheduler exists; the contract relies on ambient boot state rather than enforcing it.
- [S3] docs/specs/irq-early-init.md:37 — §3 CTRL_TIMEOUT still claims "GIC left disabled = safe state", contradicting §4 (distributor already enabled by init(); error only stops the bring-up) — developer's fix claim verified FALSE on disk
  WHY: ArchInterruptController::init() unconditionally enables GICD_CTLR + PPI 30 before the bounded WAKER poll, so on timeout the distributor is left enabled, contradicting the §3 sentence.
- [S3] src/kernel/arch/aarch64/timer.cpp:149-156,167-174 — disarm writes CNTP_TVAL_EL0=0 while ENABLE is still set, creating a transient met-condition before CTL=0 masks it
  WHY: Harmless only because global IRQs stay masked pre-scheduler and the PPI is level-sensitive (deassert clears it); CTL-first ordering would avoid the transient entirely.

## CHECKS
1. Dynamic allocations: none (no new/malloc/free in patch) — PASS
2. Concurrency boundaries: BOOT_ONLY latches + MMIO range guards + fenced PLIC writes; pre-scheduler single-core precondition documented — PASS (S3 notes above)
3. Assertion masking: new irq_early_std tests assert real behavior (OK, freq!=0, disarm remaining==0, monotonic, ticks+1); no weakened asserts — PASS
4. Memory safety: no PMM/BufferPool touched — PASS
5. Critical-section interference: ordered IDT→controller→timer→calibrate, fail-closed codes, x86 fail-open to PIC, AP path per-CPU only — PASS
6. Preprocessor semantics: CONFIG_ARCH_X86_64 AP guard symmetric; per-arch test branches match IrqState fields; GICv2 ready=true correct (no redistributor) — PASS
7. Context-retrieval artifacts: re-audit scope — no new retrieval burden beyond the two claimed fixes; prior disposition carries — PASS
S2 disarm finding from report-20260920T131258Z.md: FIXED on disk (TVAL zeroed before CTL disable in both aarch64 branches; ENABLE=0 masks the output per ARM spec, remaining()/remaining_ns() observe 0).

DECISION: APPROVED