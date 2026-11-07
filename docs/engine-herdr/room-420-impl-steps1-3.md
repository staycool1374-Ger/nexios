# Room 420 IMPL steps 1–3 — test colocation (#420)

Lane: dev. Worktree: /Users/arnold/jarvis-herdr-dev-wt1 (detached HEAD 7b8b59a67).
Transport note: /tmp/nherdr-420/prompts/dev-task.md + posts.jsonl still unreadable from this sandbox (external_directory deny; allowlist lacks /tmp/nherdr-*). The 8-step plan text was never received; steps 1–3 below are the docs-only groundwork from the RECON sketch (room-420-recon.md). No src/ moved, no branch created — #420 hard gate (plan → plan-audit APPROVED before moves) holds.

## Step 1 — destination map (prefix → src/<subsys>/tests/)

Infra STAYS in src/kernel/test/: test_registry.cpp, test_isolate.{hpp,cpp}, test_cleanup.{hpp,cpp}, test_config.{hpp,cpp}, resource_tracker.{hpp,cpp}, task_ptr.hpp, test_expected_counts.hpp, test_selftest.hpp, sched_helpers.hpp, ring3_vehicle.hpp, watchdog infra (test_watchdog.cpp/hpp), test_weak_stubs.cpp.

Moves (prefix → destination, renames: none):

| prefix | destination |
|---|---|
| test_sched_*, test_o1_scheduler, test_sporadic_server, test_aperiodic_servers, test_load_balancer, test_starvation_deadlock | src/kernel/task/tests/ |
| test_task*, test_process, test_waitpid, test_zombie_cleanup, test_idle_*, test_wcet_cleanup, test_secure_exec | src/kernel/task/tests/ |
| test_memory*, test_mempool, test_pmm, test_vmm*, test_pml4_*, test_pt_merge, test_page_tables, test_stack_*, test_slab_reclaim, test_cache_coloring, test_wcet_memory, test_lazy_tlb, test_tlb_* | src/kernel/memory/tests/ |
| test_ipc*, test_pipe*, test_spsc, test_queue_pip, test_sync*, test_spinlock*, test_locking*, test_mutex_pcp, test_pcp, test_pip, test_lock_order, test_lock_validator, test_per_cpu, test_smp_sync | src/kernel/ipc/tests/ (+ sync-owned subset to src/kernel/sync/tests/ if sync/ owns the primitive — decided at move time, map updated) |
| test_cap_* | src/kernel/cap/tests/ |
| test_vfs*, test_fat32, test_ramdisk*, test_initrd_*, test_tmpfs*, test_procfs_ops, test_vfsd*, test_devfs, test_fstab | src/kernel/vfs/tests/ |
| test_elf*, test_fpu* (5 excluded TUs keep exclusion), test_fpu_context_user, test_fpu_inv | src/kernel/elf/tests/ (FPU runtime stays arch-gated; test files move with exclusion list carried verbatim) |
| test_apic_timer, test_lapic, test_ioapic, test_apic_tpr, test_gdt_layout, test_idt, test_invpcid, test_pcid, test_hal*, test_rtc*, test_hr_clock, test_serial_logic, test_keyboard_decode, test_cross_arch | src/kernel/arch/tests/ (arch-subdir split x86_64/aarch64/riscv64 at move time if needed; default flat) |
| test_dma, test_pci, test_virtio*, test_ahci_deep, test_acpi_parse, test_iommu_live, test_iocd | src/kernel/driver/tests/ |
| test_dmesg, test_klog, test_debug*, test_profiler_sampler, test_path_tracer, test_gcov | src/kernel/debug/tests/ (log-owned: dmesg/klog → src/kernel/log/tests/ if log/ owns them) |
| test_net | src/kernel/net/tests/ |
| test_shell_*, test_services_framework | src/kernel/console/tests/ or services-adjacent dir at move time |
| test_lib*, test_checked_ptr*, test_textutils, test_random* | src/lib/tests/ (userspace-adjacent libc tests stay where the lib lives) |
| test_syscall*, test_microkernel_transition, test_pager (cap_pager stays cap), test_signals, test_posix_time, test_rlimit | src/kernel/syscall/tests/ |
| test_deadline_*, test_budget, test_ss_deadline, test_sched_edf, test_sched_admission*, test_sched_dispatch_path, test_sched_helpers | src/kernel/task/tests/ (scheduler-owned) |
| test_timer*, test_timing, test_jitter, test_hrtent (stress_hrt), test_stress_24h, test_cpu_load | src/kernel/time/tests/ |
| test_smp_bringup, test_smp_ipi, test_smp_madt, test_smp_sched, test_smp_verify, test_smp_sync (see ipc row) | src/kernel/arch/tests/ or smp-owner dir at move time |
| test_buildsystem, test_config*, test_infra, test_testrunner, test_core_isolation, test_kernel_*, test_boot_deterministic, test_no_* | STAY (harness-owned) until P3 decides their home |
| test_bench_*, test_cs_bench, bench_* | bench-owned dir at move time; excluded from default classes as today |

Rule: moves are path-prefix-only (no renames, no content edits except include-path fixes). The generated `.test-file-list` diff after each batch must show prefix changes only.

## Step 2 — build-system cutover design (mechanical, not yet applied)

Current (src/meson.build:19-55): single `test_src_dir = src_root/kernel/test`, `find *.cpp` minus 5 FPU TUs → `test_src_list` → `files()` + `test-file-list.in` stamp → `gen_test_registry.py <test_src_dir> --file-list <stamp> --arch <arch>`.

