# Jarvis RTOS — Development Roadmap

**Build:** v0.3.11-dev | **Last Release:** v0.3.10 | **Completed milestones:** see `ROADMAP_done.md` (v0.2.x — v0.3.11)

## Safety & Concurrency Guardrails (Strict)
- **Transition to Fine-Grained Locks:** All new synchronization code must use `SpinLock` + `SpinLockGuard` for short critical sections and `sync::Mutex` (without IrqGuard) for blocking paths. The global `IrqGuard` is deprecated for all uses except boot, panic, and test isolation.
- **Reference-Enforced Tasks:** When manipulating task blocks or IPC endpoints within the new init system or system calls, strictly enforce reference passing over raw pointers to prevent dangling lookups.
- **Zero-Allocation tmpfs Operations:** Ensure the initial `tmpfs` implementation relies on the pre-existing fixed `MemPool` / `BufferPool` infrastructure for its nodes to avoid unbounded allocations that violate resource tracking limits.

## Active Development — v0.3.11

### Test-Suite Compliance Audit — implement every finding of `audits/test-suite-v0.3.10.md`

Source: full audit of `src/kernel/test/**` (151 files, ~1000 tests) against the
driven-test cookbook (`test.hpp` §186-279), `docs/specs/test-harness.md`, and
the kernel specs.  All findings below are VERIFIED against the code
(2026-08-06); detailed per-test classification is in
`audits/test-suite-v0.3.10.md`.

#### P0 — memory-safety / hang hazards (fix first)

- [ ] **T0-1 — Rule-4 teardown on self-terminated tasks (~50 sites):** replace
      `Scheduler::remove_task()+t->cleanup()+delete t;` with the sanctioned
      `drain_zombie_list()` / `terminate_and_drain()` for every task the
      trampoline already routed to the zombie list.  Today only the drain's
      magic-guard (`scheduler.cpp:213`) prevents a UAF on a recycled block.
      Files: test_scheduler, test_preemption_under_syscall (x4),
      test_atomic_context_switch (x2), test_ipc, test_ipc_blocking (x3),
      test_ipc_lock_free (x2), test_buffer_pool (x3), test_queue_pip (x3),
      test_locking_stress (x2), test_spinlock (x2), test_spinlock_stress,
      test_random_syscall (x4), test_priority_inheritance (x5),
      test_testrunner (x4), test_resource_exhaustion, test_freelist_consistency,
      test_vfsd, test_vfsd_auth (x5), test_fpu (x5), deadline/sporadic/timing
      family (test_deadline_action, test_deadline_miss, test_deadline_recovery,
      test_ss_deadline, test_timing, test_wcet_overrun, test_wcet_scheduler).
- [ ] **T0-2 — `atomic_sb_litmus` never dispatches its workers**
      (test_atomic.cpp:145-219): prio-5 workers < harness prio 10, so the ISR
      epilogue always re-selects the harness (INV-4) and the store/fence/load
      sequences never run — the litmus passes vacuously.  Raise to prio ≥ 11
      and drive through real dispatch, or convert to a real concurrency test.
- [ ] **T0-3 — `vfs_pipe_read_write` double-close UAF** (test_vfs.cpp:401-404):
      manual `ops->close` on both pipe ends frees the vnodes (`pipe.cpp:116,133`)
      then `fd_table.free` ref-decs freed blocks.  Rewrite to the single-close
      FdTable path; drop the duplicate of `pipe_write_then_read_roundtrip`.
- [ ] **T0-4 — `test_tcb_write_log` corrupts the live current task**
      (test_tcb_write_log.cpp:25-48): writes 0xDD into the harness task's magic
      and calls `cleanup()` on it mid-execution.  Rewrite against a dedicated
      orphan TCB (or copy-on-write), never the running harness.
- [ ] **T0-5 — `memory_safety_pmm_free_zero` frees reserved page 0**
      (test_memory_safety.cpp:79-82): `PMM::free_page(0)` is NOT a no-op — it
      clears the bitmap and pushes phys 0 onto the free list (pmm.cpp:88-92,
      456-482), contradicting `memory.md` reserved-page binding.  Remove or
      assert the reserved-range invariant instead.
- [ ] **T0-6 — buffer-pool VA collision with `kUserYieldStubVa`**
      (test_buffer_pool_deterministic.cpp:122): buffer VA `0x40000000` equals
      the user-yield stub VA (`task.cpp:288`); `BufferPool::map` overwrites the
      stub PTE.  Move the buffer VA above `0x100000000` (also audit the 12
      other `test_buffer_pool*.cpp` tests using sub-0x100000000 VAs).
