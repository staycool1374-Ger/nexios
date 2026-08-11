build/initrd/initrd.o: src/initrd/initrd.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/programs/demo/demo.o: src/programs/demo/demo.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/lib/compiler_rt.o: src/lib/compiler_rt.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/lib/cxxabi.o: src/lib/cxxabi.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/lib/version.o: src/lib/version.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/lib/crc32.o: src/lib/crc32.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/lib/test.o: src/lib/test.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/lib/fdt/fdt_ro.o: src/lib/fdt/fdt_ro.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/lib/fdt/fdt_strerror.o: src/lib/fdt/fdt_strerror.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/lib/new.o: src/lib/new.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/lib/logger.o: src/lib/logger.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/lib/chacha.o: src/lib/chacha.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/services/program.o: src/services/program.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/services/terminal/framebuffer.o: src/services/terminal/framebuffer.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/services/terminal/terminal.o: src/services/terminal/terminal.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/services/shell.o: src/services/shell.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/gcov/gcov_handler.o: src/kernel/gcov/gcov_handler.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/bootparams.o: src/kernel/bootparams.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/irq_latency_histogram.o: src/kernel/irq_latency_histogram.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/vfs/vfsd.o: src/kernel/vfs/vfsd.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/vfs/pipe.o: src/kernel/vfs/pipe.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/vfs/devfs.o: src/kernel/vfs/devfs.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/vfs/initrd_fs.o: src/kernel/vfs/initrd_fs.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/vfs/fat32.o: src/kernel/vfs/fat32.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/vfs/procfs.o: src/kernel/vfs/procfs.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/vfs/tmpfs.o: src/kernel/vfs/tmpfs.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/vfs/vfs.o: src/kernel/vfs/vfs.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/vfs/fat32_fs.o: src/kernel/vfs/fat32_fs.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/net/arp.o: src/kernel/net/arp.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/net/net.o: src/kernel/net/net.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/kernel.o: src/kernel/kernel.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/memory/mempool.o: src/kernel/memory/mempool.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/memory/vmm.o: src/kernel/memory/vmm.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/memory/integrity.o: src/kernel/memory/integrity.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/memory/pmm.o: src/kernel/memory/pmm.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/memory/markers.o: src/kernel/memory/markers.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_deadline_recovery.o: src/kernel/test/test_deadline_recovery.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_idt.o: src/kernel/test/test_idt.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_klog.o: src/kernel/test/test_klog.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_irq_guard.o: src/kernel/test/test_irq_guard.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_wcet_overrun.o: src/kernel/test/test_wcet_overrun.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_dma.o: src/kernel/test/test_dma.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_wcet_scheduler.o: src/kernel/test/test_wcet_scheduler.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_mempool.o: src/kernel/test/test_mempool.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_pip.o: src/kernel/test/test_pip.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_idle_task.o: src/kernel/test/test_idle_task.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_testrunner.o: src/kernel/test/test_testrunner.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/bench_syscall_latency.o: src/kernel/test/bench_syscall_latency.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_irqguard_audit.o: src/kernel/test/test_irqguard_audit.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_ipc_robustness.o: src/kernel/test/test_ipc_robustness.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_hal.o: src/kernel/test/test_hal.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_syscall.o: src/kernel/test/test_syscall.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_buildsystem.o: src/kernel/test/test_buildsystem.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_textutils.o: src/kernel/test/test_textutils.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_shell_interaction.o: src/kernel/test/test_shell_interaction.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_pml4_clone.o: src/kernel/test/test_pml4_clone.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_ipc_blocking.o: src/kernel/test/test_ipc_blocking.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_budget.o: src/kernel/test/test_budget.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_locking_stress.o: src/kernel/test/test_locking_stress.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_fat32.o: src/kernel/test/test_fat32.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/bench_irq_latency.o: src/kernel/test/bench_irq_latency.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_no_dynamic_alloc_after_init.o: src/kernel/test/test_no_dynamic_alloc_after_init.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_zombie_cleanup.o: src/kernel/test/test_zombie_cleanup.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_buffer_pool_deterministic.o: src/kernel/test/test_buffer_pool_deterministic.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_atomic_context_switch.o: src/kernel/test/test_atomic_context_switch.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_jitter.o: src/kernel/test/test_jitter.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_task.o: src/kernel/test/test_task.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_starvation_deadlock.o: src/kernel/test/test_starvation_deadlock.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_debug.o: src/kernel/test/test_debug.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_memory.o: src/kernel/test/test_memory.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_idle_cleanup.o: src/kernel/test/test_idle_cleanup.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_slab_reclaim.o: src/kernel/test/test_slab_reclaim.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_ipc_lock_free.o: src/kernel/test/test_ipc_lock_free.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_ipc.o: src/kernel/test/test_ipc.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_shell_redirect.o: src/kernel/test/test_shell_redirect.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_virtio.o: src/kernel/test/test_virtio.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_random_seed.o: src/kernel/test/test_random_seed.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_registry.o: src/kernel/test/test_registry.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_pmm.o: src/kernel/test/test_pmm.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_infra.o: src/kernel/test/test_infra.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_elf.o: src/kernel/test/test_elf.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_checked_ptr.o: src/kernel/test/test_checked_ptr.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_init.o: src/kernel/test/test_init.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_memory_determinism.o: src/kernel/test/test_memory_determinism.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_lib.o: src/kernel/test/test_lib.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_pcp.o: src/kernel/test/test_pcp.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_waitpid.o: src/kernel/test/test_waitpid.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_net.o: src/kernel/test/test_net.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_random_syscall.o: src/kernel/test/test_random_syscall.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_ipc_extended.o: src/kernel/test/test_ipc_extended.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_tmpfs_invalid_mount.o: src/kernel/test/test_tmpfs_invalid_mount.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_resource_exhaustion.o: src/kernel/test/test_resource_exhaustion.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_config_checks.o: src/kernel/test/test_config_checks.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_rlimit.o: src/kernel/test/test_rlimit.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_kernel_isolation.o: src/kernel/test/test_kernel_isolation.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_tmpfs.o: src/kernel/test/test_tmpfs.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_vmm.o: src/kernel/test/test_vmm.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_syscall_fuzz.o: src/kernel/test/test_syscall_fuzz.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_dmesg.o: src/kernel/test/test_dmesg.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_process.o: src/kernel/test/test_process.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_random.o: src/kernel/test/test_random.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_lock_validator.o: src/kernel/test/test_lock_validator.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_vfsd_auth.o: src/kernel/test/test_vfsd_auth.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_iocd.o: src/kernel/test/test_iocd.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_signals.o: src/kernel/test/test_signals.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_stack_alloc.o: src/kernel/test/test_stack_alloc.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_apic_timer.o: src/kernel/test/test_apic_timer.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_framebuffer.o: src/kernel/test/test_framebuffer.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_block_device.o: src/kernel/test/test_block_device.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_priority_inheritance.o: src/kernel/test/test_priority_inheritance.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_config.o: src/kernel/test/test_config.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_driver.o: src/kernel/test/test_driver.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_static_pools.o: src/kernel/test/test_static_pools.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_vfsd.o: src/kernel/test/test_vfsd.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_mutex_pcp.o: src/kernel/test/test_mutex_pcp.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_stack_profiler.o: src/kernel/test/test_stack_profiler.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_gcov.o: src/kernel/test/test_gcov.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_atomic.o: src/kernel/test/test_atomic.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_preemption_under_syscall.o: src/kernel/test/test_preemption_under_syscall.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_pipe.o: src/kernel/test/test_pipe.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_pci.o: src/kernel/test/test_pci.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/resource_tracker.o: src/kernel/test/resource_tracker.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_spinlock.o: src/kernel/test/test_spinlock.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_random_vfs_write.o: src/kernel/test/test_random_vfs_write.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_daemon_restart_crash.o: src/kernel/test/test_daemon_restart_crash.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_scheduler.o: src/kernel/test/test_scheduler.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_memory_safety.o: src/kernel/test/test_memory_safety.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_tcb_write_log.o: src/kernel/test/test_tcb_write_log.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_tmpfs_mount_unmount_failure.o: src/kernel/test/test_tmpfs_mount_unmount_failure.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_no_op_new.o: src/kernel/test/test_no_op_new.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_rtc.o: src/kernel/test/test_rtc.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_locking.o: src/kernel/test/test_locking.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_o1_scheduler.o: src/kernel/test/test_o1_scheduler.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_microkernel_transition.o: src/kernel/test/test_microkernel_transition.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_deadline_miss.o: src/kernel/test/test_deadline_miss.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_ipc_benchmark.o: src/kernel/test/test_ipc_benchmark.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_sporadic_server.o: src/kernel/test/test_sporadic_server.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_secure_exec.o: src/kernel/test/test_secure_exec.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_spinlock_stress.o: src/kernel/test/test_spinlock_stress.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_health.o: src/kernel/test/test_health.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_cpu_load.o: src/kernel/test/test_cpu_load.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_wcet_cleanup.o: src/kernel/test/test_wcet_cleanup.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_timing.o: src/kernel/test/test_timing.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_memory_isolation.o: src/kernel/test/test_memory_isolation.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_isolate.o: src/kernel/test/test_isolate.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_ss_deadline.o: src/kernel/test/test_ss_deadline.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_cleanup.o: src/kernel/test/test_cleanup.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_watchdog.o: src/kernel/test/test_watchdog.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_queue_pip.o: src/kernel/test/test_queue_pip.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_task_lifecycle.o: src/kernel/test/test_task_lifecycle.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_timer.o: src/kernel/test/test_timer.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_lock_order.o: src/kernel/test/test_lock_order.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_vfs_fat32.o: src/kernel/test/test_vfs_fat32.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_buffer_pool.o: src/kernel/test/test_buffer_pool.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_vfs.o: src/kernel/test/test_vfs.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_page_tables.o: src/kernel/test/test_page_tables.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_random_vfs.o: src/kernel/test/test_random_vfs.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_cross_arch.o: src/kernel/test/test_cross_arch.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_weak_stubs.o: src/kernel/test/test_weak_stubs.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_fstab.o: src/kernel/test/test_fstab.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_hal_bits.o: src/kernel/test/test_hal_bits.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_sync.o: src/kernel/test/test_sync.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_preemption.o: src/kernel/test/test_preemption.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_wcet_memory.o: src/kernel/test/test_wcet_memory.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_freelist_consistency.o: src/kernel/test/test_freelist_consistency.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_deadline_action.o: src/kernel/test/test_deadline_action.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/test/test_spsc.o: src/kernel/test/test_spsc.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/driver/ahci.o: src/kernel/driver/ahci.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/driver/virtio_pci.o: src/kernel/driver/virtio_pci.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/driver/dma.o: src/kernel/driver/dma.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/driver/virtio_blk.o: src/kernel/driver/virtio_blk.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/driver/driver.o: src/kernel/driver/driver.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/driver/ata_pio.o: src/kernel/driver/ata_pio.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/driver/block_device.o: src/kernel/driver/block_device.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/driver/virtio_net.o: src/kernel/driver/virtio_net.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/driver/iocd.o: src/kernel/driver/iocd.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/random.o: src/kernel/random.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/irq_thread.o: src/kernel/irq_thread.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/daemon/daemon_mgr.o: src/kernel/daemon/daemon_mgr.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/task/taskdefs.o: src/kernel/task/taskdefs.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/task/dmesg_task.o: src/kernel/task/dmesg_task.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/task/sporadic_server.o: src/kernel/task/sporadic_server.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/task/tcb_write_log.o: src/kernel/task/tcb_write_log.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/task/scheduler.o: src/kernel/task/scheduler.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/task/task.o: src/kernel/task/task.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/task/ready_queue_manager.o: src/kernel/task/ready_queue_manager.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/task/deadline_list.o: src/kernel/task/deadline_list.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/task/all_tasks_registry.o: src/kernel/task/all_tasks_registry.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/task/task_queue.o: src/kernel/task/task_queue.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/log/ring_buffer.o: src/kernel/log/ring_buffer.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/log/dmesg.o: src/kernel/log/dmesg.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/ipc/buffer_pool.o: src/kernel/ipc/buffer_pool.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/ipc/ipc.o: src/kernel/ipc/ipc.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/sync/mutex.o: src/kernel/sync/mutex.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/sync/eventgroup.o: src/kernel/sync/eventgroup.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/sync/sync.o: src/kernel/sync/sync.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/sync/notify.o: src/kernel/sync/notify.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/sync/queue.o: src/kernel/sync/queue.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/sync/semaphore.o: src/kernel/sync/semaphore.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/syscall/syscall_handlers_process.o: src/kernel/syscall/syscall_handlers_process.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/syscall/syscall.o: src/kernel/syscall/syscall.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/syscall/syscall_handlers_sync.o: src/kernel/syscall/syscall_handlers_sync.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/syscall/syscall_handlers_ipc.o: src/kernel/syscall/syscall_handlers_ipc.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/syscall/syscall_handlers_fs.o: src/kernel/syscall/syscall_handlers_fs.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/syscall/syscall_handlers_misc.o: src/kernel/syscall/syscall_handlers_misc.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/elf/elf.o: src/kernel/elf/elf.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/debug/dump.o: src/kernel/debug/dump.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/arch/pci.o: src/kernel/arch/pci.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/arch/x86_64/hal/idt.o: src/kernel/arch/x86_64/hal/idt.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/arch/x86_64/hal/timer.o: src/kernel/arch/x86_64/hal/timer.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/arch/x86_64/hal/apic.o: src/kernel/arch/x86_64/hal/apic.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/arch/x86_64/hal/gdt.o: src/kernel/arch/x86_64/hal/gdt.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/arch/x86_64/serial.o: src/kernel/arch/x86_64/serial.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/arch/x86_64/rtc.o: src/kernel/arch/x86_64/rtc.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
build/kernel/arch/x86_64/keyboard.o: src/kernel/arch/x86_64/keyboard.cpp
	@mkdir -p $(dir $@)
	@printf "  %s %s\n" CC $@
	$(CXX) $(CXXFLAGS) -c -o $@ $<