Cutover:
1. Replace single `test_src_dir` with a dir list: `test_src_dirs = [kernel/test (infra only), task/tests, memory/tests, ipc/tests, cap/tests, vfs/tests, elf/tests, arch/tests, driver/tests, debug/tests, log/tests, net/tests, console/tests, lib/tests, syscall/tests, time/tests, ...]` — one entry per Step-1 destination that exists.
2. Discovery loops `find <dir> -name *.cpp` per dir, concatenates; the 5 FPU `endswith()` exclusions apply unchanged (same predicate, N dirs).
3. `gen_test_registry.py` needs NO change: it already takes `input_dir` + `--file-list` (tools/gen_test_registry.py:401-437, scan_tests root/file_list dual mode). Pass the first dir as `input_dir` (scan root, unused when file-list given) and the concatenated stamp as `--file-list`; or pass repo root. Registry output is path-agnostic (scans listed files).
4. `selftest_infra_rel` (7) + `selftest_tests_rel` (19) literals (src/meson.build:85-114) rewritten to new paths 1:1; no set change. Proof artifact: `diff .test-file-list` before/after = same basenames, new prefixes, same count.
5. `generic_srcs` (src/meson.build:65-81): the `find src_root` walker already recurses into new `tests/` dirs automatically (only `kernel/arch` pruned) — test TUs stay excluded from `generic_srcs` only via `external_test_dir==''` gating as today; verify no double-compile (test TUs must remain kernel-linked exactly once, as today).
6. mk/rules.mk + tools/meson/test_source_set.py: mirror the dir list + FPU_EXCLUDED there (byte-parity rule per src/meson.build header).

## Step 3 — batch sequence + verification gates (not yet executed)

Order (small batches, each independently verifiable):
- B1: task/tests (sched+task prefixes) — largest, exercises registry + expected-counts path.
- B2: memory/tests. B3: ipc+sync/tests. B4: cap/tests. B5: vfs/tests. B6: elf+fpu/tests (exclusion-sensitive). B7: arch+driver/tests. B8: debug/log/net/console/lib/syscall/time/tests. B9: harness leftovers + CI/coverage/vault sweep.

Per-batch gates (all must pass before next batch):
1. `test-file-list` diff = prefix-only, count unchanged.
2. `validate_all_consistency` green (expected-counts tables untouched in value, paths if any updated).
3. One representative `python3 tools/meson/qemu_test.py --arch x86_64 --build debug --class <owner-class>` green.
4. `make build` (check-style) Errors 0.
5. Testbed-first: batches land on `testbed`, merge to `main` per PROMPT-dev; full `run_all_classes.sh` + SIL-3 audit at B9 only.

Non-goals: no renames, no API/flag changes, no #170 rebase, no coverage-universe edits (paths flow through).

## Execution record (B1–B9 landed on pilot/420-colocation, #420)

All batches executed as path-prefix-only `git mv` (no renames, no content
edits): 221 TUs moved, 24 stay (7 infra + 17 harness-owned incl.
test_syscall_affinity, test_init, test_health, test_resource_exhaustion,
test_release_build). Decisions at move time: locking primitives
(spinlock/locking/mutex_pcp/pcp/pip/lock_order/lock_validator/per_cpu/
sync/atomic) → src/kernel/sync/tests/ (owns the primitives); test_smp_sync
→ ipc/tests; test_tls/test_exc_table/test_gcov/test_profiler_sampler →
arch/tests; test_watchdog_daemon → cap/tests; test_framebuffer/test_driver
→ driver/tests; bench+throughput/benchmark/ipi_batching → new
src/kernel/bench/tests/ (added to the Meson dir list + Make patterns).

Build-system sweep in the same commit:
- src/meson.build: bench dir added; 19 selftest_tests_rel literals rewritten.
- mk/rules.mk: FPU filter-out → %/ basename patterns; SELFTEST_TESTS
  literals rewritten; external-test filter-out extended; arch/tests
  carve-out (pruned tree) + shared TEST_COLOCATED_PATTERNS.
- Makefile: TEST_SRC_FILES filter + CLANG_TIDY_FILTER use the shared patterns.
- tools/meson/tests/test_source_set.py: make_set() mirrors the arch carve-out.
- tools/validate_style.py is_test_file + tools/style_config.json excludes:
  moved tests keep test-file treatment (style Errors 0 before/after).
- tools/coverage_report.py: is_test_src() helper (legacy + */tests/).
- tools/check_no_cross_edits.sh + tools/agents/fence/roles/{dev,qe}.json:
  test-dir globs extended to */tests/.
- Left untouched (pre-existing, out of scope): 4 stale mk SELFTEST literals
  for files absent from the tree (test_gic/test_plic/test_threaded_irqs/
  test_irq_alloc); test_lib.cpp's relative ../../../userspace include still
  resolves (same depth).

Verification (this worktree, before commit):
- Meson discovery 240 = 240, make discovery 404 = 404, both prefix-only.
- Registry: 1789 = 1789 tests, byte-identical headers vs pristine HEAD.
- Selftest literals resolve (meson 26/26; mk all but the 4 stale).
- `validate_style.py --root src/kernel`: Errors 0 (rc 0), same as baseline.
- QEMU classes not run here (no toolchain in this sandbox); QE verify covers.
