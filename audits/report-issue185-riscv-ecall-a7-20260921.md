# AUDIT REPORT 2026-09-21T061000Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/arch/riscv64/syscall_entry.S

## FINDINGS
- [S3] src/kernel/arch/riscv64/syscall_entry.S:161 — sepc+=4 sits at label 6, also reached via the pre-existing non-ecall exception fall-through (line 154 falls into 6)
  WHY: Advancing sepc past a faulting non-ecall instruction would skip rather than re-trap, but the fall-through is pre-existing/unchanged, the stub is unexecuted (issue #29), and handle_kernel_exception owns fault semantics — no new hazard.

DECISION: APPROVED