- [ ] **T0-7 — `daemon_restart_after_cleanup_crash` codifies a latent crash as
      PASS** (test_daemon_restart_crash.cpp:52-71): asserts only task presence
      over a documented crash sequence.  Fix the daemon-restart bug or gate
      `#if 0`; never pass while the crash is live.
- [ ] **T0-8 — `apic_timer_oneshot` / `apic_timer_stop_restart` can kill the
      system tick** (test_apic_timer.cpp:61-71, 102-106): oneshot sets
      `periodic_ns_ = 0` (apic.cpp:224) and `timer_start` no-ops
      (apic.cpp:275), leaving the tick dead; stop_restart asserts before
      re-arming (suite-hang on failure).  Restore periodic state in-test and
      move asserts after re-arm.

#### P1 — driven-test discipline (cookbook compliance)

- [ ] **T1-1 — Rule-5 assert-before-cleanup reorder:** move every
      `JARVIS_ASSERT*` after its `drain_zombie_list()` / `terminate_and_drain()`
      (assertions `return` on failure and leak/hang the class).  Files listed in
      §2.3 of the audit report: test_sync (x5), test_locking (x13),
      test_locking_stress (x5), test_mutex_pcp (x3), test_queue_pip (x3),
      test_zombie_cleanup (x2), test_idle_cleanup, test_preemption (x4),
      test_rlimit (x5), test_waitpid (x2), test_signals, test_apic_timer, plus
      every P0-1 file's teardown ordering.
- [ ] **T1-2 — Rule-2 single-`arch::IrqGuard` registration:** register all
      cooperating tasks under one IrqGuard so a timer tick cannot split the
      scenario.  Files: test_idle_cleanup, test_scheduler (x2), test_preemption,
      test_o1_scheduler (x2), test_idle_task, test_ipc_blocking,
      test_atomic, test_testrunner (x6), test_starvation_deadlock (x2),
      test_deadline_recovery, test_ipc_robustness, FPU suite (test_fpu,
      test_fpu_multi, test_fpu_sse, test_fpu_xmm_all), test_signals.
- [ ] **T1-3 — Rule-3 observed-state spin:** replace bounded `reschedule()` poll
      loops with `while (t->state != …) asm volatile("pause")` in the FPU/MXCSR/
      XMM suite and test_testrunner; raise prio 1-4 workers to the ≥11 driven
      floor (`test-harness.md` §5).
- [ ] **T1-4 — Replace direct-ISR / fake-tick simulation with real paths:**
      `test_cross_arch.cpp:314` (`Timer::handle_irq`), `test_dma.cpp:263,322,347`
      (`DmaEngine::handle_irq` + fabricated completion on fake port 0xFF00),
      `test_cross_arch.cpp:266` + `test_timer.cpp:62-84` (`set_ticks_for_test`
      fake ticks), `test_atomic_context_switch.cpp:237-272`
      (`scheduler_on_context_switch` impersonation).  Drive through the real ISR
      or a sanctioned driver-level mock.
- [ ] **T1-5 — Helper-API consolidation:** replace `release_task()` and inline
      `remove_task+cleanup+delete` with `terminate_and_drain()`; replace
      `dl_make` (`test_timing.cpp:511-530`), raw `state=BLOCKED; register_task`
      (`test_ipc_extended.cpp:182-183,411-412`), `register_blocked_receiver`
      (`test_ipc.cpp:74-77`), and raw `set_current` (`test_jitter.cpp:53-55,
      131-133`) with the official `create_test_task()` / `yield_as()` helpers.

#### P2 — specification & documentation alignment

- [ ] **T2-1 — Remove/fix spec-contradicting assertions:**
      `scheduler_shorter_period_preferred` (next_task has no period tiebreak —
      false-positive, test_scheduler.cpp:427-447); `syscall_fork_returns_pid`
      (tautology vs kernel `-1`, test_syscall.cpp:401,411);
      `idt_syscall_handler_installed` (LSTAR not int 0x80, test_idt.cpp:64-66);
      `deadline_list_remove_absent` (node IS a member, test_timing.cpp:612-628);
      `waitpid_cr3_switch_on_status_write` (never calls waitpid/sys_exit,
      test_waitpid.cpp:194-268); `PcpPipFallback` / `PcpCeilingDisabled`
      (named paths never exercised, test_mutex_pcp.cpp).
