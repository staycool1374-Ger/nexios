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
void register_checked_ptr_tests();
void register_fstab_tests();
void register_rtc_tests();
void register_rlimit_tests();
void register_init_tests();
void register_syscall_tests();
void register_sync_tests();
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
void register_jitter_tests();
void register_idt_tests();
void register_pipe_tests();
void register_gcov_tests();
void register_debug_tests();
void register_framebuffer_tests();
void register_pml4_clone_tests();
void register_waitpid_tests();
void register_buffer_pool_tests();
void register_block_device_tests();
void register_fat32_tests();
void register_vfs_fat32_tests();
void register_ipc_blocking_tests();
void register_vfsd_authorization_tests();
void register_textutils_tests();
void register_shell_interaction_tests();
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
void register_checked_ptr_tests();
void register_resource_exhaustion_tests();
void register_sporadic_server_tests();
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
void register_slab_reclaim_tests();
void register_static_pools_tests();
void register_stack_profiler_tests();
void register_stack_alloc_tests();
void register_page_tables_tests();
void register_kernel_isolation_tests();
void register_memory_isolation_tests();
void register_buffer_pool_deterministic_tests();
void register_no_op_new_tests();
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

// Helper functions for split "all" / "all-1" / "all-2" classes.
// all-1 and all-2 work around the cumulative kernel page-table
// corruption that occurs after ~850 snapshot-restore cycles
// (resolved in v0.3.6, docs/specs/memory.md §3.1).
static void register_all_tests();
static void register_all_tests_first_half();
static void register_all_tests_second_half();

// ---- Test class table ----
// Each class maps to a lambda that calls the relevant register_*_tests()
// functions.  The "safe" class is the curated TF_RELEASE subset for release
// builds and `selftest` with no args.  The "all" class is everything incl.
// benchmarks.

