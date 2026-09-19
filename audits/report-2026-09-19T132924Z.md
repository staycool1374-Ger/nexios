# AUDIT REPORT 2026-09-19T132924Z
PATCH: audits/pending_plan-74.md v3 (plan review — no code diff yet)
FILES: src/kernel/task/task.hpp, src/kernel/task/task.cpp, src/kernel/elf/elf.cpp, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_tls.cpp, src/kernel/task/scheduler.hpp, src/kernel/task/scheduler.cpp, src/kernel/syscall/syscall_errors.hpp, src/kernel/arch/x86_64/hal/msr_impl.hpp, src/kernel/arch/aarch64/hal/io_impl.hpp, src/kernel/arch/riscv64/hal/io_impl.hpp, src/kernel/core/global_state.hpp, src/kernel/core/global_state.cpp, src/kernel/arch/x86_64/isr_stubs.asm, src/kernel/arch/aarch64/vectors.S, src/kernel/arch/riscv64/syscall_entry.S, src/kernel/test/test_tls.cpp, src/libc/syscall.h

## FINDINGS
- [S3] src/kernel/task/scheduler.cpp:657 — live-apply TOCTOU guard scope is implicit, not stated
  WHY: Step 9 releases `scheduler_lock_` before the `&task == current_task()` gate plus live register write, which is race-free only if `irq_guard` (mirrored from set_affinity_err) is still held across it — the plan must state that explicitly or a narrowly-scoped implementer reintroduces a preemption window.
- [S3] src/kernel/arch/aarch64/vectors.S:208 — live-epilogue consume remains unpinned by test 13
  WHY: `tls_no_stale_slot_after_cancel` drives only the C++ clear helpers, so the vectors.S/syscall_entry.S consume half is still uncovered until #28-family real-arch execution — honestly documented, not silent, hence non-blocking.

DECISION: APPROVED
