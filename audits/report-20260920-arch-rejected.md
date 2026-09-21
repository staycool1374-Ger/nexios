# AUDIT REPORT 20260920T131258Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/irq-early-init.md, src/kernel/arch/aarch64/early_init.cpp, src/kernel/arch/aarch64/hal/gic.hpp, src/kernel/arch/aarch64/interrupt_controller.cpp, src/kernel/arch/aarch64/timer.cpp, src/kernel/arch/early_init.hpp, src/kernel/arch/hal/early_init.hpp, src/kernel/arch/hal/interrupt_controller.hpp, src/kernel/arch/hal/timer.hpp, src/kernel/arch/riscv64/early_init.cpp, src/kernel/arch/riscv64/interrupt_controller.cpp, src/kernel/arch/riscv64/timer.cpp, src/kernel/arch/x86_64/early_init.cpp, src/kernel/arch/x86_64/hal/timer.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_irq_early_std.cpp, src/kernel/test/test_registry.cpp

## FINDINGS
- [S2] src/kernel/arch/aarch64/timer.cpp:149 — oneshot(0)/periodic(0) disarm writes CNT_CTL=0 but leaves CNTP_TVAL_EL0 untouched, so the patch's own arch-neutral disarm test (remaining()==0, remaining_ns()==0) cannot pass on aarch64
  WHY: remaining() reads CNTP_TVAL_EL0 which freezes at the last programmed nonzero interval when the counter is disabled, and remaining_ns() derives from it — the in-tree comment at :167 ("leave the count for remaining()") concedes the mismatch.
- [S3] src/kernel/arch/x86_64/early_init.cpp:672, src/kernel/arch/aarch64/early_init.cpp:92, src/kernel/arch/riscv64/early_init.cpp:401 — g_early_irq_done is a plain non-atomic bool written without IrqGuard
  WHY: Safe only under the unstated single-core/IRQs-masked early-boot precondition; a concurrent or IRQ-context re-entry could tear the latch check.
- [S3] docs/specs/irq-early-init.md:44 — CTRL_TIMEOUT claims "GIC left disabled = safe state"
  WHY: ArchInterruptController::init() unconditionally enables GICD_CTLR + PPI 30 after the bounded WAKER poll, so on timeout the distributor is left enabled, contradicting the spec sentence.
- [S3] src/kernel/arch/x86_64/early_init.cpp:694 — early_irq_init() registers the production scheduler-calling timer handler with no explicit global-IRQ mask in the sequence (deviation (a))
  WHY: Safe only because the caller environment holds IRQs masked until the scheduler exists; the contract relies on ambient boot state rather than enforcing it.

## PATCH
audits/rejected_patch.diff was written and zeroes CNTP_TVAL_EL0 in both aarch64 disarm branches so remaining()/remaining_ns() observe 0 per the Timer disarm contract.

DECISION: REJECTED