static constexpr kernel::test::TestClass g_test_classes[] = {
    // -- testrunner: harness + freelist + infra integrity, expected-panic LAST
    //    (the panic test halts the kernel, so everything else must run first).
    {"testrunner", []() { register_testrunner_tests(); register_freelist_consistency_tests(); register_infra_tests(); register_expected_panic_tests(); }},

    // -- basic: low-level primitives (strings, utils, ErrorOr, atomics) --
    {"basic_lib", []() { register_lib_tests(); }},
    {"basic_atomic", []() { register_atomic_tests(); }},

    // -- configuration: build-system + compile-time config sanity --
    {"configuration_build", []() { register_buildsystem_tests(); register_config_checks_tests(); }},

    // -- data_structures: container primitives consumed by other subsystems --
    {"data_structures_spsc", []() { register_spsc_tests(); }},
    {"data_structures_buffer_pool", []() { register_buffer_pool_tests(); }},
    {"data_structures_buffer_pool_deterministic",
     []() { register_buffer_pool_deterministic_tests(); }},

    // -- synchronization: locking primitives and protocols --
    {"synchronization_spinlock",
     []() {
         register_spinlock_tests();
         register_spinlock_stress_tests();
     }},
    {"synchronization_sync", []() { register_sync_tests(); }},
    {"synchronization_locking",
     []() {
         register_locking_tests();
         register_locking_stress_tests();
     }},
    {"synchronization_lock_order", []() { register_lock_order_tests(); }},
    {"synchronization_lock_validator", []() { register_lock_validator_tests(); }},
    {"synchronization_irq_guard",
     []() {
         register_irq_guard_tests();
         register_irqguard_audit_tests();
     }},
    {"synchronization_pip",
     []() {
         register_pip_tests();
         register_queue_pip_tests();
     }},
    {"synchronization_pcp",
     []() {
         register_pcp_tests();
         register_mutex_pcp_tests();
     }},
    {"synchronization_pi_donation", []() { register_priority_inheritance_tests(); }},

    // -- scheduler: policy, queueing, preemption, budgets --
    {"scheduler_core", []() { register_scheduler_tests(); }},
    {"scheduler_o1", []() { register_o1_scheduler_tests(); }},
    {"scheduler_atomic", []() { register_atomic_context_switch_tests(); }},
    {"scheduler_sporadic", []() { register_sporadic_server_tests(); }},
    {"scheduler_idle",
     []() {
         register_idle_task_tests();
         register_idle_cleanup_tests();
     }},
    {"scheduler_zombie",
     []() {
         register_zombie_cleanup_tests();
         register_wcet_cleanup_tests();
     }},
    {"scheduler_preemption",
     []() {
         register_preemption_tests();
         register_preemption_under_syscall_tests();
     }},
    {"scheduler_budget", []() { register_budget_tests(); }},
    {"scheduler_cpu_load", []() { register_cpu_load_tests(); }},
    {"scheduler_starvation", []() { register_starvation_deadlock_tests(); }},

    // -- task: TCB, lifecycle, FPU, init --
    {"task_core", []() { register_task_tests(); }},
    {"task_lifecycle", []() { register_task_lifecycle_tests(); }},
    {"task_fpu",
     []() {
         register_fpu_tests();
         register_fpu_sse_tests();
         register_fpu_clone_tests();
         register_fpu_multi_tests();
         register_fpu_xmm_all_tests();
     }},
    {"task_init", []() { register_init_tests(); }},
    {"task_tcb_log", []() { register_tcb_write_log_tests(); }},

    // -- syscall: dispatch interface and fuzzing --
    {"syscall_core", []() { register_syscall_tests(); }},
    {"syscall_fuzz", []() { register_syscall_fuzz_tests(); }},

    // -- process: lifecycle, exec, signals, limits --
    {"process_lifecycle", []() { register_process_tests(); }},
    {"process_elf", []() { register_elf_tests(); }},
    {"elf_loader", []() { register_elf_loader_tests(); }},
    {"process_signals", []() { register_signals_tests(); }},
    {"process_rlimit", []() { register_rlimit_tests(); }},
    {"process_waitpid", []() { register_waitpid_tests(); }},
    {"process_pml4_clone", []() { register_pml4_clone_tests(); }},
    {"process_secure_exec", []() { register_secure_exec_tests(); }},

    // -- ipc: messages, events, notifications, pipes --
    {"ipc_core", []() { register_ipc_tests(); }},
    {"ipc_blocking", []() { register_ipc_blocking_tests(); }},
    {"ipc_extended", []() { register_ipc_extended_tests(); }},
    {"ipc_lock_free", []() { register_ipc_lock_free_tests(); }},
    {"ipc_robustness", []() { register_ipc_robustness_tests(); }},
    {"ipc_pipe", []() { register_pipe_tests(); }},

    // -- vfs: filesystem core and backends --
    {"vfs_core", []() { register_vfs_tests(); }},
    {"vfs_tmpfs",
     []() {
         register_tmpfs_tests();
         register_tmpfs_invalid_mount_tests();
         register_tmpfs_mount_unmount_failure_tests();
     }},
    {"vfs_fstab", []() { register_fstab_tests(); }},
    {"vfs_fat32", []() { register_fat32_tests(); }},
    {"vfs_fat32_integration", []() { register_vfs_fat32_tests(); }},

    // -- servers: user-space daemons --
    {"servers_vfsd", []() { register_vfsd_tests(); }},
    {"servers_vfsd_auth", []() { register_vfsd_authorization_tests(); }},
    {"servers_iocd", []() { register_iocd_tests(); }},
    {"servers_daemon_restart", []() { register_daemon_restart_crash_tests(); }},
    {"servers_health", []() { register_health_tests(); }},

    // -- memory: allocators, safety, layout, VMM --
    // register_memory_tests() is a documented 0-test delegate (sub-classes
    // register themselves); kept for all-class consistency.
    {"memory_pmm", []() { register_memory_tests(); register_pmm_tests(); }},
    {"memory_mempool", []() { register_mempool_tests(); }},
    {"memory_slab", []() { register_slab_reclaim_tests(); }},
    {"memory_safety", []() { register_memory_safety_tests(); }},
    {"memory_determinism",
     []() {
         register_memory_determinism_tests();
         register_no_dynamic_alloc_tests();
     }},
    {"memory_checked_ptr", []() { register_checked_ptr_tests(); }},
    {"memory_resource_exhaustion", []() { register_resource_exhaustion_tests(); }},
    {"memory_stack_alloc", []() { register_stack_alloc_tests(); }},
    {"memory_stack_profiler", []() { register_stack_profiler_tests(); }},
    {"memory_static_pools", []() { register_static_pools_tests(); }},
    {"memory_no_op_new", []() { register_no_op_new_tests(); }},
    {"memory_page_tables", []() { register_page_tables_tests(); }},
    {"memory_kernel_isolation", []() { register_kernel_isolation_tests(); }},
    {"memory_isolation", []() { register_memory_isolation_tests(); }},
    {"memory_vmm", []() { register_vmm_tests(); }},

    // -- wcet / deadline: worst-case execution time and deadline handling --
    {"wcet_overrun", []() { register_wcet_overrun_tests(); }},
    {"wcet_scheduler", []() { register_wcet_scheduler_tests(); }},
    // bench_wcet_memory: TF_BENCH-only (runner is_bench heuristic needs the
    // class name to start with "be"; a "wcet_memory" name would filter them).
    {"bench_wcet_memory", []() { register_wcet_memory_tests(); }},
    {"deadline_miss", []() { register_deadline_miss_tests(); }},
    {"deadline_recovery", []() { register_deadline_recovery_tests(); }},
    {"deadline_action", []() { register_deadline_action_tests(); }},
    {"deadline_ss", []() { register_ss_deadline_tests(); }},

    // -- timing: tick accounting, alarms, rate-monotonic, deadline lists --
    {"timing_core", []() { register_timing_tests(); }},

    // -- hal: hardware abstraction layer --
    {"hal_core", []() { register_hal_tests(); }},
    {"hal_bits", []() { register_hal_bits_tests(); }},
    {"hal_idt", []() { register_idt_tests(); }},
    {"hal_timer", []() { register_timer_tests(); }},
    {"hal_apic", []() { register_apic_timer_tests(); }},
    {"hal_rtc", []() { register_rtc_tests(); }},

    // -- drivers: device driver framework and controllers --
    {"drivers_core", []() { register_driver_tests(); }},
    {"drivers_block", []() { register_block_device_tests(); }},
    {"drivers_pci", []() { register_pci_tests(); }},
    {"drivers_virtio", []() { register_virtio_tests(); }},
    {"drivers_dma", []() { register_dma_tests(); }},

    // -- network: pure protocol primitives --
    {"network_core", []() { register_net_tests(); }},

    // -- shell / user-interface --
    {"shell_interaction", []() { register_shell_interaction_tests(); }},
    {"shell_redirect", []() { register_shell_redirect_tests(); }},
    {"shell_textutils", []() { register_textutils_tests(); }},
    {"ui_framebuffer", []() { register_framebuffer_tests(); }},

    // -- random: RNG subsystem --
    {"random_core", []() { register_random_tests(); }},
    {"random_seed", []() { register_random_seed_tests(); }},
    {"random_syscall", []() { register_random_syscall_tests(); }},
    {"random_vfs", []() { register_random_vfs_tests(); }},
    {"random_vfs_write", []() { register_random_vfs_write_tests(); }},

    // -- logging / debug --
    {"logging_dmesg", []() { register_dmesg_tests(); }},
    {"logging_klog", []() { register_klog_tests(); }},
    {"debug_core", []() { register_debug_tests(); }},
    {"debug_gcov", []() { register_gcov_tests(); }},

    // -- arch: architecture-specific and cross-arch --
    {"arch_cross", []() { register_cross_arch_tests(); }},

#if defined(CONFIG_ARCH_AARCH64)
    {"arch_aarch64", []() { register_aarch64_tests(); }},
#endif

#if defined(CONFIG_ARCH_RISCV64)
    {"arch_riscv64", []() { register_riscv64_tests(); }},
#endif

    // -- bench: timing-sensitive benchmarks, run last --
    {"bench_ipc", []() { register_ipc_benchmark_tests(); }},
    {"bench_syscall", []() { register_bench_syscall_latency_tests(); }},
    {"bench_irq", []() { register_bench_irq_latency_tests(); }},
    {"bench_jitter", []() { register_jitter_tests(); }},
    {"bench_microkernel", []() { register_microkernel_transition_tests(); }},

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

    // -- all: everything (debug mode) --
    //    PtPoolSnapshot fix + pool relocation prevents cumulative
    //    page-table corruption across snapshot cycles (v0.3.6).
    //    all-1/all-2 retained for parallel CI, but not needed for
    //    correctness.
    {"all", []() { register_all_tests(); }},
    {"all-1", []() { register_all_tests_first_half(); }},
    {"all-2", []() { register_all_tests_second_half(); }},
};

