# AUDIT REPORT 20260825T060240Z
PATCH: audits/pending_patch.diff
FILES: Makefile, mk/rules.mk, tools/gen_test_registry.py, linker/linker_aarch64.ld, linker/linker_riscv64.ld, src/kernel/arch/aarch64/serial.cpp, src/kernel/arch/aarch64/test_stubs.cpp, src/kernel/arch/aarch64/timer.cpp, src/kernel/arch/riscv64/serial.cpp, src/kernel/arch/riscv64/test_stubs.cpp, src/kernel/arch/riscv64/timer.cpp, src/kernel/debug/dump.cpp, src/kernel/kernel.cpp, src/kernel/task/scheduler.cpp, src/kernel/task/task.cpp, src/kernel/test/test_isolate.cpp, src/kernel/test/test_pml4_clone.cpp, src/kernel/test/test_stack_profiler.cpp

## FINDINGS
- [S3] scheduler.cpp:current_sp() — split combined `defined(CONFIG_ARCH_AARCH64) || defined(CONFIG_ARCH_RISCV64)` into separate `#elif` branches with arch-specific asm (`mov` for aarch64, `mv` for riscv64). WHY: Correct per-arch SP-reading instruction; matches existing test_isolate.cpp implementation.
- [S3] scheduler.cpp:switch_away_from_terminating() — changed `&exiting.context.rsp` to `&TASK_STACK_PTR(&exiting)` with TASK_STACK_PTR macro; CR0.TS canary write guarded with `#if defined(CONFIG_ARCH_X86_64)`. WHY: TASK_STACK_PTR is arch-aware macro (rsp/sp_el0/sp); CR0.TS write is x86_64-specific only.
- [S3] scheduler.cpp:3 latent pushfq reads in scheduler_diag_depth_skip/, scheduler_diag_rsp_abort/, scheduler_record_skip() — guarded with `#if defined(CONFIG_ARCH_X86_64)`. WHY: pushfq is x86_64-only instruction; prevents non-x86 compilation.
- [S3] task.cpp:_task_trampoline — guarded with `#if defined(CONFIG_ARCH_X86_64)`. WHY: Infinite hlt loop fix is x86_64-specific.
- [S3] task.cpp:alloc_kslot() returns 0 on non-x86; init_kstack_window/map_kstack_page/unmap_kstack_page/free_kslot/kslot_snapshot_restore all guarded with CONFIG_ARCH_X86_64. WHY: Kernel-stack window (CR3/invlpg PTE wiring) is x86_64-only; other arches use HHDM direct-map fallback.
- [S3] task.cpp:done_stack label + stack_va var guarded with `#if defined(CONFIG_ARCH_X86_64)` in TaskControlBlock::create(). WHY: Consistent with kslot VA window being x86_64-only.
- [S3] task.cpp:create_user() + clone() — map_kstack_page calls guarded with CONFIG_ARCH_X86_64. WHY: Same kstack window rationale.
- [S3] task.cpp:free_stack_pdpt() guarded with CONFIG_ARCH_X86_64. WHY: Private stack PDPT is x86_64 (4-level PML4/PDPT/PD/PT) concept; AArch64/RISC-V use different page-table shapes.
- [S3] task.cpp:cleanup() — unmap_kstack_page / free_kslot / free_stack_pdpt all guarded with CONFIG_ARCH_X86_64. WHY: Ensures kslot/free-stack PDPT cleanup only runs on x86_64 where the window exists.
- [S3] kernel.cpp:APIC init block guarded with `#if defined(CONFIG_ARCH_X86_64)`. WHY: APIC is x86_64-specific; AArch64/RISC-V use their interrupt controllers.
- [S3] debug/dump.cpp:pkv2/pkv3 functions guarded with `#if defined(CONFIG_ARCH_X86_64)`. WHY: Functions use x86_64-specific register names (context.cs, context.ss).
- [S3] debug/dump.cpp:dump_task_info() context register dump — arch-aware: x86_64 (rsp/rip), aarch64 (sp_el0/elr_el1/spsr_el1), default (sp/sepc). WHY: Correct register names per architecture.
- [S3] debug/dump.cpp:dump_all_tasks() deferred-switch target detection — arch-aware with arch-specific saved_sp and context.rsp comparisons. WHY: Correct per-arch context field access.
- [S3] debug/dump.cpp:dump_all_tasks() register dump — arch-aware (x86_64: rsp/rip, aarch64: sp_el0/elr_el1, default: sp/sepc). WHY: Correct register names per architecture.
- [S3] tools/gen_test_registry.py — arch-aware `strip_disabled_blocks()` with `_eval_arch_condition()` that evaluates `#if defined(CONFIG_ARCH_*)` blocks against target architecture; leftmost-arch-false && rule drops foreign-arch blocks; `--arch` CLI argument added. WHY: Prevents undefined symbols on non-x86 by not emitting tests inside foreign-arch `#if defined(CONFIG_ARCH_*)` blocks; fixes legacy truncation bug where old `#else` depth handling dropped 5 real tests from generated_tests[].
- [S3] linker/linker_aarch64.ld — `_stack_start`/`_stack_end` as higher-half absolute symbols (`0xFFFF800000000000 + _stack_start_low`) to avoid AArch64 ADR_PREL_PG_HI21 relocations against low-VMA section. WHY: Compile/link-only fix for relocation errors; no runtime guarantee claimed.
- [S3] linker/linker_riscv64.ld — `_stack_start`/`_stack_end` around .boot_stack in .bss (already higher-half). WHY: Already correct higher-half placement; no change in behavior.
- [S3] src/kernel/arch/aarch64/serial.cpp — added `Serial::write_count()` (atomic counter, matching x86_64). WHY: Consistent serial console write counter across architectures.
- [S3] src/kernel/arch/riscv64/serial.cpp — added `Serial::write_count()` (atomic counter, matching x86_64). WHY: Consistent serial console write counter across architectures.
- [S3] src/kernel/arch/aarch64/timer.cpp — changed `volatile uint64_t Timer::ticks_ = 0` to `constinit uint64_t Timer::ticks_ = 0`; added `Timer::tsc_freq_hz()`. WHY: const-correctness improvement; resolves header conflict with constinit; was volatile unnecessarily.
- [S3] src/kernel/arch/riscv64/timer.cpp — changed `volatile uint64_t Timer::ticks_ = 0` to `constinit uint64_t Timer::ticks_ = 0`; added `Timer::tsc_freq_hz()`. WHY: Same const-correctness improvement.
- [S3] src/kernel/arch/aarch64/test_stubs.cpp — added `register_pml4_clone_tests()` stub. WHY: PML4 page-table tests are x86_64-specific; stub provides clean skip message.
- [S3] src/kernel/arch/riscv64/test_stubs.cpp — added `register_pml4_clone_tests()` stub. WHY: Same rationale; PML4 tests are x86_64-specific.
- [S3] src/kernel/test/test_isolate.cpp:current_sp() — split per-arch `mov %0, sp` (`mv` riscv64). WHY: Correct SP-reading instruction per architecture; matches existing implementation.
- [S3] src/kernel/test/test_stack_profiler.cpp — `stack_profiler_context_rsp_in_range` assertion now arch-aware (x86_64: context.rsp, aarch64: context.sp_el0, riscv64: context.sp). WHY: Correct register name per architecture.
- [S3] src/kernel/test/test_stack_profiler.cpp — `stack_profiler_current_task_stack_valid` sp-read now arch-aware (x86_64: `mov %%rsp`, aarch64: `mov %0, sp`, riscv64: `mv %0, sp`). WHY: Correct SP-reading instruction per architecture.
- [S3] mk/rules.mk — FPU/SSE test filter comment updated to apply on all arches; `GEN_CPP_RULES` keyed per arch (`mk/cpp-rules.$(ARCH).gen.mk`); compile rules add `| check-arch` order-only prerequisite; `check-arch` target simplified to stamp writer. WHY: Arch-keyed compile rules prevent cross-arch build contamination; check-arch safety net persists the stamp.
- [S3] mk/rules.mk — ARCH_STAMP parse-time clean mechanism in Makefile: on ARCH change, `rm -rf build initrd_root debug release profiling` before .d files load, guaranteeing objects re-derive for target arch. WHY: Ensures fresh build derivation when switching architectures.
- [S3] mk/rules.mk — TEST_FILE_LIST keyed per arch (`build/.test-file-list-$(ARCH)`). WHY: Cross-arch switch never consumes a stale file list.
- [S3] Makefile — ARCH_STAMP parse-time check before mk/rules.mk -includes .d files; on ARCH change, cleans build dir before dependency files load. WHY: Guarantees every object is re-derived for the target architecture; prevents stale .d files from causing "object up to date" with wrong arch.
- [S3] Makefile — TEST_FILE_LIST keyed per arch; gen_test_registry.py called with `--arch $(ARCH)`. WHY: Consistent arch-aware file list and registry generation.

## PATCH
audits/rejected_patch.diff was NOT written; all findings are S3-only (style/convention/design improvements with no blocker or major defect violations).

## DECISION: APPROVED
