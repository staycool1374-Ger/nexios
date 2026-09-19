/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/// @file test_registry.cpp
/// @brief Test registry implementation.

#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-null-argument"
#pragma GCC diagnostic ignored "-Wanalyzer-possible-null-dereference"
#endif

#include <test.hpp>
#include <logger.hpp>
#include <string.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/test/test_expected_counts.hpp>

using namespace kernel;

// ---- forward declarations for per-file registration functions ----

void register_lib_tests();
void register_memory_tests();
void register_ipc_tests();
void register_scheduler_tests();
void register_task_tests();
void register_driver_tests();
void register_vfs_tests();
void register_tmpfs_tests();
void register_tmpfs_invalid_mount_tests();
void register_tmpfs_mount_unmount_failure_tests();
void register_signals_tests();
void register_process_tests();
void register_elf_tests();
void register_elf_loader_tests();
void register_libc_verify_tests();
void register_elf_shared_tests();
void register_checked_ptr_tests();
void register_fstab_tests();
void register_rtc_tests();
void register_rlimit_tests();
void register_init_tests();
void register_syscall_tests();
void register_sync_tests();
void register_sync_block_pattern_tests();
void register_spinlock_tests();
void register_task_lifecycle_tests();
void register_idle_task_tests();
void register_zombie_cleanup_tests();
void register_tcb_write_log_tests();
void register_wcet_cleanup_tests();
void register_idle_cleanup_tests();
void register_testrunner_tests();
void register_expected_panic_tests();
void register_freelist_consistency_tests();
void register_infra_tests();
void register_config_checks_tests();
void register_wcet_memory_tests();
void register_no_dynamic_alloc_tests();
void register_vfsd_tests();
void register_iocd_tests();
void register_health_tests();
void register_timer_tests();
void register_timing_tests();
void register_spsc_tests();
void register_preemption_under_syscall_tests();
void register_spinlock_stress_tests();
void register_atomic_context_switch_tests();
void register_bench_syscall_latency_tests();
void register_bench_irq_latency_tests();
void register_apic_timer_tests();
void register_apic_tpr_tests();
void register_jitter_tests();
void register_idt_tests();
void register_exc_table_tests();
void register_pipe_tests();
void register_gcov_tests();
void register_debug_tests();
void register_framebuffer_tests();
void register_pml4_clone_tests();
void register_pt_merge_tests();
void register_cap_core_tests();
void register_cap_lifecycle_tests();
void register_cap_syscall_tests();
void register_cap_ipc_tests();
void register_cap_untyped_tests();
void register_cap_mmio_tests();
void register_cap_mmio_user_tests();
void register_cap_irq_tests();
void register_cap_irq_notify_tests();
#if defined(CONFIG_ARCH_X86_64)
void register_cap_msix_tests();
#endif
void register_cap_iommu_tests();
void register_iommu_live_tests();
void register_waitpid_tests();
void register_buffer_pool_tests();
void register_block_device_tests();
void register_fat32_tests();
void register_vfs_fat32_tests();
void register_ipc_blocking_tests();
void register_ipc_timeout_tests();
void register_vfsd_authorization_tests();
void register_textutils_tests();
void register_shell_interaction_tests();
void register_debug_dump_tests();
void register_profiler_sampler_tests();
void register_kernel_top_tests();
void register_per_cpu_tests();
void register_sync_err_api_tests();
void register_checked_ptr_api_tests();
void register_memory_integrity_tests();
void register_shell_commands_tests();
void register_services_framework_tests();
void register_irq_guard_tests();
void register_shell_redirect_tests();
void register_klog_tests();
void register_dmesg_tests();
void register_hal_tests();
void register_buildsystem_tests();
void register_secure_exec_tests();
void register_pci_tests();
void register_virtio_tests();
void register_dma_tests();
void register_net_tests();
void register_ipc_benchmark_tests();
void register_ipc_robustness_tests();
void register_syscall_fuzz_tests();
void register_starvation_deadlock_tests();
void register_deadline_miss_tests();
void register_wcet_overrun_tests();
void register_wcet_scheduler_tests();
void register_deadline_action_tests();
void register_ss_deadline_tests();
void register_deadline_recovery_tests();
void register_priority_inheritance_tests();
void register_queue_pip_tests();
void register_mutex_pcp_tests();
void register_resource_exhaustion_tests();
void register_microkernel_transition_tests();
void register_random_tests();
void register_random_vfs_tests();
void register_random_syscall_tests();
void register_random_seed_tests();
void register_fpu_tests();
void register_fpu_sse_tests();
void register_fpu_clone_tests();
void register_fpu_multi_tests();
void register_fpu_xmm_all_tests();
void register_random_vfs_write_tests();
void register_ipc_lock_free_tests();
void register_locking_tests();
void register_locking_stress_tests();
void register_preemption_tests();
void register_ipc_extended_tests();
void register_daemon_restart_crash_tests();
void register_irqguard_audit_tests();
void register_memory_safety_tests();
void register_memory_determinism_tests();
void register_pmm_tests();
void register_mempool_tests();
void register_slab_reclaim_tests();
void register_sporadic_server_tests();
void register_syscall_affinity_tests();
void register_tls_tests();
void register_atomic_tests();
void register_cross_arch_tests();
void register_o1_scheduler_tests();
void register_vmm_tests();
void register_hal_bits_tests();
void register_lock_order_tests();
void register_budget_tests();
void register_pip_tests();
void register_pcp_tests();
void register_lock_validator_tests();
void register_cpu_load_tests();
void register_static_pools_tests();
void register_stack_profiler_tests();
void register_stack_alloc_tests();
void register_page_tables_tests();
void register_kernel_isolation_tests();
void register_memory_isolation_tests();
void register_buffer_pool_deterministic_tests();
void register_no_op_new_tests();
void register_stress_hrt_tests();
void register_scheduler_hrt_tests();
void register_hrt_monotonic_tests();
void register_timer_wheel_tests();
void register_task_metering_tests();
void register_sched_edf_tests();
void register_sched_admission_tests();
void register_sched_admission_verify_tests();
void register_aperiodic_servers_tests();
void register_syscall_fastpath_tests();
void register_fpu_inv_tests();
void register_cap_shm_tests();
void register_cap_death_tests();
void register_cap_pager_tests();
void register_ipc_fastpath_tests();
#if defined(CONFIG_ARCH_X86_64)
void register_rtc_datetime_tests();
void register_keyboard_decode_tests();
void register_gdt_layout_tests();
void register_serial_logic_tests();
void register_acpi_parse_tests();
void register_smp_madt_tests();
void register_smp_ipi_tests();
void register_smp_bringup_tests();
void register_sched_affinity_tests();
void register_smp_sched_tests();
void register_lapic_tests();
void register_ioapic_tests();
void register_core_isolation_tests();
void register_load_balancer_tests();
void register_cache_coloring_tests();
void register_smp_sync_tests();
void register_smp_verify_tests();
void register_pcid_tests();
void register_invpcid_tests();
void register_lazy_tlb_tests();
void register_ipi_batching_tests();
void register_tlb_latency_tests();
void register_pml4_sync_tests();
#endif
void register_virtio_blk_req_tests();
void register_ahci_deep_tests();
void register_ahci_live_tests();
void register_initrd_parser_tests();
void register_vfs_procfs_tests();
void register_vfs_tmpfs_corrupt_tests();
void register_devfs_tests();
void register_procfs_ops_tests();
void register_initrd_fs_tests();
void register_vfs_errors_tests();
void register_pipe_blocking_tests();
#if defined(CONFIG_ARCH_AARCH64)
void register_aarch64_tests();
#endif
#if defined(CONFIG_ARCH_RISCV64)
void register_riscv64_tests();
#endif