// ---- all / all-1 / all-2 function definitions ----
static void register_all_tests() {
    register_testrunner_tests();
    register_infra_tests();
    register_buffer_pool_tests();
    register_lib_tests();
    register_memory_tests();
    register_pmm_tests();
    register_mempool_tests();
    register_ipc_tests();
    register_ipc_extended_tests();
    register_ipc_robustness_tests();
    register_scheduler_tests();
    register_task_tests();
    register_driver_tests();
    register_vfs_tests();
    register_tmpfs_tests();
    register_tmpfs_invalid_mount_tests();
    register_tmpfs_mount_unmount_failure_tests();
    register_signals_tests();
    register_process_tests();
    register_elf_tests();
    register_elf_loader_tests();
    register_checked_ptr_tests();
    register_fstab_tests();
    register_rtc_tests();
    register_rlimit_tests();
    register_init_tests();
    register_syscall_tests();
    register_syscall_fuzz_tests();
    register_sync_tests();
    register_spinlock_tests();
    register_task_lifecycle_tests();
    register_idle_task_tests();
    register_zombie_cleanup_tests();
    register_wcet_cleanup_tests();
    register_idle_cleanup_tests();
    register_vfsd_tests();
    register_iocd_tests();
    register_health_tests();
    register_timer_tests();
    register_timing_tests();
    register_spsc_tests();
    register_idt_tests();
    register_pipe_tests();
     register_gcov_tests();
     register_debug_tests();
     register_framebuffer_tests();
#if CONFIG_VERSION_NUM >= 0x000309
     register_preemption_under_syscall_tests();
#endif
     register_spinlock_stress_tests();
    register_atomic_context_switch_tests();
    register_bench_syscall_latency_tests();
     register_bench_irq_latency_tests();
     register_starvation_deadlock_tests();

     // v0.3.4 tests
     register_apic_timer_tests();
     register_jitter_tests();
     register_deadline_miss_tests();
    register_wcet_overrun_tests();
    register_ss_deadline_tests();
    register_deadline_recovery_tests();
    register_deadline_action_tests();
    register_wcet_scheduler_tests();
    register_priority_inheritance_tests();
    register_queue_pip_tests();
    register_mutex_pcp_tests();
    register_pml4_clone_tests();
    register_waitpid_tests();
    register_resource_exhaustion_tests();
    register_block_device_tests();
    register_fat32_tests();
     register_vfs_fat32_tests();
#if CONFIG_VERSION_NUM >= 0x000309
     register_ipc_blocking_tests();
#endif
     register_ipc_lock_free_tests();
    register_vfsd_authorization_tests();
    register_textutils_tests();
    register_shell_interaction_tests();
    register_irq_guard_tests();
    register_irqguard_audit_tests();
    register_shell_redirect_tests();
    register_klog_tests();
    register_dmesg_tests();
    register_daemon_restart_crash_tests();
    register_hal_tests();
    register_buildsystem_tests();
    register_config_checks_tests();
    register_secure_exec_tests();
    register_pci_tests();
    register_virtio_tests();
    register_dma_tests();
    register_net_tests();
    register_ipc_benchmark_tests();
    register_microkernel_transition_tests();
    register_wcet_memory_tests();
    register_random_tests();
    register_random_vfs_tests();
    register_random_syscall_tests();
    register_random_seed_tests();
    register_random_vfs_write_tests();
    register_memory_safety_tests();
    register_memory_determinism_tests();
    register_no_dynamic_alloc_tests();
    register_sporadic_server_tests();
    register_atomic_tests();
    register_cross_arch_tests();
    register_vmm_tests();
    register_hal_bits_tests();
    register_o1_scheduler_tests();
    register_lock_order_tests();
    register_budget_tests();
    register_pip_tests();
    register_pcp_tests();
    register_lock_validator_tests();
    register_cpu_load_tests();
    register_slab_reclaim_tests();
    register_static_pools_tests();
    register_stack_profiler_tests();
    register_stack_alloc_tests();
    register_page_tables_tests();
    register_kernel_isolation_tests();
    register_memory_isolation_tests();
    register_buffer_pool_deterministic_tests();
    register_no_op_new_tests();
    register_locking_tests();
    register_locking_stress_tests();
    register_preemption_tests();
#if defined(CONFIG_ARCH_AARCH64)
     register_aarch64_tests();
#endif
#if defined(CONFIG_ARCH_RISCV64)
     register_riscv64_tests();
#endif
    register_external_test_classes();
}

