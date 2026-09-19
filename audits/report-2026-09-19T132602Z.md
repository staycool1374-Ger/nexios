# AUDIT REPORT 2026-09-19T132602Z
PATCH: audits/pending_plan-74.md v2 (plan review — no code diff yet)
FILES: src/kernel/task/task.hpp, src/kernel/task/task.cpp, src/kernel/elf/elf.cpp, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_tls.cpp, src/kernel/task/scheduler.hpp, src/kernel/task/scheduler.cpp, src/kernel/syscall/syscall_errors.hpp, src/kernel/arch/x86_64/hal/msr_impl.hpp, src/kernel/arch/aarch64/hal/io_impl.hpp, src/kernel/arch/riscv64/hal/io_impl.hpp, src/kernel/core/global_state.hpp, src/kernel/core/global_state.cpp, src/kernel/arch/x86_64/isr_stubs.asm, src/kernel/arch/aarch64/vectors.S, src/kernel/arch/riscv64/syscall_entry.S, src/kernel/test/test_tls.cpp, src/libc/syscall.h

## FINDINGS
- [S2] src/kernel/arch/aarch64/vectors.S:248 — .tls_apply inserted between CR3 clear (:248) and isb (:249) breaks TTBR/isb pairing
  WHY: On the CR3-taken/TLS-zero path `cbz x3, .restore` jumps over the :249 isb, leaving a TTBR0 switch + TLBI with no instruction-synchronization barrier before ERET.
- [S3] src/kernel/arch/riscv64/syscall_entry.S:212 — inserted block reuses numeric label `7:` already defined at :212
  WHY: `beqz t1, 7f` binds to the nearest forward `7:` so behavior is accidentally correct, but duplicate local labels are fragile — use a unique label.
- [S3] src/kernel/task/scheduler.cpp:678 — live-apply gate uses `is_current_on_any_cpu` precedent for a local register write
  WHY: FS_BASE/TPIDR/tp writes affect only this CPU, so the gate must be `&task == current_task()` — harmless today (single-core, always-self call sites) but wrong on SMP if the API ever targets a remote-running task.
- [S3] src/kernel/arch/aarch64/vectors.S:208 — no-abort/publish+arm atomicity invariant has no aarch64-side test
  WHY: The relied-upon invariant is pinned only by x86 `tls_cancel_clears_slot`, which cannot exercise the aarch64 epilogue path.
- [S3] docs/specs/syscall-abi-picolibc.md:133 — binding spec still mandates LA57 live-mode query vs plan's static 48-bit gate
  WHY: Zero LA57 implementation symbols tree-wide makes the static gate exact today, but the spec/plan asymmetry persists — the plan's recorded trigger condition (widen gate if LA57 lands) is the correct mitigation, spec text should follow.

DECISION: REJECTED