- [ ] **T2-2 — Fix misleading priority/wake-order tests:** either add real
      boost/restore or wake-order probes, or rename `mutex_priority_inheritance_indirect`,
      `mutex_priority_chain`, `mutex_waiter_priority_order`,
      `semaphore_wait_priority_order`, `priority_inversion_under_contention`
      (test_locking*, test_locking_stress).
- [ ] **T2-3 — Reconcile cross-file contradiction:** `test_starvation_deadlock.cpp:100-104`
      claims "Mutex::lock() cannot block a dispatched task" vs
      `test_priority_inheritance.cpp:90-107` which relies on genuine mutex
      blocking.  One is stale — resolve against the real `Mutex::lock` retry
      budget semantics.
- [ ] **T2-4 — Update `docs/specs/test-harness.md:54-55`** to match the v0.3.10
      cookbook rule 6 (external termination of a semaphore-blocked task is now
      SAFE via the `waiting_on_semaphore` teardown).
- [ ] **T2-5 — De-QEMU-specific hard asserts:** replace machine-specific
      assertions in `test_pci.cpp` (host-bridge class/subclass, ISA at 00:01.0,
      no-MSI) and `test_virtio.cpp` (requires virtio-net) with capability-gated
      probes so the `net`/`pci` classes do not fail on other QEMU configs.
- [ ] **T2-6 — Fix stale spec comments:** `test_memory_safety.cpp:40` (largest
      pool is 8192 not 4480); `test_timing.cpp` header "never mutate
      deadline_ticks" vs `dl_make:521`; `test_jitter.cpp:22` "bounded <10× min"
      vs `avg < 1e6`; `sporadic_server.cpp` EXHAUSTED bg_prio=42 config.

#### P3 — trivial / redundant test purge

- [ ] **T3-1 — Delete pure-pass stub files (assert nothing), ~90 tests:**
      test_capability (22), test_gdt (5), test_pic (3), test_gic (3),
      test_plic (3), test_threaded_irqs (3), test_irq_alloc (3),
      test_address (6), test_bootparams (4), test_multiboot (5),
      test_serial (4), test_keyboard (5), test_vfs_internal (8),
      test_mlock (5), test_stress (6), test_wfg (4), test_deadlock_detect (6),
      test_deadlock_recovery (6), test_integration (1),
      test_tmpfs_io_timeout (1), test_tmpfs_corrupted_metadata (1).
- [ ] **T3-2 — Delete or implement the stub blocks in otherwise-live files,
      ~60 tests:** test_vfsd (11 daemon-auth stubs), test_iocd (7),
      test_driver (4), test_health (5), test_gcov (4), test_debug (2),
      test_textutils (1), test_spinlock (2), test_ipc_extended (2),
      test_pipe (2), test_cpu_load (2), test_memory (empty register),
      test_ipc_benchmark (1).  Where the kernel API is unimplemented (capability,
      mlock, vfsd-auth, threaded-irqs), either implement it or gate the stubs
      `#if 0` with a comment — do not count unconditional passes as coverage.
- [ ] **T3-3 — Remove tautologies / assert-nothing bodies:** `syscall_dispatch_get_ticks`
      (`g_ret>0 || true`), `IpcLatencyJitter` (`max>=min`),
      `vfsd_absent_syscall_fails` (`==-1 || >=0`), `process_num_children_count`
      (`(void)…`), `lock_order_consistent_nesting`/`lock_order_three_way`
      (`JARVIS_ASSERT(true)`), and the `if(!sender){PASS;return}` silent-OOM
      skips (test_ipc_extended:355, test_ipc_robustness:315, test_buffer_pool).
- [ ] **T3-4 — Remove verbatim duplicates (~20 tests):** lifecycle_zombie_no_waker,
      task_cleanup_frees_msg_queue_with_blocked_senders,
      elf_load_init_task_common_called, kernel_hlt_idle_still_exists,
      idle_task_calls_pause_syscall, multiple_idle_tasks_prevented,
      MempoolFragmentation, slab_reclaim_reallocate,
      timer_deadline_miss_detection_fires/_skips_future/_skips_zero,
      DeadlineMonitorDetectsMiss, SsDeadlineMissDuringReplenish,
      ipc_priority_inheritance_send, IpcQueueWraparoundEdge,
      ipc_receive_was_blocked_restores_state, pml4_dump_no_user_entries,
      fat32_dir_attribute_* (4), fat32_chain_corrupt_* (2),
      hal_bits msb/low duplicates (2), preemption_interrupt_enable_disable_cycle,
      scheduler_quantum_exhaustion.