// First half: up to register_atomic_tests() (~425 tests, well below the
// ~850-cycle snapshot-restore page-table corruption limit).
static void register_all_tests_first_half() {
    register_testrunner_tests();
    register_buffer_pool_tests();
    register_lib_tests();
    register_memory_tests();
    register_pmm_tests();
    register_mempool_tests();
    register_ipc_tests();
    register_ipc_robustness_tests();
    register_scheduler_tests();
    register_task_tests();
    register_driver_tests();
    register_vfs_tests();
    register_tmpfs_tests();
    register_tmpfs_invalid_mount_tests();
    register_tmpfs_mount_unmount_failure_tests();
    register_signals_tests();
    register_process_tests();
    register_elf_tests();
    register_elf_loader_tests();
    register_checked_ptr_tests();
    register_fstab_tests();
    register_rtc_tests();
    register_rlimit_tests();
    register_init_tests();
    register_syscall_tests();
    register_syscall_fuzz_tests();
    register_sync_tests();
    register_spinlock_tests();
    register_task_lifecycle_tests();
    register_idle_task_tests();
    register_zombie_cleanup_tests();
    register_wcet_cleanup_tests();
    register_idle_cleanup_tests();
    register_vfsd_tests();
    register_iocd_tests();
    register_health_tests();
    register_timer_tests();
    register_timing_tests();
    register_spsc_tests();
    register_idt_tests();
    register_pipe_tests();
     register_gcov_tests();
     register_debug_tests();
     register_framebuffer_tests();
#if CONFIG_VERSION_NUM >= 0x000309
     register_preemption_under_syscall_tests();
#endif
     register_spinlock_stress_tests();
    register_atomic_context_switch_tests();
    register_bench_syscall_latency_tests();
     register_bench_irq_latency_tests();
     register_starvation_deadlock_tests();

     // v0.3.4 tests
     register_apic_timer_tests();
     register_jitter_tests();
     register_deadline_miss_tests();
    register_wcet_overrun_tests();
    register_ss_deadline_tests();
    register_deadline_recovery_tests();
    register_deadline_action_tests();
    register_wcet_scheduler_tests();
    register_priority_inheritance_tests();
    register_queue_pip_tests();
    register_mutex_pcp_tests();
    register_pml4_clone_tests();
    register_waitpid_tests();
    register_resource_exhaustion_tests();
    register_block_device_tests();
    register_fat32_tests();
     register_vfs_fat32_tests();
#if CONFIG_VERSION_NUM >= 0x000309
     register_ipc_blocking_tests();
#endif
     register_ipc_lock_free_tests();
    register_vfsd_authorization_tests();
    register_textutils_tests();
    register_shell_interaction_tests();
    register_irq_guard_tests();
    register_irqguard_audit_tests();
    register_shell_redirect_tests();
    register_klog_tests();
    register_dmesg_tests();
    register_hal_tests();
    register_buildsystem_tests();
    register_secure_exec_tests();
    register_pci_tests();
    register_virtio_tests();
    register_dma_tests();
    register_net_tests();
    register_ipc_benchmark_tests();
    register_microkernel_transition_tests();
    register_random_tests();
    register_random_vfs_tests();
    register_random_syscall_tests();
    register_random_seed_tests();
    register_random_vfs_write_tests();
    register_memory_safety_tests();
    register_memory_determinism_tests();
    register_sporadic_server_tests();
    register_atomic_tests();
}

// Second half: the remaining tests.
static void register_all_tests_second_half() {
    register_cross_arch_tests();
    register_vmm_tests();
    register_hal_bits_tests();
    register_o1_scheduler_tests();
    register_lock_order_tests();
    register_budget_tests();
    register_pip_tests();
    register_pcp_tests();
    register_lock_validator_tests();
    register_cpu_load_tests();
    register_slab_reclaim_tests();
    register_static_pools_tests();
    register_stack_profiler_tests();
    register_stack_alloc_tests();
    register_page_tables_tests();
    register_buffer_pool_deterministic_tests();
    register_no_op_new_tests();
#if defined(CONFIG_ARCH_AARCH64)
     register_aarch64_tests();
#endif
#if defined(CONFIG_ARCH_RISCV64)
     register_riscv64_tests();
#endif
    register_external_test_classes();
}

static constexpr size_t g_test_class_count =
    sizeof(g_test_classes) / sizeof(g_test_classes[0]);

// ---- register_class ----
// Looks up `name` in g_test_classes[], calls its register_fn, and records
// a class section boundary for output grouping.  Returns true on success.
bool kernel::test::register_class(const char *name) {
    g_current_class = name;
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
