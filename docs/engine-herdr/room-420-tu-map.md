# Room 420 TU map — flat → colocated (steps 1–3 IMPL, #420)

Source of truth for batch moves. Rules: path-prefix-only moves, no renames,
no content edits except include-path fixes. Infra stays.

## Stay in src/kernel/test/ (harness-owned)

test_registry.cpp, test_isolate.{hpp,cpp}, test_cleanup.{hpp,cpp},
test_config.{hpp,cpp}, resource_tracker.{hpp,cpp}, task_ptr.hpp,
test_expected_counts.hpp, test_selftest.hpp, sched_helpers.hpp,
ring3_vehicle.hpp, test_watchdog.{cpp,hpp}, test_weak_stubs.cpp,
test_buildsystem.cpp, test_config_checks.cpp, test_infra.cpp,
test_testrunner.cpp, test_core_isolation.cpp, test_kernel_isolation.cpp,
test_kernel_top.cpp, test_boot_deterministic.cpp, test_no_dynamic_alloc_after_init.cpp,
test_no_op_new.cpp, test_cross_boundary.cpp, test_pointer_isolation.cpp,
test_syscall_affinity.cpp (harness-affinity; re-home at B9 if owner says so).

## Move map (basename prefix → destination)

- test_sched_*, test_o1_scheduler, test_sporadic_server, test_aperiodic_servers,
  test_load_balancer, test_starvation_deadlock, test_sched_edf,
  test_sched_admission*, test_sched_dispatch_path, test_deadline_*,
  test_budget, test_ss_deadline → src/kernel/task/tests/ (B1)
- test_task*, test_process, test_waitpid, test_zombie_cleanup, test_idle_*,
  test_wcet_cleanup, test_secure_exec → src/kernel/task/tests/ (B1)
- test_memory*, test_mempool, test_pmm, test_vmm*, test_pml4_*, test_pt_merge,
  test_page_tables, test_stack_*, test_slab_reclaim, test_cache_coloring,
  test_wcet_memory, test_lazy_tlb, test_tlb_*, test_freelist_consistency,
  test_static_pools, test_wcet_overrun → src/kernel/memory/tests/ (B2)
- test_ipc*, test_pipe*, test_spsc, test_queue_pip, test_sync*,
  test_spinlock*, test_locking*, test_mutex_pcp, test_pcp, test_pip,
  test_lock_order, test_lock_validator, test_per_cpu, test_smp_sync,
  test_atomic* → src/kernel/ipc/tests/ (B3; sync-owned subset to
  src/kernel/sync/tests/ if sync/ owns the primitive)
- test_cap_* → src/kernel/cap/tests/ (B4)
- test_vfs*, test_fat32, test_ramdisk*, test_initrd_*, test_tmpfs*,
  test_procfs_ops, test_vfsd*, test_devfs, test_fstab → src/kernel/vfs/tests/ (B5)
- test_elf*, test_fpu* (5 excluded TUs carry exclusion verbatim),
  test_fpu_context_user, test_fpu_inv → src/kernel/elf/tests/ (B6)
- test_apic_timer, test_lapic, test_ioapic, test_apic_tpr, test_gdt_layout,
  test_idt, test_invpcid, test_pcid, test_hal*, test_rtc*, test_hr_clock,
  test_serial_logic, test_keyboard_decode, test_cross_arch, test_smp_bringup,
  test_smp_ipi, test_smp_madt, test_smp_sched, test_smp_verify,
  test_gcov, test_profiler_sampler → src/kernel/arch/tests/ (B7)
- test_dma, test_pci, test_virtio*, test_ahci_deep, test_acpi_parse,
  test_iommu_live, test_iocd, test_block_device → src/kernel/driver/tests/ (B7)
- test_dmesg, test_klog → src/kernel/log/tests/ (B8)
- test_debug*, test_path_tracer → src/kernel/debug/tests/ (B8)
- test_net → src/kernel/net/tests/ (B8)
- test_shell_*, test_services_framework → src/kernel/console/tests/ (B8)
- test_lib*, test_checked_ptr*, test_textutils, test_random*,
  test_libc_* → src/lib/tests/ (B8)
- test_syscall*, test_microkernel_transition, test_signals, test_posix_time,
  test_rlimit, test_random_syscall → src/kernel/syscall/tests/ (B8)
- test_timer*, test_timing, test_jitter, test_stress_hrt, test_stress_24h,
  test_cpu_load, test_hr_clock (see arch row if hal-owned),
  test_scheduler_hrt → src/kernel/time/tests/ (B8)
- bench_*, test_cs_bench, test_ipc_benchmark, test_ipc_throughput,
  test_ipi_batching → bench-owned dir at move time, excluded from default
  classes as today (B9)
- test_daemon_restart_crash, test_watchdog_daemon, test_task_watchdog,
  test_task_metering, test_task_path, test_tcb_write_log,
  test_preemption*, test_priority_inheritance, test_pml4_sync,
  test_vmm_path, test_memory_safety, test_memory_isolation,
  test_memory_integrity, test_memory_determinism, test_buffer_pool*,
  test_wcet_*, test_elf_guard_e2e, test_exc_table, test_fstab (see vfs row),
  test_health, test_debug_dump, test_debug_syscall, test_dma (see driver row),
  test_resource_exhaustion, test_rlimit (see syscall row),
  test_slab_reclaim (see memory row) → owner dir per prefix table at move
  time; ambiguous homes resolved in the batch commit message.

## API stability (step 2)

JARVIS_REGISTER_TEST / JARVIS_REGISTER_RELEASE_TEST / TF_* untouched.
tools/gen_test_registry.py untouched (input_dir + --file-list dual mode
already location-agnostic; test_gen_registry.py already basename-matches
FPU exclusions). src/meson.build discovery now scans legacy dir + every
existing <subsys>/tests/ dir (fs.is_dir guard → output identical until
files move). tools/meson/tests/test_source_set.py FPU exclusion converted
to basename matching (FPU_EXCLUDED_NAMES); legacy exact-path set kept for
reference. mk/rules.mk + coverage_report.py + clean_test_suite.py
--test-dir + style_config.json excludes are follow-ups at first real move
(batch commit updates them; no moves in this diff).

## Verification rule (step 3, per batch)

test-file-list diff prefix-only + count unchanged →
validate_all_consistency → representative qemu class → make build Errors 0.
