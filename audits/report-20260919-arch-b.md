# AUDIT REPORT 2026-09-19T164046Z
PATCH: audits/pending_patch.diff
FILES: mk/cpp-rules.gen.mk, src/kernel/arch/aarch64/hal/io_impl.hpp, src/kernel/arch/aarch64/vectors.S, src/kernel/arch/riscv64/hal/io_impl.hpp, src/kernel/arch/riscv64/syscall_entry.S, src/kernel/arch/x86_64/hal/msr_impl.hpp, src/kernel/arch/x86_64/isr_stubs.asm, src/kernel/core/global_state.cpp, src/kernel/elf/elf.cpp, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_errors.hpp, src/kernel/syscall/syscall_handlers_tls.cpp, src/kernel/task/scheduler.cpp, src/kernel/task/scheduler.hpp, src/kernel/task/task.cpp, src/kernel/task/task.hpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_tls.cpp, src/libc/syscall.h

## FINDINGS
- [S3] audits/pending_plan-74.md — no graphify/vault query dispositions visible to auditor
  WHY: grep for graphify/vault/retriev in the plan returns zero lines, so check 7 is evaluated against the plan's code-anchor citations only with the issue-#74-thread evidence noted as unreachable.
- [S3] src/kernel/arch/aarch64/vectors.S, src/kernel/arch/riscv64/syscall_entry.S — live-epilogue consume pinned only by C++ clear-helper tests on x86 execution
  WHY: tls_cancel_clears_slot and tls_no_stale_slot_after_cancel drive cancel_pending_switch_cpu/clear_switch_globals, so real aarch64/riscv64 epilogue apply awaits #28-family execution as the plan documents.

DECISION: APPROVED