- [ ] **T3-5 — Fix misleading names or behaviors:** idle_task_calls_pause_syscall,
      pipe_write_to_full_blocks, queue_send_receive_block,
      syscall_dispatch_reboot/_halt (enum-only), bench_syscall_latency (measures
      no syscalls), slab_reclaim_pages_returned/_free_idempotent,
      page_tables_pool_exhaustion, ata_pio_identify/_read_write_sector
      (MockBlockDevice only), vmm_clone_failure_rollback, hal_page_table_map_unmap/_clone,
      irqguard_remaining_sites_validated, checked_ptr_valid, klog_concurrent_readers,
      klog_invalid_buffer_eFault, pml4_fork_no_child_corrupt_parent,
      timer_rate_monotonic_schedule_indirect.

**Fix discipline (per AGENTS.md Mandatory Bugfix Sequence):**
1. P0 items first, one at a time (T0-1 → T0-8); each verified by the smallest
   applicable class gate (`buffer_pool`, `scheduler`, `ipc`, `vfs`,
   `lock_protocol`, `memory_safety`, `process`, `testrunner`).
2. P1 cookbook compliance after P0; re-run `vfs`, `lock_protocol`, `scheduler`,
   `ipc_blocking`, `memory` to 0 failures.
3. P2 spec/doc alignment; P3 purge last (removes registered-test counts — update
   `test_expected_counts.hpp` / CI expectations when deleting).
4. Keep `CONFIG_DEBUG_IPC_SCHED` OFF for per-class gates; ON only for targeted
   debug analysis (the H2 race that required it for the debug `all` gate is
   RESOLVED, ROADMAP v0.3.9).

**Acceptance criteria (DONE when):**
- T0-1..T0-8 fixed and each verified by a class gate (no ResourceTracker deltas).
- T1-1..T1-5 applied: no `remove_task+cleanup+delete` on self-terminated tasks,
  no assert-before-cleanup, no direct-ISR/fake-tick simulation, no raw helper
  re-implementations remain.
- T2-1..T2-6 resolved: every audited assertion matches its spec; test-harness.md
  rule 6 updated.
- T3-1..T3-5 purged; registered-test counts updated in `test_expected_counts.hpp`.
- `make build` clean (Errors: 0); `selftest` green; every touched class 0 failures;
  `test-history.txt` rows appended.

**Out of scope:** ~~H2 race (v0.3.9)~~ (RESOLVED 2026-08-08), BufferPool +1 (v0.3.11),
ISO 26262 certification artifacts, and kernel-behavior changes not directly
required to make a test match its documented contract.


## Past Releases

See `ROADMAP_done.md` for completed items: v0.2.x — v0.3.11 (boundary audit, PfA concurrency redesign, test hygiene, H2 race, trigger-driven testing rework, BufferPool +1 PMM leak).

---

## Future Roadmap (Aspirational)

### Phase 4.5: Memory Protection (0.4.x) — prerequisite for safe SMP
- [ ] **Requirement spec:** `docs/specs/memory.md` §7 (REQ-MP-01..06). Current state: user↔user isolation + user-stack guard pages present; kernel-task↔kernel-task isolation ABSENT; software canaries absent. Decisions: full private kernel page tables, both MMU guard pages + software canaries, HW enforcement (SMAP/SMEP/PAN/PXN) recommended-not-mandatory.
- [ ] **0.4.0-MP1** — Private kernel-half page tables per kernel task (clone kernel PML4, private data/bss/stack frames, CR3 switch; preserve HHDM for kernel→user access)
- [ ] **0.4.0-MP2** — MMU red-zone guard pages between text/data/heap/stack segments (kernel + user tasks)
- [ ] **0.4.0-MP3** — Software sentinel canaries at segment boundaries, verified on syscall + context-switch entry
- [ ] **0.4.0-MP4** — Optional HW enforcement: SMAP/SMEP (x86_64) / PAN/PXN (aarch64) with `stac/clac` audit
- [ ] **0.4.0-MP5** — Verification suite: cross-task #PF tests, canary-tamper detection, HHDM kernel→user read, SMAP/PAN negatives
- [ ] **0.4.0-MP6** — Kernel stack guard page via private VA window (moved from v0.3.7; requires snapshot-safe page table pool)
- [ ] **0.4.0-MP7** — `page_table_shared_` removal — complete deep-copy fork (walk all user entries, allocate new PDPT/PD/PT, copy contents). Current state: config + pool done.