// ---- External test registration hook (weak symbol) ----
// When the external test suite is linked in, it provides a strong override
// that registers test classes defined in the external repo.  When no external
// tests are linked, this is a no-op.
__attribute__((weak)) void register_external_test_classes() {}

// ---- Per-file registration helpers (issue #173) ----
// Each fine-grained test class below delegates to one helper; the 16
// structural/semantic aggregate classes reuse the same helpers, so every
// register_*_tests() call has exactly one definition site.  Helpers are
// behavior-identical to the lambdas they replace.  The obsolete split
// classes `all` / `all-1` / `all-2` are removed; the scripted full-suite
// runner (`make test-full` -> scripts/run_all_classes.sh) replaces them.

static void run_testrunner_group() {
    register_testrunner_tests();
    register_freelist_consistency_tests();
    register_infra_tests();
    register_external_test_classes();
    register_expected_panic_tests();
}
static void run_basic_lib_group() { register_lib_tests(); }
static void run_basic_atomic_group() { register_atomic_tests(); }
static void run_configuration_build_group() {
    register_buildsystem_tests();
    register_config_checks_tests();
}
static void run_data_structures_spsc_group() { register_spsc_tests(); }
static void run_data_structures_buffer_pool_group() {
    register_buffer_pool_tests();
}
static void run_data_structures_buffer_pool_deterministic_group() {
    register_buffer_pool_deterministic_tests();
}
static void run_synchronization_spinlock_group() {
    register_spinlock_tests();
    register_spinlock_stress_tests();
}
static void run_synchronization_sync_group() {
    register_sync_tests();
    register_sync_block_pattern_tests();
}
static void run_synchronization_locking_group() {
    register_locking_tests();
    register_locking_stress_tests();
}
static void run_synchronization_lock_order_group() {
    register_lock_order_tests();
}
static void run_synchronization_lock_validator_group() {
    register_lock_validator_tests();
}
static void run_synchronization_irq_guard_group() {
    register_irq_guard_tests();
    register_irqguard_audit_tests();
}
static void run_synchronization_pip_group() {
    register_pip_tests();
    register_queue_pip_tests();
}
static void run_synchronization_pcp_group() {
    register_pcp_tests();
    register_mutex_pcp_tests();
}
static void run_synchronization_pi_donation_group() {
    register_priority_inheritance_tests();
}
static void run_synchronization_err_api_group() {
    register_sync_err_api_tests();
}
static void run_scheduler_core_group() { register_scheduler_tests(); }
static void run_scheduler_o1_group() { register_o1_scheduler_tests(); }
static void run_scheduler_atomic_group() {
    register_atomic_context_switch_tests();
}
static void run_scheduler_sporadic_group() {
    register_sporadic_server_tests();
}
static void run_scheduler_idle_group() {
    register_idle_task_tests();
    register_idle_cleanup_tests();
}
static void run_scheduler_zombie_group() {
    register_zombie_cleanup_tests();
    register_wcet_cleanup_tests();
}
static void run_scheduler_preemption_group() {
    register_preemption_tests();
    register_preemption_under_syscall_tests();
}
static void run_scheduler_budget_group() { register_budget_tests(); }
static void run_scheduler_cpu_load_group() { register_cpu_load_tests(); }
static void run_scheduler_starvation_group() {
    register_starvation_deadlock_tests();
}
static void run_scheduler_hrt_group() { register_scheduler_hrt_tests(); }
static void run_task_core_group() { register_task_tests(); }
static void run_task_lifecycle_group() { register_task_lifecycle_tests(); }
static void run_task_init_group() { register_init_tests(); }
static void run_initrd_parser_group() { register_initrd_parser_tests(); }
static void run_fpu_invariants_group() { register_fpu_inv_tests(); }
static void run_syscall_core_group() { register_syscall_tests(); }
static void run_syscall_fuzz_group() { register_syscall_fuzz_tests(); }
static void run_syscall_fastpath_group() {
    register_syscall_fastpath_tests();
}
static void run_syscall_affinity_group() {
    register_syscall_affinity_tests();
}
static void run_tls_group() { register_tls_tests(); }
static void run_process_lifecycle_group() { register_process_tests(); }
static void run_process_elf_group() { register_elf_tests(); }
static void run_elf_loader_group() { register_elf_loader_tests(); }
static void run_libc_verify_group() { register_libc_verify_tests(); }
static void run_elf_shared_group() { register_elf_shared_tests(); }
static void run_process_signals_group() { register_signals_tests(); }
static void run_process_rlimit_group() { register_rlimit_tests(); }
static void run_process_waitpid_group() { register_waitpid_tests(); }
static void run_process_pml4_clone_group() { register_pml4_clone_tests(); }
static void run_pt_merge_group() { register_pt_merge_tests(); }
static void run_process_secure_exec_group() { register_secure_exec_tests(); }
static void run_cap_core_group() { register_cap_core_tests(); }
static void run_cap_lifecycle_group() { register_cap_lifecycle_tests(); }
static void run_cap_syscall_group() { register_cap_syscall_tests(); }
static void run_cap_ipc_group() { register_cap_ipc_tests(); }
static void run_cap_untyped_group() { register_cap_untyped_tests(); }
static void run_cap_mmio_group() { register_cap_mmio_tests(); }
static void run_cap_mmio_user_group() { register_cap_mmio_user_tests(); }
static void run_cap_irq_group() { register_cap_irq_tests(); }
static void run_cap_irq_notify_group() { register_cap_irq_notify_tests(); }
#if defined(CONFIG_ARCH_X86_64)
static void run_cap_msix_group() { register_cap_msix_tests(); }
#endif
static void run_cap_iommu_group() { register_cap_iommu_tests(); }
static void run_cap_shm_group() { register_cap_shm_tests(); }
static void run_cap_death_group() { register_cap_death_tests(); }
static void run_cap_pager_group() { register_cap_pager_tests(); }
static void run_ipc_fastpath_group() { register_ipc_fastpath_tests(); }
static void run_ipc_pipe_blocking_group() {
    register_pipe_blocking_tests();
}
static void run_ipc_core_group() { register_ipc_tests(); }
static void run_ipc_blocking_group() { register_ipc_blocking_tests(); }
static void run_ipc_timeout_group() { register_ipc_timeout_tests(); }
static void run_ipc_extended_group() { register_ipc_extended_tests(); }
static void run_ipc_lock_free_group() { register_ipc_lock_free_tests(); }
static void run_ipc_robustness_group() { register_ipc_robustness_tests(); }
static void run_ipc_pipe_group() { register_pipe_tests(); }
static void run_vfs_core_group() { register_vfs_tests(); }
static void run_vfs_tmpfs_group() {
    register_tmpfs_tests();
    register_tmpfs_invalid_mount_tests();
    register_tmpfs_mount_unmount_failure_tests();
}
static void run_vfs_fstab_group() { register_fstab_tests(); }
static void run_vfs_fat32_group() { register_fat32_tests(); }
static void run_vfs_fat32_integration_group() {
    register_vfs_fat32_tests();
}
static void run_vfs_procfs_group() { register_vfs_procfs_tests(); }
static void run_vfs_tmpfs_corrupt_group() {
    register_vfs_tmpfs_corrupt_tests();
}
static void run_vfs_devfs_group() { register_devfs_tests(); }
static void run_vfs_procfs_ops_group() { register_procfs_ops_tests(); }
static void run_vfs_initrd_fs_group() { register_initrd_fs_tests(); }
static void run_vfs_errors_group() { register_vfs_errors_tests(); }
static void run_servers_vfsd_group() { register_vfsd_tests(); }
static void run_servers_vfsd_auth_group() {
    register_vfsd_authorization_tests();
}
static void run_servers_iocd_group() { register_iocd_tests(); }
static void run_servers_daemon_restart_group() {
    register_daemon_restart_crash_tests();
}
static void run_servers_health_group() { register_health_tests(); }
static void run_services_framework_group() {
    register_services_framework_tests();
}
static void run_memory_pmm_group() {
    register_memory_tests();
    register_pmm_tests();
}
static void run_memory_mempool_group() { register_mempool_tests(); }
static void run_memory_slab_group() { register_slab_reclaim_tests(); }
static void run_memory_safety_group() { register_memory_safety_tests(); }
static void run_memory_determinism_group() {
    register_memory_determinism_tests();
    register_no_dynamic_alloc_tests();
}
static void run_memory_checked_ptr_group() { register_checked_ptr_tests(); }
static void run_kernel_top_group() { register_kernel_top_tests(); }
static void run_per_cpu_group() { register_per_cpu_tests(); }
static void run_memory_checked_ptr_api_group() {
    register_checked_ptr_api_tests();
}
static void run_memory_integrity_group() {
    register_memory_integrity_tests();
}
static void run_memory_resource_exhaustion_group() {
    register_resource_exhaustion_tests();
}
static void run_memory_stack_alloc_group() { register_stack_alloc_tests(); }
static void run_memory_stack_profiler_group() {
    register_stack_profiler_tests();
}
static void run_memory_static_pools_group() { register_static_pools_tests(); }
static void run_memory_no_op_new_group() { register_no_op_new_tests(); }
static void run_memory_page_tables_group() { register_page_tables_tests(); }
static void run_memory_kernel_isolation_group() {
    register_kernel_isolation_tests();
}
static void run_memory_isolation_group() {
    register_memory_isolation_tests();
}
static void run_memory_vmm_group() { register_vmm_tests(); }
static void run_wcet_overrun_group() { register_wcet_overrun_tests(); }
static void run_wcet_scheduler_group() { register_wcet_scheduler_tests(); }
static void run_bench_wcet_memory_group() { register_wcet_memory_tests(); }
static void run_deadline_miss_group() { register_deadline_miss_tests(); }
static void run_deadline_recovery_group() {
    register_deadline_recovery_tests();
}
static void run_deadline_action_group() { register_deadline_action_tests(); }
static void run_deadline_ss_group() { register_ss_deadline_tests(); }
static void run_timing_core_group() { register_timing_tests(); }
static void run_hal_core_group() { register_hal_tests(); }
static void run_hal_bits_group() { register_hal_bits_tests(); }
static void run_hal_idt_group() { register_idt_tests(); }
static void run_exc_table_group() { register_exc_table_tests(); }
static void run_hal_timer_group() { register_timer_tests(); }
static void run_hal_apic_group() { register_apic_timer_tests(); }
static void run_apic_tpr_group() { register_apic_tpr_tests(); }
static void run_hal_rtc_group() { register_rtc_tests(); }
#if defined(CONFIG_ARCH_X86_64)
static void run_hal_rtc_datetime_group() { register_rtc_datetime_tests(); }
static void run_hal_keyboard_decode_group() {
    register_keyboard_decode_tests();
}
static void run_hal_gdt_layout_group() { register_gdt_layout_tests(); }
static void run_hal_serial_logic_group() { register_serial_logic_tests(); }
static void run_acpi_parse_group() { register_acpi_parse_tests(); }
static void run_smp_madt_group() { register_smp_madt_tests(); }
static void run_smp_ipi_group() { register_smp_ipi_tests(); }
static void run_smp_bringup_group() { register_smp_bringup_tests(); }
static void run_sched_affinity_group() { register_sched_affinity_tests(); }
static void run_smp_sched_group() { register_smp_sched_tests(); }
static void run_lapic_group() { register_lapic_tests(); }
static void run_ioapic_group() { register_ioapic_tests(); }
static void run_core_isolation_group() { register_core_isolation_tests(); }
static void run_load_balancer_group() { register_load_balancer_tests(); }
static void run_cache_coloring_group() { register_cache_coloring_tests(); }
static void run_smp_sync_group() { register_smp_sync_tests(); }
static void run_smp_verify_group() { register_smp_verify_tests(); }
static void run_pcid_group() { register_pcid_tests(); }
static void run_invpcid_group() { register_invpcid_tests(); }
static void run_lazy_tlb_group() { register_lazy_tlb_tests(); }
static void run_ipi_batching_group() { register_ipi_batching_tests(); }
static void run_tlb_latency_group() { register_tlb_latency_tests(); }
static void run_pml4_sync_group() { register_pml4_sync_tests(); }
#endif
static void run_drivers_core_group() { register_driver_tests(); }
static void run_drivers_block_group() { register_block_device_tests(); }
static void run_drivers_pci_group() { register_pci_tests(); }
static void run_drivers_virtio_group() { register_virtio_tests(); }
static void run_drivers_dma_group() { register_dma_tests(); }
static void run_drivers_virtio_blk_req_group() {
    register_virtio_blk_req_tests();
}
#if defined(CONFIG_ARCH_X86_64)
static void run_drivers_ahci_deep_group() { register_ahci_deep_tests(); }
#endif
static void run_network_core_group() { register_net_tests(); }
static void run_shell_interaction_group() {
    register_shell_interaction_tests();
}
static void run_shell_redirect_group() { register_shell_redirect_tests(); }
static void run_shell_textutils_group() { register_textutils_tests(); }
static void run_debug_dump_group() { register_debug_dump_tests(); }
static void run_profiler_sampler_group() {
    register_profiler_sampler_tests();
}
static void run_shell_commands_group() { register_shell_commands_tests(); }
static void run_ui_framebuffer_group() { register_framebuffer_tests(); }
static void run_random_core_group() { register_random_tests(); }
static void run_random_seed_group() { register_random_seed_tests(); }
static void run_random_syscall_group() { register_random_syscall_tests(); }
static void run_random_vfs_group() { register_random_vfs_tests(); }
static void run_random_vfs_write_group() {
    register_random_vfs_write_tests();
}
static void run_logging_dmesg_group() { register_dmesg_tests(); }
static void run_logging_klog_group() { register_klog_tests(); }
static void run_debug_core_group() { register_debug_tests(); }
static void run_debug_gcov_group() { register_gcov_tests(); }
static void run_arch_cross_group() { register_cross_arch_tests(); }
static void run_bench_ipc_group() { register_ipc_benchmark_tests(); }
static void run_bench_syscall_group() {
    register_bench_syscall_latency_tests();
}
static void run_bench_irq_group() { register_bench_irq_latency_tests(); }
static void run_bench_jitter_group() { register_jitter_tests(); }
static void run_bench_microkernel_group() {
    register_microkernel_transition_tests();
}
static void run_stress_hrt_group() { register_stress_hrt_tests(); }
static void run_hrt_monotonic_group() { register_hrt_monotonic_tests(); }
static void run_timer_wheel_group() { register_timer_wheel_tests(); }
static void run_task_metering_group() { register_task_metering_tests(); }
static void run_sched_edf_group() { register_sched_edf_tests(); }
static void run_sched_admission_group() {
    register_sched_admission_tests();
}
static void run_sched_admission_verify_group() {
    register_sched_admission_verify_tests();
}
static void run_aperiodic_servers_group() {
    register_aperiodic_servers_tests();
}

