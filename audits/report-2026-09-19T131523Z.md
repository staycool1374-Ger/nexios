# AUDIT REPORT 2026-09-19T131523Z
PATCH: audits/pending_plan-74.md (plan review — no code diff yet)
FILES: src/kernel/task/task.hpp, src/kernel/task/task.cpp, src/kernel/elf/elf.cpp, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_tls.cpp, src/kernel/syscall/syscall_errors.hpp, src/kernel/arch/x86_64/hal/msr_impl.hpp, src/kernel/task/scheduler.hpp, src/kernel/core/global_state.hpp, src/kernel/core/global_state.cpp, src/kernel/task/scheduler.cpp, src/kernel/arch/x86_64/isr_stubs.asm, src/kernel/arch/aarch64/vectors.S, src/kernel/arch/riscv64/syscall_entry.S, src/kernel/test/test_tls.cpp, src/kernel/test/test_expected_counts.hpp, src/libc/syscall.h

## FINDINGS
- [S2] src/kernel/syscall/syscall_handlers_tls.cpp (plan step 7) — direct SpinLockGuard on scheduler_lock_ with no IrqGuard
  WHY: scheduler_lock_ is private (scheduler.hpp:933) and task-context TCB mutation requires IrqGuard + lock (set_affinity_err precedent scheduler.cpp:655, scheduler.hpp:209), else the timer ISR taking the same lock deadlocks and the plan as written does not compile.
- [S2] src/kernel/task/scheduler.cpp:947,1802,1827,1984,2013,3810,4674,3987 — TLS slot missing at 7 CR3-clear sites and the second publish site
  WHY: Every path that zeroes load_cr3_from must zero load_tls_from and the ~3972-3992 dispatch path must publish TLS alongside CR3, or a later switch consumes a stale base (wrong-thread TLS, the plan's own declared S2).
- [S2] src/kernel/arch/riscv64/syscall_entry.S:227 — plan step 11 live-tp write is clobbered before sret
  WHY: The GPR restore (ld x4, OFF_TP(sp)) runs after the satp switch, so the apply must update the OFF_TP save area or write post-restore, else TLS never takes effect on riscv64.
- [S2] src/kernel/elf/elf.cpp:715 + plan step 7 — no self-set live apply; exec path has no reset step
  WHY: The setter runs on a stale FS_BASE/TPIDR/tp until next preemption (breaks the §13 crt0 TLS_SET-then-use sequence), and exec_into_current reuses the live TCB with no tls reset though the plan claims "exec resets to 0" (only finalize is stepped).
- [S3] src/kernel/memory/address.hpp:111 — LA57 live-query helper does not exist; is_canonical is 48-bit-only
  WHY: No paging_levels/five-level/CR4.LA57 symbol exists anywhere and x86 CONFIG_USER_SPACE_LIMIT (nexios_config.h:141) already caps at 47-bit, so the planned live-mode branch is dead untestable code.
- [S3] src/kernel/arch/aarch64/vectors.S:237 — plan assumes per-CPU slot[cpu] on archs with single-slot access and no abort path
  WHY: aarch64/riscv consume the array base only (scheduler.hpp:1045-1048) with skip-to-restore paths that never clear, so the plan must mirror that pattern explicitly with its staleness argument.
- [S3] src/kernel/arch/x86_64/isr_stubs.asm:422 — asm insert underspecifies preserved registers and fallback-path behavior
  WHY: WRMSR needs ecx/edx/eax so the insert must name the exact preserve set (rax/rcx/rdx + r11 index, no calls), and .load_cr3 is also reached via the kernel_cr3 fallback where a nonzero TLS slot must be impossible (depends on the S2 clear-site fix).
- [S3] src/kernel/syscall/syscall_errors.hpp:44 — TLS_INVALID_BASE=110 is not the first free ID above the task band
  WHY: The task band ends at 108 and 109 is also unused, so use 109 or document the reservation, and confirm no error-count assertion needs updating.
- [S3] audits/pending_plan-74.md:3 — no graphify/vault retrieval dispositions stated in the plan
  WHY: The issue #74 thread allegedly holds them but is unreachable to this auditor, so this check is evaluated against the plan's own spec citations (all anchors verified real) with the GitHub-side check noted as a limitation.

DECISION: REJECTED