### Phase 5: SMP + Multicore (0.4.x)
#### 0.4.1–0.4.2 — APIC & SMP Boot
- [ ] Local/IO APIC, X2APIC, per-CPU GDT/TSS, INIT-SIPI AP startup
- [ ] TPR-based interrupt prioritization, core state isolation
- [ ] **Per-CPU asm for `isr_nesting_depth`** — move from the single global symbol to GS-relative access on x86_64 (TPIDR/tp on aarch64/riscv64); the C++ side already uses `__atomic_*` (v0.3.7 PfA-B). CpuContext plumbing (`current_cpu()`) is in place.
- [ ] **`hhdm_modified_` (VAR-17) re-audit** — task-context only today (single-core safe); re-audit under SMP with per-CPU ownership or atomics.

#### 0.4.3–0.4.4 — Per-CPU Scheduling & Cache
- [ ] Distributed run queues, real-time load balancer, SYS_SET/GET_AFFINITY
- [ ] Cache coloring allocator, SMP spinlocks/rwlocks, WCET re-audit

#### 0.4.5–0.4.6 — TLB Shootdown & IPI Reduction
- [ ] PCID, selective INVPCID, lazy shootdowns, IPI batching, latency profiling

### Phase 6: System Integration / Userspace ABI (0.5.x)

**Priority:** picolibc integration — syscall ABI, TLS, POSIX stubs.

#### Syscall ABI Definition
- [ ] **Document trap/IRQ numbers** — create `src/kernel/syscall/syscall.h` with stable, documented trap vectors and IRQ numbers
- [ ] **Register conventions** — specify register layout for syscall arguments and return values per architecture (x86_64: `rax=num, rdi, rsi, rdx, r10, r8, r9`; aarch64: `x8=num, x0-x5`; riscv64: `a7=num, a0-a5`)
- [ ] **syscall.h public header** — export to userspace, used by both kernel dispatcher and libc stubs

#### picolibc Integration
- [ ] **POSIX syscall stubs** — implement `src/libc/picolib_stubs.c` with wrappers for `_write`, `_read`, `_sbrk`, `_exit`, `_open`, `_close`, `_fstat`, `_lseek`, `_getpid`, `_kill` using `jarvis_syscall()` dispatcher
- [ ] **Build picolibc** — compile with meson as `libc.a` + `libm.a` (static), targeting x86_64-elf
- [ ] **Makefile integration** — link `libc.a`/`libm.a` into kernel image; add build rules for picolibc subproject
- [ ] **TLS on context switch** — every task switch must load the thread-local-storage address into the appropriate base register (`FS` on x86_64, `TPIDR_EL0` on aarch64, `tp` on riscv64). picolibc uses this for `errno` and per-task internal state — no global locks needed.
- [ ] **Verify** — `printf`, `malloc`, `scanf` work from userspace tasks via syscall stubs

### Phase 7: Safety Systems (0.6.x)
- [ ] ICH9/HPET hardware watchdog + NMI pre-timeout, PIT fallback, SYS_WATCHDOG_KICK
- [ ] Per-task software watchdog (SYS_WATCHDOG_CREATE), /proc/[pid]/watchdog
- [ ] Wait-for-graph deadlock detection, watchdog-driven recovery, SYS_HEALTH_STATUS
- [ ] Idle-task safety monitors: RAM March C-, CPU ALU verification, utilisation tracking

### Phase 8: Microkernel Transition (0.7.x–0.8.x)
- [ ] Externalise VFS & block I/O to user-space servers
- [ ] Externalise device drivers (keyboard, framebuffer, timer/RTC)
- [ ] Kernel reduction: scheduler, IPC, page-table management, interrupt routing only
- [ ] Capability-based security (SYS_CAP_GRANT / SYS_CAP_REVOKE)

### Phase 9: Hardware Drivers & Protocols (0.9.x)
- [ ] Full TCP/IP stack (ARP, IP, ICMP, UDP, TCP) with Ethernet NIC driver
- [ ] USB driver stack (UHCI/EHCI/xHCI)
- [ ] Hot-path secure call sequence layer (<seqguard.hpp>)