// ---- Test class table ----
// Each class maps to a helper above (one definition site per
// register_*_tests() call).  The "safe" class is the curated TF_RELEASE
// subset for release builds and `selftest` with no args.  The 16
// structural/semantic aggregates (issue #173) replace the removed `all`
// class; the scripted full-suite runner (`make test-full`) runs them.

static constexpr kernel::test::TestClass g_test_classes[] = {
    // -- testrunner: harness + freelist + infra integrity, expected-panic LAST
    //    (the panic test halts the kernel, so everything else must run first).
    {"testrunner", []() { run_testrunner_group(); }},

    // -- basic: low-level primitives (strings, utils, ErrorOr, atomics) --
    {"basic_lib", []() { run_basic_lib_group(); }},
    {"basic_atomic", []() { run_basic_atomic_group(); }},

    // -- configuration: build-system + compile-time config sanity --
    {"configuration_build", []() { run_configuration_build_group(); }},

    // -- data_structures: container primitives consumed by other subsystems --
    {"data_structures_spsc", []() { run_data_structures_spsc_group(); }},
    {"data_structures_buffer_pool",
     []() { run_data_structures_buffer_pool_group(); }},
    {"data_structures_buffer_pool_deterministic",
     []() { run_data_structures_buffer_pool_deterministic_group(); }},

    // -- synchronization: locking primitives and protocols --
    {"synchronization_spinlock",
     []() { run_synchronization_spinlock_group(); }},
    {"synchronization_sync",
     []() { run_synchronization_sync_group(); }},
    {"synchronization_locking",
     []() { run_synchronization_locking_group(); }},
    {"synchronization_lock_order",
     []() { run_synchronization_lock_order_group(); }},
    {"synchronization_lock_validator",
     []() { run_synchronization_lock_validator_group(); }},
    {"synchronization_irq_guard",
     []() { run_synchronization_irq_guard_group(); }},
    {"synchronization_pip", []() { run_synchronization_pip_group(); }},
    {"synchronization_pcp", []() { run_synchronization_pcp_group(); }},
    {"synchronization_pi_donation",
     []() { run_synchronization_pi_donation_group(); }},

    // -- scheduler: policy, queueing, preemption, budgets --
    {"scheduler_core", []() { run_scheduler_core_group(); }},
    {"scheduler_o1", []() { run_scheduler_o1_group(); }},
    {"scheduler_atomic", []() { run_scheduler_atomic_group(); }},
    {"scheduler_sporadic", []() { run_scheduler_sporadic_group(); }},
    {"scheduler_idle", []() { run_scheduler_idle_group(); }},
    {"scheduler_zombie", []() { run_scheduler_zombie_group(); }},
    {"scheduler_preemption", []() { run_scheduler_preemption_group(); }},
    {"scheduler_budget", []() { run_scheduler_budget_group(); }},
    {"scheduler_cpu_load", []() { run_scheduler_cpu_load_group(); }},
    {"scheduler_starvation", []() { run_scheduler_starvation_group(); }},

    // -- scheduler_hrt: hard real-time time assertions on real dispatch
    //    (issue #102) — same functional scenarios as scheduler_core WITH
    //    relative-bound time measurement (wake latency, preemption, jitter).
    {"scheduler_hrt", []() { run_scheduler_hrt_group(); }},

    // -- task: TCB, lifecycle, FPU, init --
    {"task_core", []() { run_task_core_group(); }},
    {"task_lifecycle", []() { run_task_lifecycle_group(); }},
    {"task_fpu",
     []() {
         register_fpu_tests();
         register_fpu_sse_tests();
         register_fpu_clone_tests();
         register_fpu_multi_tests();
         register_fpu_xmm_all_tests();
     }},
    {"task_init", []() { run_task_init_group(); }},

    // -- initrd_parser: cpio-newc archive parser (issue #112) --
    {"initrd_parser", []() { run_initrd_parser_group(); }},
    {"task_tcb_log", []() { register_tcb_write_log_tests(); }},

    // -- fpu_invariants: FPU/SIMD context invariants (issue #93) --
    // The legacy test_fpu*.cpp files are filtered out of the x86_64 build
    // (GCC 16 / -mgeneral-regs-only); this class carries the INV-FPU tests.
    {"fpu_invariants", []() { run_fpu_invariants_group(); }},

    // -- syscall: dispatch interface and fuzzing --
    {"syscall_core", []() { run_syscall_core_group(); }},
    {"syscall_fuzz", []() { run_syscall_fuzz_group(); }},

    // -- syscall_fastpath: tiered FAST/FULL dispatch (issue #92) --
    {"syscall_fastpath", []() { run_syscall_fastpath_group(); }},

    // -- syscall_affinity: SET/GET affinity round-trips (issue #61) --
    // Previously registered but called by no class (not even `all`);
    // wired into the `core` aggregate (issue #173).
    {"syscall_affinity", []() { run_syscall_affinity_group(); }},

    // -- tls: TLS_SET + TLS-on-switch publish/apply (issue #74) --
    {"tls", []() { run_tls_group(); }},

    // -- process: lifecycle, exec, signals, limits --
    {"process_lifecycle", []() { run_process_lifecycle_group(); }},
    {"process_elf", []() { run_process_elf_group(); }},
    {"elf_loader", []() { run_elf_loader_group(); }},
    {"libc_verify", []() { run_libc_verify_group(); }},
    {"elf_shared", []() { run_elf_shared_group(); }},
    {"process_signals", []() { run_process_signals_group(); }},
    {"process_rlimit", []() { run_process_rlimit_group(); }},
    {"process_waitpid", []() { run_process_waitpid_group(); }},
    {"process_pml4_clone", []() { run_process_pml4_clone_group(); }},
    {"pt_merge", []() { run_pt_merge_group(); }},
    {"process_secure_exec", []() { run_process_secure_exec_group(); }},

    // -- cap: capability-based access control (CSpace core engine) --
    {"cap_core", []() { run_cap_core_group(); }},
    {"cap_lifecycle", []() { run_cap_lifecycle_group(); }},
    {"cap_syscall", []() { run_cap_syscall_group(); }},
    {"cap_ipc", []() { run_cap_ipc_group(); }},
    {"cap_untyped", []() { run_cap_untyped_group(); }},
    {"cap_mmio", []() { run_cap_mmio_group(); }},
    {"cap_mmio_user", []() { run_cap_mmio_user_group(); }},
    {"cap_irq", []() { run_cap_irq_group(); }},
    {"cap_irq_notify", []() { run_cap_irq_notify_group(); }},
#if defined(CONFIG_ARCH_X86_64)
    {"cap_msix", []() { run_cap_msix_group(); }},
#endif
    {"cap_iommu", []() { run_cap_iommu_group(); }},
    // Live VT-d enablement (issue #9).  NOT part of any aggregate —
    // requires the q35+intel-iommu QEMU variant (`make execute-test
    // x86_64 debug iommu_live`); on the default pc build there is no
    // DMAR unit.  The scripted full run (`make test-full`) covers it.
    {"iommu_live", []() { register_iommu_live_tests(); }},

    // -- cap_shm: capability-gated shared-memory rings (issue #106 Part B) --
    {"cap_shm", []() { run_cap_shm_group(); }},

    // -- cap_death: asynchronous task-death notifications (issue #105 Part B) --
    {"cap_death", []() { run_cap_death_group(); }},

    // -- cap_pager: external pager protocol (issue #107) --
    {"cap_pager", []() { run_cap_pager_group(); }},

    // -- ipc_fastpath: in-register IPC fastpath (issue #11) --
    {"ipc_fastpath", []() { run_ipc_fastpath_group(); }},

    // -- ipc_pipe_blocking: pipe blocking semantics (issue #111) --
    {"ipc_pipe_blocking", []() { run_ipc_pipe_blocking_group(); }},

    // -- ipc: messages, events, notifications, pipes --
    {"ipc_core", []() { run_ipc_core_group(); }},
    {"ipc_blocking", []() { run_ipc_blocking_group(); }},
    {"ipc_timeout", []() { run_ipc_timeout_group(); }},
    {"ipc_extended", []() { run_ipc_extended_group(); }},
    {"ipc_lock_free", []() { run_ipc_lock_free_group(); }},
    {"ipc_robustness", []() { run_ipc_robustness_group(); }},
    {"ipc_pipe", []() { run_ipc_pipe_group(); }},

    // -- vfs: filesystem core and backends --
    {"vfs_core", []() { run_vfs_core_group(); }},
    {"vfs_tmpfs", []() { run_vfs_tmpfs_group(); }},
    {"vfs_fstab", []() { run_vfs_fstab_group(); }},
    {"vfs_fat32", []() { run_vfs_fat32_group(); }},
    {"vfs_fat32_integration",
     []() { run_vfs_fat32_integration_group(); }},

    // -- vfs_procfs: /proc filesystem nodes (issue #109) --
    {"vfs_procfs", []() { run_vfs_procfs_group(); }},

    // -- vfs_tmpfs_corrupt: tmpfs corruption/timeout analogue (issue #114) --
    {"vfs_tmpfs_corrupt", []() { run_vfs_tmpfs_corrupt_group(); }},

    // -- vfs_devfs: /dev device-node operations (issue #124) --
    {"vfs_devfs", []() { run_vfs_devfs_group(); }},

    // -- vfs_procfs_ops: procfs per-node op contracts (issue #124) --
    {"vfs_procfs_ops", []() { run_vfs_procfs_ops_group(); }},

    // -- vfs_initrd_fs: initrd filesystem vnodes (issue #124) --
    {"vfs_initrd_fs", []() { run_vfs_initrd_fs_group(); }},

    // -- vfs_errors: VfsError *_err API + error-code mapping (issue #124) --
    {"vfs_errors", []() { run_vfs_errors_group(); }},

    // -- servers: user-space daemons --
    {"servers_vfsd", []() { run_servers_vfsd_group(); }},
    {"servers_vfsd_auth", []() { run_servers_vfsd_auth_group(); }},
    {"servers_iocd", []() { run_servers_iocd_group(); }},
    {"servers_daemon_restart",
     []() { run_servers_daemon_restart_group(); }},
    {"servers_health", []() { register_health_tests(); }},

    // -- memory: allocators, safety, layout, VMM --
    // register_memory_tests() is a documented 0-test delegate (sub-classes
    // register themselves); kept for all-class consistency.
    {"memory_pmm", []() { run_memory_pmm_group(); }},
    {"memory_mempool", []() { run_memory_mempool_group(); }},
    {"memory_slab", []() { run_memory_slab_group(); }},
    {"memory_safety", []() { run_memory_safety_group(); }},
    {"memory_determinism", []() { run_memory_determinism_group(); }},
    {"memory_checked_ptr", []() { run_memory_checked_ptr_group(); }},
    {"kernel_top", []() { run_kernel_top_group(); }},
    {"per_cpu", []() { run_per_cpu_group(); }},
    {"synchronization_err_api",
     []() { run_synchronization_err_api_group(); }},
    {"memory_checked_ptr_api",
     []() { run_memory_checked_ptr_api_group(); }},
    {"memory_integrity", []() { run_memory_integrity_group(); }},
    {"memory_resource_exhaustion",
     []() { run_memory_resource_exhaustion_group(); }},
    {"memory_stack_alloc", []() { run_memory_stack_alloc_group(); }},
    {"memory_stack_profiler",
     []() { run_memory_stack_profiler_group(); }},
    {"memory_static_pools", []() { run_memory_static_pools_group(); }},
    {"memory_no_op_new", []() { run_memory_no_op_new_group(); }},
    {"memory_page_tables", []() { run_memory_page_tables_group(); }},
    {"memory_kernel_isolation",
     []() { run_memory_kernel_isolation_group(); }},
    {"memory_isolation", []() { run_memory_isolation_group(); }},
    {"memory_vmm", []() { run_memory_vmm_group(); }},

    // -- wcet / deadline: worst-case execution time and deadline handling --
    {"wcet_overrun", []() { run_wcet_overrun_group(); }},
    {"wcet_scheduler", []() { run_wcet_scheduler_group(); }},
    // bench_wcet_memory: TF_BENCH-only (runner is_bench heuristic needs the
    // class name to start with "be"; a "wcet_memory" name would filter them).
    {"bench_wcet_memory", []() { run_bench_wcet_memory_group(); }},
    {"deadline_miss", []() { run_deadline_miss_group(); }},
    {"deadline_recovery", []() { run_deadline_recovery_group(); }},
    {"deadline_action", []() { run_deadline_action_group(); }},
    {"deadline_ss", []() { run_deadline_ss_group(); }},

    // -- timing: tick accounting, alarms, rate-monotonic, deadline lists --
    {"timing_core", []() { run_timing_core_group(); }},

    // -- hal: hardware abstraction layer --
    {"hal_core", []() { run_hal_core_group(); }},
    {"hal_bits", []() { run_hal_bits_group(); }},
    {"hal_idt", []() { run_hal_idt_group(); }},
    {"exc_table", []() { run_exc_table_group(); }},
    {"hal_timer", []() { run_hal_timer_group(); }},
    {"hal_apic", []() { run_hal_apic_group(); }},
    {"apic_tpr", []() { run_apic_tpr_group(); }},
    {"hal_rtc", []() { run_hal_rtc_group(); }},
#if defined(CONFIG_ARCH_X86_64)
    {"hal_rtc_datetime", []() { run_hal_rtc_datetime_group(); }},
    {"hal_keyboard_decode", []() { run_hal_keyboard_decode_group(); }},
    {"hal_gdt_layout", []() { run_hal_gdt_layout_group(); }},
    {"hal_serial_logic", []() { run_hal_serial_logic_group(); }},
    {"acpi_parse", []() { run_acpi_parse_group(); }},
    {"smp_madt", []() { run_smp_madt_group(); }},
    {"smp_ipi", []() { run_smp_ipi_group(); }},
    {"smp_bringup", []() { run_smp_bringup_group(); }},
    {"sched_affinity", []() { run_sched_affinity_group(); }},
    {"smp_sched", []() { run_smp_sched_group(); }},
    {"lapic", []() { run_lapic_group(); }},
    {"ioapic", []() { run_ioapic_group(); }},
    {"core_isolation", []() { run_core_isolation_group(); }},
    {"load_balancer", []() { run_load_balancer_group(); }},
    {"cache_coloring", []() { run_cache_coloring_group(); }},
    {"smp_sync", []() { run_smp_sync_group(); }},
    {"smp_verify", []() { run_smp_verify_group(); }},
    {"pcid", []() { run_pcid_group(); }},
    {"invpcid", []() { run_invpcid_group(); }},
    {"lazy_tlb", []() { run_lazy_tlb_group(); }},
    {"ipi_batching", []() { run_ipi_batching_group(); }},
    {"tlb_latency", []() { run_tlb_latency_group(); }},
    {"pml4_sync", []() { run_pml4_sync_group(); }},
#endif

    // -- drivers: device driver framework and controllers --
    {"drivers_core", []() { run_drivers_core_group(); }},
    {"drivers_block", []() { run_drivers_block_group(); }},
    {"drivers_pci", []() { run_drivers_pci_group(); }},
    {"drivers_virtio", []() { run_drivers_virtio_group(); }},
    {"drivers_dma", []() { run_drivers_dma_group(); }},

    // -- drivers_virtio_blk_req: virtio-blk request path (issue #117) --
    {"drivers_virtio_blk_req",
     []() { run_drivers_virtio_blk_req_group(); }},

#if defined(CONFIG_ARCH_X86_64)
    // -- drivers_ahci_deep: AHCI protocol contracts (issue #108) --
    {"drivers_ahci_deep", []() { run_drivers_ahci_deep_group(); }},

    // -- ahci_live: real AHCI command path on the q35+ICH9 variant --
    //    NOT part of any aggregate — requires the q35+OVMF+ide-hd QEMU
    //    variant (`make execute-test x86_64 debug ahci_live`); on the
    //    default pc machine there is no AHCI controller.  The scripted
    //    full run (`make test-full`) covers it.
    {"ahci_live", []() { register_ahci_live_tests(); }},
#endif

    // -- network: pure protocol primitives --
    {"network_core", []() { run_network_core_group(); }},

    // -- shell / user-interface --
    {"shell_interaction", []() { run_shell_interaction_group(); }},
    {"shell_redirect", []() { run_shell_redirect_group(); }},
    {"shell_textutils", []() { run_shell_textutils_group(); }},
    {"debug_dump", []() { run_debug_dump_group(); }},
    {"profiler_sampler", []() { run_profiler_sampler_group(); }},
    {"shell_commands", []() { run_shell_commands_group(); }},
    {"services_framework", []() { run_services_framework_group(); }},
    {"ui_framebuffer", []() { run_ui_framebuffer_group(); }},

    // -- random: RNG subsystem --
    {"random_core", []() { run_random_core_group(); }},
    {"random_seed", []() { run_random_seed_group(); }},
    {"random_syscall", []() { run_random_syscall_group(); }},
    {"random_vfs", []() { run_random_vfs_group(); }},
    {"random_vfs_write", []() { run_random_vfs_write_group(); }},

    // -- logging / debug --
    {"logging_dmesg", []() { run_logging_dmesg_group(); }},
    {"logging_klog", []() { run_logging_klog_group(); }},
    {"debug_core", []() { run_debug_core_group(); }},
    {"debug_gcov", []() { run_debug_gcov_group(); }},

    // -- arch: architecture-specific and cross-arch --
    {"arch_cross", []() { run_arch_cross_group(); }},

#if defined(CONFIG_ARCH_AARCH64)
    {"arch_aarch64", []() { register_aarch64_tests(); }},
#endif

#if defined(CONFIG_ARCH_RISCV64)
    {"arch_riscv64", []() { register_riscv64_tests(); }},
#endif

    // -- bench: timing-sensitive benchmarks, run last --
    {"bench_ipc", []() { run_bench_ipc_group(); }},
    {"bench_syscall", []() { run_bench_syscall_group(); }},
    {"bench_irq", []() { run_bench_irq_group(); }},
    {"bench_jitter", []() { run_bench_jitter_group(); }},
    {"bench_microkernel", []() { run_bench_microkernel_group(); }},

    // -- hrt: hard real-time measurement under QEMU -icount (issue #101) --
    // Bounds are RELATIVE (stress vs measured baseline), so the class also
    // runs cleanly inside aggregates without -icount.  Class-scoped icount
    // in the Makefile gives it hard deterministic timing when run standalone.
    {"stress_hrt", []() { run_stress_hrt_group(); }},
    {"hrt_monotonic", []() { run_hrt_monotonic_group(); }},
    {"timer_wheel", []() { run_timer_wheel_group(); }},
    {"task_metering", []() { run_task_metering_group(); }},
    {"sched_edf", []() { run_sched_edf_group(); }},
    {"sched_admission", []() { run_sched_admission_group(); }},
    {"sched_admission_verify",
     []() { run_sched_admission_verify_group(); }},
    {"aperiodic_servers", []() { run_aperiodic_servers_group(); }},

    // -- safe: curated subset with TF_RELEASE tests --
    {"safe",
     []() {
         register_lib_tests();
         register_checked_ptr_tests();
         register_block_device_tests();
         register_fat32_tests();
         register_vfs_fat32_tests();
         register_waitpid_tests();
         register_shell_interaction_tests();
         register_hal_bits_tests();
         register_o1_scheduler_tests();
     }},

    // ---- Structural/semantic aggregates (issue #173) ----
    // Each aggregate reuses the fine-class helpers above.  Together with
    // the standalone specials (ahci_live, iommu_live, task_fpu,
    // task_tcb_log) run by `make test-full`, they cover the full test
    // base previously run as `all`.
    {"core",
     []() {
         run_basic_lib_group();
         run_basic_atomic_group();
         run_configuration_build_group();
         run_data_structures_spsc_group();
         run_data_structures_buffer_pool_group();
         run_data_structures_buffer_pool_deterministic_group();
         run_synchronization_spinlock_group();
         run_synchronization_sync_group();
         run_synchronization_locking_group();
         run_synchronization_lock_order_group();
         run_synchronization_lock_validator_group();
         run_synchronization_irq_guard_group();
         run_synchronization_pip_group();
         run_synchronization_pcp_group();
         run_synchronization_pi_donation_group();
         run_synchronization_err_api_group();
         run_scheduler_core_group();
         run_scheduler_o1_group();
         run_scheduler_atomic_group();
         run_scheduler_idle_group();
         run_scheduler_zombie_group();
         run_scheduler_preemption_group();
         run_scheduler_budget_group();
         run_scheduler_cpu_load_group();
         run_scheduler_starvation_group();
         run_task_core_group();
         run_task_lifecycle_group();
         run_task_init_group();
         run_fpu_invariants_group();
         run_kernel_top_group();
         run_per_cpu_group();
         run_memory_pmm_group();
         run_memory_mempool_group();
         run_memory_slab_group();
         run_memory_safety_group();
         run_memory_determinism_group();
         run_memory_checked_ptr_group();
         run_memory_checked_ptr_api_group();
         run_memory_integrity_group();
         run_memory_resource_exhaustion_group();
         run_memory_stack_alloc_group();
         run_memory_stack_profiler_group();
         run_memory_static_pools_group();
         run_memory_no_op_new_group();
         run_memory_page_tables_group();
         run_memory_kernel_isolation_group();
         run_memory_isolation_group();
         run_memory_vmm_group();
         run_syscall_core_group();
         run_syscall_fuzz_group();
          run_syscall_fastpath_group();
          run_syscall_affinity_group();
          run_tls_group();
     }},
    {"ipc",
     []() {
         run_ipc_core_group();
         run_ipc_blocking_group();
         run_ipc_timeout_group();
         run_ipc_extended_group();
         run_ipc_lock_free_group();
         run_ipc_robustness_group();
         run_ipc_pipe_group();
         run_ipc_fastpath_group();
         run_ipc_pipe_blocking_group();
     }},
    {"capability",
     []() {
         run_cap_core_group();
         run_cap_lifecycle_group();
         run_cap_syscall_group();
         run_cap_ipc_group();
         run_cap_untyped_group();
         run_cap_mmio_group();
         run_cap_mmio_user_group();
         run_cap_irq_group();
         run_cap_irq_notify_group();
#if defined(CONFIG_ARCH_X86_64)
         run_cap_msix_group();
#endif
         run_cap_iommu_group();
         run_cap_shm_group();
         run_cap_death_group();
         run_cap_pager_group();
     }},
    // proc_elf: process lifecycle + ELF loading (issue #173).  Named
    // proc_elf (not process_elf) — the fine-grained file class
    // `process_elf` (test_elf.cpp) keeps that name; register_class()
    // resolves the FIRST table match, so the names must differ.
    {"proc_elf",
     []() {
         run_process_lifecycle_group();
         run_process_elf_group();
         run_elf_loader_group();
         run_elf_shared_group();
         run_process_signals_group();
         run_process_rlimit_group();
         run_process_waitpid_group();
          run_process_pml4_clone_group();
          run_pt_merge_group();
          run_process_secure_exec_group();
          run_libc_verify_group();
     }},
    {"storage",
     []() {
         run_vfs_core_group();
         run_vfs_tmpfs_group();
         run_vfs_fstab_group();
         run_vfs_fat32_group();
         run_vfs_fat32_integration_group();
         run_vfs_procfs_group();
         run_vfs_tmpfs_corrupt_group();
         run_vfs_devfs_group();
         run_vfs_procfs_ops_group();
         run_vfs_initrd_fs_group();
         run_vfs_errors_group();
         run_initrd_parser_group();
     }},
    {"servers",
     []() {
         run_servers_vfsd_group();
         run_servers_vfsd_auth_group();
         run_servers_iocd_group();
         run_servers_daemon_restart_group();
         run_servers_health_group();
         run_services_framework_group();
     }},
    {"drivers",
     []() {
         run_drivers_core_group();
         run_drivers_block_group();
         run_drivers_pci_group();
         run_drivers_virtio_group();
         run_drivers_dma_group();
         run_drivers_virtio_blk_req_group();
#if defined(CONFIG_ARCH_X86_64)
         run_drivers_ahci_deep_group();
#endif
         run_network_core_group();
     }},
    {"hal",
     []() {
         run_hal_core_group();
         run_hal_bits_group();
         run_hal_idt_group();
         run_exc_table_group();
         run_hal_timer_group();
         run_hal_apic_group();
         run_apic_tpr_group();
         run_hal_rtc_group();
#if defined(CONFIG_ARCH_X86_64)
         run_hal_rtc_datetime_group();
         run_hal_keyboard_decode_group();
         run_hal_gdt_layout_group();
         run_hal_serial_logic_group();
         run_acpi_parse_group();
#endif
         run_arch_cross_group();
     }},
#if defined(CONFIG_ARCH_X86_64)
    {"smp",
     []() {
         run_smp_madt_group();
         run_smp_ipi_group();
         run_sched_affinity_group();
         run_lapic_group();
         run_ioapic_group();
         run_core_isolation_group();
         run_load_balancer_group();
         run_cache_coloring_group();
         run_smp_sync_group();
         run_smp_verify_group();
         run_pcid_group();
         run_invpcid_group();
         run_lazy_tlb_group();
         run_ipi_batching_group();
         run_tlb_latency_group();
         run_pml4_sync_group();
         run_hal_apic_group();
         run_apic_tpr_group();
     }},
    // smp_multicpu: the SMP classes requiring a second CPU (issue #173).
    // Needs `-smp 2` (Makefile CLASS gate); every other aggregate keeps
    // the default single-CPU topology (variant-topology trap, issue #23).
    {"smp_multicpu",
     []() {
         run_smp_bringup_group();
         run_smp_sched_group();
     }},
#endif
    {"deadline",
     []() {
         run_wcet_overrun_group();
         run_wcet_scheduler_group();
         run_deadline_miss_group();
         run_deadline_recovery_group();
         run_deadline_action_group();
         run_deadline_ss_group();
         run_timing_core_group();
         run_timer_wheel_group();
         run_scheduler_sporadic_group();
         run_scheduler_hrt_group();
         run_sched_edf_group();
         run_sched_admission_group();
         run_sched_admission_verify_group();
         run_aperiodic_servers_group();
         run_task_metering_group();
         run_hrt_monotonic_group();
         run_stress_hrt_group();
     }},
    {"ui",
     []() {
         run_shell_interaction_group();
         run_shell_redirect_group();
         run_shell_textutils_group();
         run_shell_commands_group();
         run_ui_framebuffer_group();
         run_debug_dump_group();
         run_profiler_sampler_group();
     }},
    {"logging_debug",
     []() {
         run_logging_dmesg_group();
         run_logging_klog_group();
         run_debug_core_group();
         run_debug_gcov_group();
     }},
    {"random",
     []() {
         run_random_core_group();
         run_random_seed_group();
         run_random_syscall_group();
         run_random_vfs_group();
         run_random_vfs_write_group();
     }},
    // bench: TF_BENCH-only (runner is_bench heuristic needs the class
    // name to start with "be").  bench_wcet_memory lives here rather
    // than in `deadline` so its tests execute instead of being filtered.
    {"bench",
     []() {
         run_bench_ipc_group();
         run_bench_syscall_group();
         run_bench_irq_group();
         run_bench_jitter_group();
         run_bench_microkernel_group();
         run_bench_wcet_memory_group();
     }},
};

