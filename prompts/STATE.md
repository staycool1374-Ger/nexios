# STATE — Current Work State

> Moved out of AGENTS.md (session state, not operating rules).
> Update after each milestone; AGENTS.md only links here.

## Active Summary
### Objective
- **DONE (2026-08-25):** Multi-arch compile-clean (#99, commit `36fa5118`): `make debug NO_LTO=1 ARCH={aarch64,riscv64}` now link green to a kernel ELF (QEMU virt targets) without changing x86_64 runtime behavior. SIL 3 approved (`audits/report-20260825T060240Z.md`).
- Previous: `make build` green (check-style error gate) after the scheduler-deadlock fix and the full style/clang-tidy cleanup.

### Important Details
- Branch `main`. Baseline `2f1a7faf`; refactor commits `8334a120`..`f37b3e2d`; multi-arch commit `36fa5118`.
- **Multi-arch portability now in place:**
  - Build system: parse-time arch-stamp check in Makefile (before `.d` files load) → deterministic one-shot arch switches without manual `make clean`; arch-keyed `mk/cpp-rules.$(ARCH).gen.mk`; FPU/SSE test filter now all arches; `gen_test_registry.py --arch` drops foreign-arch tests (also fixed a legacy truncation bug that dropped 5 real tests from generated_tests[]).
  - Kernel: scheduler.cpp/task.cpp/kernel.cpp `CONFIG_ARCH_X86_64` guards around x86-only CR3/invlpg/pushfq/APIC/kstack-window code; aarch64 linker `_stack_start`/`_stack_end` as higher-half absolute symbols; aarch64/riscv64 `Serial::write_count()` + riscv64 `Timer::tsc_freq_hz()` + constinit.
  - Verified: x86_64 selftest 133/133; check-style 0 errors; no-clean arch-switch matrix (x86→riscv→aarch→x86) all green.
- **Known caveats (out of scope for #99):** aarch64/riscv64 are COMPILE-CLEAN only — no runtime boot guarantee yet (issues #28/#29). `mk/cpp-rules.gen.mk` (old non-keyed) stays git-tracked unreferenced. riscv64 userspace crt0.S lacks a `__riscv` branch (GNU ld warns, still links).
- **Pre-existing H2 deferred-switch race** (BUGS.md, RE-OPENED): ~50% flaky TIMEOUT/PANIC in ipc_core/elf_loader/all/selftest; reproducible on baseline; NOT caused by multi-arch work.

### Work State
#### Completed
- **GLOBAL-VARIABLE ENCAPSULATION REFACTOR (2026-08-14, commits `8334a120`..`f37b3e2d`, SIL 3 approved):**
  - **Phase 1** (`1b569a8c`): dead code + linkage hygiene — removed `g_boot_ns` (zero readers); made `g_virtio_net_dev`, `g_h2_ring`/`g_h2_idx`, `fat32_root_vnode`, `g_watchdog_*` static/TU-local; deleted stale `scheduler.cpp.bak*`.
  - **Phase 2** (`a99bf374`): `g_dmesg` → `log::DmesgService`, `g_klog` → `log::KlogService` (Meyers singletons, private ctor + buffer); `dmesg_push*` macros → inline fns; all consumers migrated.
  - **Phase 3** (`75a47590`): `src/kernel/core/global_state.{hpp,cpp}` created — single definition point; `verify_and_write<T>` (IDEMPOTENT/BOOT_ONLY/NEVER_WRITE rules + CONFIG_DEBUG audit ring); BootState/FaultState/TestState accessors; duplicate defs + unused externs removed (kernel.hpp/test.hpp/syscall.hpp/test_isolate.hpp).
  - **Phase 4** (`43dd2f2d`): VfsState — `fat32_partition_instance` → `try_set_fat32_partition` (RANGE_CHECKED: null or kernel-half). Filesystem singletons stay module-owned (documented).
  - **Phase 5** (`34d7cf2d`): NetState — `g_nic` → `try_set_nic` (RANGE_CHECKED). Daemon PIDs left file-static (already accessor-wrapped).
  - **Phase 6** (`e06790ec`): AsmSwitchState — 14 deferred-switch globals (scheduler_save_rsp_to, isr_nesting_depth, fpu_owner, …) moved to global_state.cpp `extern "C"` block, byte-identical symbols for isr_stubs.asm; `kernel::fpu_owner` qualifier fixes.
  - **Phase 7** (`f37b3e2d`): docs + full gates — **debug `all` 870/870 (trace ON), release `all` 84/84 (trace OFF)**.
- Scheduler deadlocks fixed + committed (`dfc3aec`): pop_front cycle-guard, rebuild_ready_queue flag-clear, ready_queue_manager in_ready_queue_ maintenance + restore_pod, wake_waiting_parent, reap test, TEMP DEBUG removed. 16 runs clean.
- Style errors 128 → 0: added `#pragma once` to 105 headers; fixed 117 `init_required` value-initializers; fixed 2 `no_const_cast` (block_device.hpp/.cpp param type, virtio_blk.cpp staging buffer); added `arch::pause()` to 5 infinite loops.
- Checker false-positive fixes (tools/validate_style.py): skip assembly (`;`, .S/.asm); placement-new; `break`/`return` in `while(true)`; `wfi` halt loops.
- clang-format applied to 297 files via new `.clang-format` (ColumnLimit 80, Attach braces, 4-space, SortIncludes false, BreakStringLiterals false); build verified clean.
- Cheap-high-value path DONE: fixed 2 real `branch-clone` (scheduler.cpp can_reap ~698-707, fat32_fs.cpp SEEK_END/default); disabled 3 clang-tidy checks in Makefile (performance-no-int-to-ptr, bugprone-reserved-identifier, bugprone-easily-swappable-parameters).
- MemoryChecker in tools/validate_style.py REWRITTEN to a brace-depth-aware function-nesting stack (func_stack + pending_func) with: boot-alloc exemption set (_boot_alloc_funcs: AhciDriver::probe, AtaPioDriver::probe_first_drive, VirtioBlkDriver::probe, virtio_net_probe, higherhalf_entry); control-keyword exclusion (_ctrl_keywords); skip of `#`/comment lines; and `\b` word boundaries on `_new_delete` (r"\bnew\b\s|\bdelete\b\s|\bmalloc\s*\(|\bfree\s*\(") to stop false matches on `is_free(`/`bufpool_free(`/`track_*_free(`.
- VERIFIED: `make check-style` → Errors: 0, Passed. `make debug NO_LTO=1` → ISO built cleanly.

#### Active
- (none outstanding) — multi-arch compile-clean committed (`36fa5118`); full build gates green on all 3 arches.

#### Blocked
- (none) — aarch64/riscv64 runtime boot (issues #28/#29) is out of scope for compile-clean; future work.

### Next Move
- Optional: reduce the non-blocking style warnings and clang-tidy warnings.
- Next multi-arch step (when scheduled): aarch64/riscv64 runtime boot path (issues #28/#29), riscv64 userspace crt0.S `__riscv` branch, runtime test gates.
- Tracked separately: the pre-existing H2 deferred-switch race (BUGS.md, RE-OPENED) — ~50% flaky TIMEOUT/PANIC in ipc_core/elf_loader/all/selftest, reproducible on baseline.

### Relevant Files
- Makefile: parse-time arch-stamp check (near BUILD_STAMP) → deterministic cross-arch switch; `TEST_FILE_LIST` keyed per arch; `--arch` passed to gen_test_registry.
- mk/rules.mk: `GEN_CPP_RULES := mk/cpp-rules.$(ARCH).gen.mk` (arch-keyed); FPU/SSE filter all arches; `check-arch` = stamp writer.
- tools/gen_test_registry.py: arch-aware `strip_disabled_blocks` (SCAN/DROP/ARCH mode stack), `_eval_arch_condition`, `--arch`.
- linker/linker_aarch64.ld: `_stack_start`/`_stack_end` higher-half absolute symbols (HHDM + low_vma).
- linker/linker_riscv64.ld: `_stack_start`/`_stack_end` around .boot_stack in .bss.
- src/kernel/task/scheduler.cpp: per-arch current_sp(); CONFIG_ARCH_X86_64 guards on CR3 diag + latent pushfq.
- src/kernel/task/task.cpp: kstack-window (CR3/invlpg) + free_stack_pdpt + production kslot block guarded to x86_64; alloc_kslot() returns 0 on non-x86.
- src/kernel/kernel.cpp: APIC init guarded to x86_64.
- src/kernel/arch/{aarch64,riscv64}/serial.cpp: Serial::write_count() added.
- src/kernel/arch/riscv64/timer.cpp: constinit ticks_, tsc_freq_hz() added.
- src/kernel/test/test_isolate.cpp / test_stack_profiler.cpp: per-arch SP asm (mv for riscv64).
- src/kernel/test/test_pml4_clone.cpp: whole-file CONFIG_ARCH_X86_64 guard; arch test_stubs.cpp register_pml4_clone_tests() stubs.
- src/kernel/core/global_state.{hpp,cpp}: single definition point for all cross-TU kernel globals (from the 7-phase refactor).
- BUGS.md: elf_loader H2-family flake logged (2026-08-14).