static constexpr size_t g_test_class_count =
    sizeof(g_test_classes) / sizeof(g_test_classes[0]);

// ---- register_class ----
// Looks up `name` in g_test_classes[], calls its register_fn, and records
// a class section boundary for output grouping.  Returns true on success.
bool kernel::test::register_class(const char *name) {
    kernel::gs::set_current_class(name);
    for (size_t i = 0; i < g_test_class_count; ++i) {
        if (strcmp(name, g_test_classes[i].name) == 0) {
            size_t before = Registry::count();
            g_test_classes[i].register_all();
            size_t after = Registry::count();
            if (after > before) {
                Registry::record_class_section(name, before, after - before);
            }
            size_t added = after - before;
            Logger::info("[TCOUNT] class=%s added=%u total=%u", name,
                         (unsigned)added, (unsigned)after);
            validate_class_count(name, after);
            return true;
        }
    }
    Logger::warn("Unknown test class: %s", name);
    return false;
}

void kernel::test::dump_class_counts() {
    Logger::raw_write("[TCOUNT] dumping all class counts\n");
    for (size_t i = 0; i < g_test_class_count; ++i) {
        Registry::clear();
        g_test_classes[i].register_all();
        size_t count = Registry::count();
        Logger::raw_write("[TCOUNT] ");
        Logger::raw_write(g_test_classes[i].name);
        Logger::raw_write(" = ");
        Logger::print_dec(count);
        Logger::raw_write("\n");
    }
    Registry::clear();
    validate_all_consistency();
}
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
