#pragma once

/// @file test_expected_counts.hpp
/// @brief Expected test-case count tables per architecture.

#include <types.hpp>
#include <logger.hpp>

namespace kernel::test {

struct ExpectedCounts {
    const char *name;
    size_t x86_64;
    size_t aarch64;
    size_t riscv64;
};

static constexpr ExpectedCounts k_expected_counts[] = {
    // Class name           x86_64  aarch64  riscv64
    {"safe",                136,    0,       0      },  // curated TF_RELEASE subset (85 executed, +49 TF_KERNEL incl. ata_pio_absent_drive_init_false, issue #66) + 2 CRC32 lib tests (#126)
    {"selftest",            136,    0,       0      },  // same as safe
    {"testrunner",           16,    0,       0      },  // harness + freelist + infra + expected-panic (v0.3.8)

    // basic
    {"basic_lib",            20,    0,       0      },  // string/utils/type-traits/ErrorOr/version + CRC32 vectors (#126) + static-DTB fdt_* tests (#183)
    {"basic_atomic",         12,    0,       0      },  // atomic RMW ops, litmus, acquire/release

    // configuration
    {"configuration_build",  10,    0,       0      },  // buildsystem + compile-time config sanity (v0.3.7)

    // data_structures
    {"data_structures_spsc",  8,    0,       0      },  // SPSC queue primitives
    {"data_structures_buffer_pool", 25, 0,    0      },  // alloc/free/transfer + v0.3.11 B1-B3 PT-walk regressions
    {"data_structures_buffer_pool_deterministic", 6, 0, 0},  // pre-allocated buffers, zero-copy, no alloc after init

    // synchronization
    {"synchronization_spinlock", 10, 0,      0      },  // spinlock(9) + spinlock_stress(1)
    {"synchronization_sync", 19,    0,       0      },  // semaphore/mutex/queue/eventgroup primitives + block-pattern (6)
    {"synchronization_locking", 19, 0,       0      },  // locking(13) + locking_stress(6)
    {"synchronization_lock_order",  4, 0,    0      },  // nested SpinLock acquisition order
    {"synchronization_lock_validator",  6, 0, 0      },  // lock validator
    {"synchronization_irq_guard",  4, 0,    0      },  // irq_guard(3) + irqguard_audit(1)
    {"synchronization_pip",  10,    0,       0      },  // pip(7) + queue_pip(3)
    {"synchronization_pcp",   8,    0,       0      },  // pcp(5) + mutex_pcp(3)
    {"synchronization_pi_donation",  5, 0,   0      },  // PI donation mutex+semaphore

    // scheduler
    {"scheduler_core",       17,    0,       0      },  // reschedule/remove/reap/quantum/waitpid/FIFO + loadavg EMA math (issue #172)
    {"scheduler_o1",         13,    0,       0      },  // O(1) priority map / ready queue
    {"scheduler_atomic",      6,    0,       0      },  // atomic context-switch invariants
    {"scheduler_sporadic",   25,    0,       0      },  // sporadic server scheduling policy
    {"scheduler_idle",       11,    0,       0      },  // idle_task(10) + idle_cleanup(1)
    {"scheduler_zombie",      5,    0,       0      },  // zombie_cleanup(4) + wcet_cleanup(1)
    {"scheduler_preemption", 13,    0,       0      },  // preemption(7) + preemption_under_syscall(4) + 2 block-arm tests (issue #212)
    {"scheduler_budget",      6,    0,       0      },  // task budget accounting
    {"scheduler_cpu_load",    5,    0,       0      },  // idle/CPU load metrics
    {"scheduler_starvation",  3,    0,       0      },  // SchedulerStarvation + PriorityInversionChain5 + DeadlockNestedMutexLoad
    {"scheduler_hrt",         4,    0,       0      },  // hard-RT time assertions on real dispatch (issue #102): rdtsc canary + IPC/semaphore wake latency + release jitter

    // task
    {"task_core",             6,    0,       0      },  // TCB cleanup/page tables/clone
    {"task_lifecycle",        9,    0,       0      },  // exit/zombie/reparent
    {"task_fpu",              0,    0,       0      },  // FPU test files excluded from x86_64 build (GCC 16); reserved home
    {"task_init",             5,    0,       0      },  // init task exists/reparents + reaper notify/IPC wake (issue #155)
    {"task_tcb_log",          1,    0,       0      },  // TCB write-log tracer
    {"fpu_invariants",        5,    0,       0      },  // FPU/SIMD context invariants (issue #93 + #151): no-alloc, nesting-impossible, alignment, own-arm no-clobber, percpu-reset

    // syscall
    {"syscall_core",         32,    0,       0      },  // syscall interface (exit test disabled in source) + 9 user-task probe/dispatch tests (#143, #127, #134: open, exec, klog) + 4 affinity tests (issue #61) + error_string map + 3 x86 ABI bridge tests (issue #30)
    {"syscall_fuzz",          4,    0,       0      },  // syscall fuzzing
    {"syscall_fastpath",      5,    0,       0      },  // tiered FAST/FULL dispatch (issue #92): mask, correctness, canary skip/full-validate, latency

    // process
    {"process_lifecycle",    16,    0,       0      },  // process lifecycle, child table (12 + 4 MP-1/7)
    {"process_elf",           9,    0,       0      },  // ELF loader validation/segments
    {"elf_loader",            8,    0,       0      },  // background chunked ELF loader (success/errors/cancel/cycles/yield)
    {"elf_shared",            9,    0,       0      },  // DT_NEEDED shared-object support (issue #95)
    {"process_signals",       8,    0,       0      },  // signal delivery/handling
    {"process_rlimit",        5,    0,       0      },  // getrlimit/brk
    {"process_waitpid",       3,    0,       0      },  // waitpid zombie/reap
    {"process_pml4_clone",   10,    0,       0      },  // fork deep-copy page tables (7 + 3 MP-7 named)
    {"pt_merge",              5,    0,       0      },  // kernel-half merge (issue #96): equivalence, idempotent, late-mapping, allocates-nothing, mismatch-detected
    {"process_secure_exec",   5,    0,       0      },  // exec argv/envp validation

    // cap — capability-based access control (CSpace)
    {"cap_core",             10,    0,       0      },  // CSpace engine: CNode/CSlot lifecycle, handle decode, revoke
    {"cap_lifecycle",         8,    0,       0      },  // grant/copy/revoke/mint + Endpoint/FrameCap objects
    {"cap_syscall",           8,    0,       0      },  // SYS_CAP_GRANT/COPY/REVOKE/MINT dispatch
    {"cap_ipc",               6,    0,       0      },  // cap-gated IPC + frame mapping
    {"cap_untyped",          18,    0,       0      },  // Untyped allocator + sub-range carve/child split (issue #1)
    {"cap_mmio",              14,   0,       0      },  // MMIO caps + I/O delegation (0.4.2 issue #3) + revocation-closure (issue #8)
    {"cap_mmio_user",         9,   0,       0      },  // user MMIO map/unmap syscalls + registry (0.4.2 issue #8)
    {"cap_irq",               12,   0,       0      },  // IRQ caps + user-space delivery (0.4.2 issue #2)
    {"cap_irq_notify",        7,   0,       0      },  // IRQ→IPC Notify bridge, NOTIFY mode (0.4.2 issue #7)
    {"cap_msix",              13,   0,       0      },  // per-vector MSI-X caps + delivery (0.4.2 issue #10)
    {"cap_iommu",             12,   0,       0      },  // IOMMU DMA protection (0.4.2 issue #4)
    {"iommu_live",            6,    0,       0      },  // Live VT-d enablement (0.4.2 issue #9; q35+intel-iommu variant only)
    {"cap_shm",               6,    0,       0      },  // capability-gated shared-memory rings (issue #106 Part B): map roundtrip, revoke denied, producer-consumer, death drain, revoke cleanup + frame_create validation (#134)
    {"cap_death",             9,    0,       0      },  // async task-death notifications (issue #105 Part B): roundtrip, crash reason, fan-in, after-death, supervisor-drain, full, exactly-once, unwatch, nonblock
    {"cap_pager",             15,   0,       0      },  // external pager protocol (issue #107 + dispatch rejects #134): authority, recv, classification, recover-IP, roundtrip, map-after-timeout, abort-poison, timeout, dead-drain, client-death, revoke, deadlock-out, smap/canary + recv-empty/abort-unregister dispatch
    {"ipc_fastpath",          14,   0,       0      },  // in-register IPC fastpath (issue #11): mask membership, ABI layout, pop_clamped parity, oversize-reject, roundtrip, recv-oversized-stays, send_sync-oversized-reply, full-queue-block, empty-block, send_sync roundtrip, authority, no-user-deref canary, latency, hybrid queue
    {"ipc_pipe_blocking",      6,   0,       0      },  // pipe blocking semantics (issue #111): reader wake, full-pipe partial write, write-close EOF, read-close EPIPE, two-reader order, closed-end errors
    {"vfs_procfs",             9,   0,       0      },  // procfs nodes (issue #109): root dir, readdir static/pid, meminfo format, pci, self stat, pid dir close, unknown reject, dir read
    {"vfs_tmpfs_corrupt",      7,   0,       0      },  // tmpfs corrupt/timeout analogue (issue #114): duplicates, missing unlink, non-empty dir, oversize, stale recycle, fragmentation, concurrent
    {"vfs_devfs",             10,   0,       0      },  // devfs device nodes (issue #124): root stable/reject/readdir/lookup + null, console, tty, kbd, random op contracts + init
    {"vfs_procfs_ops",         6,   0,       0      },  // procfs per-node op contracts (issue #124): root byte-op rejection, meminfo/pci readonly, self dir, pid dir, pid stat
    {"vfs_initrd_fs",          7,   0,       0      },  // initrd filesystem vnodes (issue #124): root dir/fail-closed lseek/readdir, lookup build, file readonly/lseek/close
    {"vfs_errors",             6,   0,       0      },  // VfsError *_err API (issue #124): fdtable codes, resolve, find_fs, mount, mkdir/create/unlink, init/set_root
    {"hal_rtc_datetime",       6,   0,       0      },  // RTC date arithmetic (issue #116): tm mapping, composition, stability, BCD edges/roundtrip/contract
    {"hal_keyboard_decode",   10,   0,       0      },  // keyboard decode (issue #110): tables, shift/ctrl/alt, break, unknown, control keys, caps XOR, read, flush
    {"hal_gdt_layout",         8,   0,       0      },  // GDT layout (issue #115): gdtr, null, code/data, user ring3, TSS base/limit, IOPB, live selectors
    {"hal_serial_logic",       6,   0,       0      },  // UART logic (issue #118): init regs, FIFO, loopback roundtrip, newline, puts/count, idle getchar
    {"acpi_parse",             5,   0,       0      },  // ACPI/DMAR discovery (issue #113): default contract, fail-closed scan, purity + mb2 zero-size bound + scan repeat (#180)
    {"smp_madt",               3,   0,       0      },  // ACPI MADT discovery (issue #25 Phase B1): default contract, BSP listed, purity
    {"smp_ipi",                2,   0,       0      },  // APIC IPI path (issue #25 Phase B2): absent-target INIT/SIPI accepted, self FIXED delivered
    {"smp_bringup",            5,   0,       0      },  // AP bring-up (issue #25 Phase B4 + #153): blob layout, staged block memcmp, parked count == MADT APs, mb2 intact + relocated
    {"sched_affinity",         7,    0,       0      },  // CPU affinity (issue #25 Phase C1): default mask, lowest-bit targeting, empty/user clamps, re-queue, is_idle + set_affinity_err OK (issue #23)
    {"smp_sched",              10,   0,       0      },  // AP scheduling (issue #25 Phase C1): pinned run + IPI wake + BSP unaffected + cross-CPU move + queue fanout (0-AP trivial pass) + 5 per-CPU admission/migration tests (issue #23)
    {"lapic",                6,   0,       0      },  // Local APIC (issue #85 module 2): enabled contract, ID stability, one-shot/periodic zero boundaries, bounded one-shot, EOI safety
    {"ioapic",               5,   0,       0      },  // I/O APIC (issue #85 module 3): boot routing liveness, mask/unmask cycle, invalid-IRQ reject, idempotence, lines sweep
    {"core_isolation",       5,   0,       0      },  // Core isolation (issue #85 module 5): slot stride, BSP slot, write isolation, AP-slot envelope, PML4 valid
    {"load_balancer",        4,   0,       0      },  // Load balancer stubs (issue #85 module 8): idle-pull/work-push/RT-exclusion/threshold pending balancer API
    {"cache_coloring",       4,   0,       0      },  // Cache-coloring allocator (issue #62; stubs from #85 module 10): spread/collisions/sizes/bit-extract real
    {"smp_sync",             6,   0,       0      },  // SMP sync (issue #85 module 11): IRQ-guard IF contract + unlock/relock (real), 2-CPU race/rwlock/migration/ticket stubs
    {"smp_verify",           5,   0,       0      },  // SMP verify (issue #85 module 12): census + AP-tick liveness + lock bound (real), inversion/soak stubs
    {"pcid",                 4,   0,       0      },  // PCID stubs (issue #85 module 13): CR4/tag/retention/rollover pending PCID API
    {"invpcid",              4,   0,       0      },  // INVPCID stubs (issue #85 module 14): single/context/all/nonexistent pending API
    {"lazy_tlb",             4,   0,       0      },  // Lazy-shootdown policy (issue #158; stubs from #85 module 15): defer/coalesce/quarantine/timeout real
    {"ipi_batching",         4,   0,       0      },  // IPI batching (issue #159; baseline from #85 module 16): unbatched 5:5 baseline, collapse/order/overflow real
    {"tlb_latency",          4,   0,       0      },  // TLB-latency profiling (issue #160; stubs from #85 module 17): record/avg/p99/batch-scaling real
    {"pml4_sync",            5,   0,       0      },  // PML4 sync (issue #85 module 18): remap/unmap table visibility (real), remote/write-barrier/remove-all stubs
    {"drivers_virtio_blk_req", 13,   0,       0      },  // virtio-blk request path (issue #117): init, null transport, timeout+descriptor layout, read/write roundtrip, error mapping, used cookie + 6 completion/ISR contracts (issue #65): match basic/null, ISR record, blocked roundtrip, timeout retire, teardown drain
    {"drivers_ahci_deep",      10,   0,       0      },  // AHCI protocol contracts (issue #108): CmdHeader/CmdTable layout, PRD encoding, NCQ tag, constants + 5 completion/ISR contracts (issue #64): signatures, match basic/NCQ/error/null
    {"ahci_live",              5,   0,       0      },  // real AHCI command path on q35+ICH9 variant (issue #108 + #64): probe, roundtrip, isolation + IRQ roundtrip + IRQ error path — NOT in all
    {"initrd_parser",          9,   0,       0      },  // cpio-newc parser (issue #112): find dotted/missing, corrupt magic, truncation, trailer, readdir order, normalization, zero-length, restore

    // ipc
    {"ipc_core",             23,    0,       0      },  // queue/priority/notify/eventgroup/sync roundtrip
    {"ipc_blocking",          4,    0,       0      },  // IPC blocking send_sync/handshake tests
    {"ipc_timeout",           7,    0,       0      },  // wheel-armed bounded receive: fastpath/timeout/msg-wins/forever/kill/stale/full (issue #18)
    {"ipc_extended",          11,   0,       0      },  // size limits, mid-queue removal, timeout, inversion + 2 arrival-wake gate tests (issue #208)
    {"ipc_lock_free",         3,    0,       0      },  // lock-free queue
    {"ipc_robustness",        7,    0,       0      },  // misformed/wraparound/concurrent/cleanup (+ IpcPriorityOrderedWake, issue #106 Part A)
    {"ipc_pipe",              6,    0,       0      },  // kernel pipe object

    // vfs
    {"vfs_core",             20,    0,       0      },  // fdtable/resolve/mount/mkdir/unlink
    {"vfs_tmpfs",            10,    0,       0      },  // tmpfs(6) + invalid_mount(2) + mount_unmount_failure(2)
    {"vfs_fstab",             5,    0,       0      },  // fstab parsing
    {"vfs_fat32",            40,    0,       0      },  // FAT32 fs unit tests
    {"vfs_fat32_integration", 14,    0,       0      },  // VFS-on-FAT32

    // servers
    {"servers_vfsd",         18,    0,       0      },  // VFS daemon kernel-bypass ops/auth (crash-restart tests disabled in source)
    {"servers_vfsd_auth",     23,   0,       0      },  // VFS daemon authorization + dup/dup2/pipe + fs handlers mkdir/unlink/rmdir/lseek/ioctl/readdir + kernel-task delivery fstat/stat/readdir (#134, #175) + bogus-whence rejection (#176)
    {"servers_iocd",          7,    0,       0      },  // IOCD daemon boots/IRQ/MMIO/affinity (crash-restart disabled in source)
    {"servers_daemon_restart", 2,    0,       0      },  // unknown-daemon rejection + terminate/ensure resurrect cycle (issue #135); crash test stays #if 0-disabled
    {"servers_health",        5,    0,       0      },  // SYS_HEALTH_STATUS metrics/procfs

    // memory
    {"memory_pmm",            9,    0,       0      },  // PMM alloc/free unit tests + window geometry (hosts 0-test delegate) + err wrappers (issue #143)
    {"memory_mempool",        5,    0,       0      },  // MemPool allocator tests + err/pin/Pool helpers (issue #143)
    {"memory_slab",           5,    0,       0      },  // Slab reclaim tests
    {"memory_safety",        11,    0,       0      },  // MemPool/PMM invariants + MP-2 red zones + MP-3 canaries
    {"memory_determinism",    4,    0,       0      },  // PMM exhaustion + no-dynamic-alloc neutral cycles (v0.3.8)
    {"memory_checked_ptr",    4,    0,       0      },  // Checked pointer + signal frame tests
    {"memory_resource_exhaustion", 5, 0,      0      },  // FdTable, TaskLimit, MaxBuffers, MempoolFrag, PmmExhaustion
    {"memory_stack_alloc",   11,    0,       0      },  // stack allocation, guard pages, overflow hook (8 + 3 MP-6)
    {"memory_stack_profiler", 6,    0,       0      },  // kernel stack depth profiling
#if CONFIG_STATIC_POOLS_ONLY
    {"memory_static_pools",   6,    0,       0      },  // CONFIG_STATIC_POOLS_ONLY, MemPool::reserve
#else
    {"memory_static_pools",   4,    0,       0      },  // MemPool::reserve only (no PMM gating without CONFIG_STATIC_POOLS_ONLY)
#endif
    {"memory_no_op_new",      6,    0,       0      },  // no operator new/delete, all MemPool / placement-new
    {"memory_page_tables",    9,    0,       0      },  // page-table pool, budget, no sharing
    {"memory_kernel_isolation", 6,  0,       0      },  // v0.4.0 MP-1 private kernel-half PML4s + teardown validation (issue #199)
    {"memory_isolation",      3,    0,       0      },  // v0.4.0 MP-5 cross-task / HHDM / guard-page proof
    {"memory_vmm",           13,    0,       0      },  // VMM map/unmap/clone/huge-page/hhdm + err wrappers + cap map paths (issue #143) + take semantics (#60)

    // wcet / deadline
    {"wcet_overrun",          2,    0,       0      },  // WcetOverrunDetectionFires + DeadlineMissWithinWcet
    {"wcet_scheduler",        2,    0,       0      },  // WCET benchmark for scan_deadlines (P7b) + balancer_tick worst case (issue #62 re-audit)
    {"bench_wcet_memory",    2,    0,       0      },  // WCET mempool/vmm (TF_BENCH)
#if CONFIG_DEADLINE_MONITOR_TASK
    {"deadline_miss",         5,    0,       0      },  // + DeadlineMonitorTaskSpawned + DeadlineMonitorDetectsMiss
#else
    {"deadline_miss",         3,    0,       0      },  // DeadlineMissWhileBlocked + DeadlineMissWhileTerminatedSkipped + DeadlineRearmOnPeriodRollover
#endif
    {"deadline_recovery",     4,    0,       0      },  // DeadlineActionKillCleansUp + DeadlineDetectionMagicCheck + DeadlineDetectionMcdcCoverage + DeadlineActionNotifyMonitor
    {"deadline_action",       1,    0,       0      },  // single action-dispatch test per build (CONFIG_DEADLINE_ACTION)
    {"deadline_ss",           3,   0,       0      },  // SsExhaustionTriggersDeadline + SsDeadlineMissDuringReplenish + SsReplenishAfterMiss

    // timing
    {"timing_core",          19,    0,       0      },  // tick accounting, alarm, rate-monotonic, deadline list + READY-no-charge regression (issue #154)
    {"posix_time",           10,    0,       0      },  // clock_gettime/nanosleep/timer_create/timerfd over HRT wheel (issue #76)

    // hal
    {"hal_core",             14,    0,       0      },  // HAL page tables/context/interrupts/timers/io/cpuid
    {"hal_bits",             14,    0,       0      },  // bit-manipulation utilities
    {"hal_idt",               6,    0,       0      },  // IDT entries/handlers/IST
    {"exc_table",             4,    0,       0      },  // ISR_ERR mask audit, #VE/#HV frame layout, reserved-vector routing + exception_name lookup (#131)
    {"hal_timer",             5,    0,       0      },  // PIT/timer subsystem
    {"irq_early_std",         7,    0,       0      },  // early IRQ+timer init conformance (issue #198)
    {"hal_apic",              3,    0,       0      },  // APIC timer tick rate, one-shot, stop
    {"apic_tpr",              6,    0,       0      },  // TPR classes/shadow/guard/vector reservation + live IPI block-and-hold (#26, issue #85 module 6)
    {"hal_rtc",               2,    0,       0      },  // RTC read/BCD

    // drivers
    {"drivers_core",          8,    0,       0      },  // driver registry, IOCD boots, keyboard/serial, MMIO caps + FLAW-08/10 bounded waits
        {"drivers_block",            12,   0,       0      },  // block device, ATA_PIO, AHCI
    {"drivers_pci",          16,    0,       0      },  // PCI enumeration/MSI/BARs (bounded-time test commented out)
    {"drivers_virtio",       11,    0,       0      },  // VirtIO probe/feature/queue + FLAW-03 net lock
    {"drivers_dma",          19,    0,       0      },  // DMA buffer/SG/PRD + FLAW-01/02 engine locking

    // network
    {"network_core",          18,   0,       0      },  // MAC/IPv4/ARP/checksum + mock-NIC RX dispatch + ICMP wire-order request (#138, #179) + malformed-length/version/truncation rejection (#178) + UDP build/poll/set_reply

    // shell / ui
    {"shell_interaction",    19,    0,       0      },  // shell commands (+ tasks memory columns)
    {"shell_redirect",        3,    0,       0      },  // shell I/O redirection
    {"shell_textutils",       1,    0,       0      },  // text utilities
    {"debug_dump",            4,    0,       0      },  // diagnostic dump smoke (issue #128): scheduler info, task info live+missing, all-tasks walk, cpu registers
    {"synchronization_err_api", 7, 0,       0      },  // sync *_err API (issue #132): EventGroup, Notify, Queue, Semaphore, Mutex, guards + SPSC ring
    {"kernel_top",            22, 0,       0      },  // Top-level kernel (#131): histogram + random + IrqThread (#144, unknown-vec/sync-ack/guard) + datetime + global_state accessors (#136)
    {"per_cpu",               4,  0,       0      },  // Per-CPU foundation (issue #25 + #151): frozen slot offsets, BSP identity, nesting-depth live storage, fpu-owner independence
    {"memory_checked_ptr_api", 9,  0,       0      },  // CheckedPtr/safe-copy template instantiations (issue #127): scalars, const types, VFS structs, SignalFrame, IPC records, zero-count, fail-closed copies + fault-recovery path (issue #143) + TaskTimes/const views
    {"memory_integrity",      2,  0,       0      },  // section markers + incremental kernel-text CRC (issue #127)
    {"profiler_sampler",     6,    0,       0      },  // sampling profiler API (issue #129): rate gate, ring wrap, non-destructive dump, symbol lookup bounds, symbol-table parsing, init reset
    {"shell_commands",       30,    0,       0      },  // shell command surface (issue #125): capture, listprog/run/registry, jobs/ulimit/wait, alias, history, type, set/shift, printf, test, trap, umask/times, dirs, cd/pwd, fs cycle, drivers/loader, dmesg, lspci, ifconfig, usage, source
    {"services_framework",    7,    0,       0      },  // services framework (issue #125): terminal colors, length-bounded write, cursor/splash, fb gate, scroll, program registry bounds
    {"ui_framebuffer",        5,    0,       0      },  // framebuffer init/putpixel/clear/scroll

    // random
    {"random_core",           7,    0,       0      },  // RNG smoke/non-repeating
    {"random_seed",           2,    0,       0      },  // random seed init
    {"random_syscall",        4,    0,       0      },  // getrandom syscall
    {"random_vfs",            2,    0,       0      },  // /dev/random VFS node
    {"random_vfs_write",      2,    0,       0      },  // /dev/random VFS write

    // logging / debug
    {"logging_dmesg",        15,    0,       0      },  // DmesgBuffer + error strings + suppression
    {"logging_klog",          8,    0,       0      },  // kernel log read/wrap/concurrent
    {"debug_core",            2,    0,       0      },  // write formats + switch logs (qemu_debug_exit tests disabled)
    {"debug_gcov",            4,    0,       0      },  // GCOV coverage metadata

    // arch
    {"arch_cross",           25,    0,       0      },  // cross-architecture tests (16 + 2 SMEP-gated + 3 SMAP-gated + 4 teardown, x86_64 only)
#if defined(CONFIG_ARCH_AARCH64)
    {"arch_aarch64",          0,   27,       0      },  // 17 existing + 5 MP-4.4 + 1 #103 deep-copy descriptor regression + 3 ABI frame tests (issue #30) + 1 EL0 fork smoke (issue #104)
#endif
#if defined(CONFIG_ARCH_RISCV64)
    {"arch_riscv64",          0,    0,       3      },  // 3 ABI frame tests (issue #30); validation enabled by nonzero counts
#endif

    // bench
    {"bench_ipc",             7,    0,       0      },  // IPC throughput (TF_BENCH)
    {"bench_syscall",         3,    0,       0      },  // syscall latency (TF_BENCH)
    {"bench_irq",             3,    0,       0      },  // IRQ latency histogram (TF_BENCH)
    {"bench_jitter",          2,    0,       0      },  // schedule-to-schedule jitter
    {"bench_microkernel",     5,    0,       0      },  // minimal privileged surface / isolation / jitter / drift

    // hrt — hard real-time measurement under QEMU -icount (issue #101)
    {"stress_hrt",            2,    0,       0      },  // rdtsc baseline canary + IPC-under-stress hard-bound (TF_KERNEL)
    {"hrt_monotonic",         5,    0,       0      },  // HRT monotonic clock: calibrate fail-closed + ns monotonicity + source validity + test-ticks + oneshot sub-tick (issue #16)
    {"timer_wheel",           8,    0,       0      },  // event-timer wheel: arm/expire/cancel/stale-gen/exhaustion/pop-cap/cpu-isolation/reset/burst (issue #17)
    {"task_metering",         7,    0,       0      },  // per-task exec-ns metering: switch-delta/no-charge/period-reset/self/other+esrch/null/terminate (issue #21)
    {"sched_edf",             7,    0,       0      },  // deadline-aware preemptive scheduling: dm-order/dm-clamp/edf-first/preempt/no-thrash/exempt/miss-recover (issue #19)
    {"sched_admission",       7,    0,       0      },  // enforced admission: lub-deny/wcet-period/untracked-exempt/implicit-defer/budget-create/exemptions/fork-deny (issue #20)
    {"sched_admission_verify", 7,   0,       0      },  // admission journeys: taskdefs-budgets/defer-retry/mode-parity/bg-guard/percpu-epsilon/budget-roundtrip/boot-parity (issue #24)
    {"aperiodic_servers",     7,    0,       0      },  // DS/BG modes: idle-preserve/topup/bursts/bg-priority/no-edf/admission-budgets/compat-default (issue #22)

    // Structural/semantic aggregates (issue #173).  Values are filled
    // from measured `dump-counts` output; 0 disables validation.
    {"core",                441,  0,       0      },  // scheduler+tasks+memory+syscall+sync+basic (#173): +1 checked_ptr api, +2 user-open, +4 klog/exec (#127/#134), +2 prior drift + 3 x86 ABI bridge tests (issue #30) + 2 block-arm tests (issue #212)
    {"ipc",                 81,   0,       0      },  // all ipc_* incl. fastpath + pipe_blocking (issue #173) + 2 arrival-wake gate tests (issue #208)
    {"capability",          147,  0,       0      },  // all cap_* excl. iommu_live (#173): +2 pager dispatch, +1 frame_create (#134)
    {"proc_elf",            83,   0,       0      },  // process_* + elf_* + pt_merge + libc_verify 5 (issues #173, #75)
    {"libc_verify",          5,    0,       0      },  // hosted-C Ring 3 verify (issue #75, x86_64-only)
    {"storage",             143,  0,       0      },  // all vfs_* + initrd_parser (issue #173)
    {"servers",             57,   0,       0      },  // servers_* + services_framework (#173): vfsd_auth grew 5->19 (#134), +1 daemon rejection (#135)
    {"drivers",             94,   0,       0      },  // drivers_* + virtio_blk_req + ahci_deep + net (issue #173)
    {"hal",                 121,    0,       0      },  // hal_* + exc_table(4, +1 exception_name #131) + acpi + arch_cross + irq_early_std (issues #173, #198, #199)
    {"smp",                 81,   0,       0      },  // single-CPU smp/lapic/ioapic/cache/pcid/tlb (issue #173)
    {"smp_multicpu",        18,   0,       0      },  // smp_bringup + smp_sched + drain-spare/remote-refuse/quiesce-nesting (issues #173, #197)
    {"deadline",            125,  0,       0      },  // wcet/deadline/timing/hrt/servers + posix_time (issues #173, #76)
    {"ui",                  68,   0,       0      },  // shell_* + framebuffer + debug_dump + sampler (issue #173)
    {"logging_debug",       29,   0,       0      },  // dmesg + klog + debug + gcov (issue #173)
    {"random",              17,   0,       0      },  // random_* (issue #173)
    {"bench",               22,   0,       0      },  // bench_* + bench_wcet_memory, TF_BENCH-only (issue #173)
    {"syscall_affinity",    4,    0,       0      },  // SET/GET affinity syscalls, rescued (issue #61, wired up #173)
    {"tls",                 12,   12,      12     },  // TLS_SET + publish/apply (issue #74)
};

static constexpr size_t k_expected_count_size =
    sizeof(k_expected_counts) / sizeof(k_expected_counts[0]);

inline size_t arch_count(const ExpectedCounts &ec) {
#if defined(CONFIG_ARCH_X86_64)
    return ec.x86_64;
#elif defined(CONFIG_ARCH_AARCH64)
    return ec.aarch64;
#elif defined(CONFIG_ARCH_RISCV64)
    return ec.riscv64;
#else
    return ec.x86_64;
#endif
}

inline size_t expected_for_class(const char *name) {
    for (size_t i = 0; i < k_expected_count_size; ++i) {
        if (__builtin_strcmp(name, k_expected_counts[i].name) == 0) {
            return arch_count(k_expected_counts[i]);
        }
    }
    return 0;
}

inline bool validate_class_count(const char *name, size_t actual_count) {
    size_t expected = expected_for_class(name);
    if (expected == 0) {
        return true;
    }
    if (actual_count != expected) {
        Logger::warn("[TCOUNT] MISMATCH class=%s expected=%u actual=%u "
                     " -- update test_expected_counts.hpp",
                     name, (unsigned)expected, (unsigned)actual_count);
        return false;
    }
    return true;
}

inline void validate_all_consistency() {
    // Issue #173: no `all` class anymore.  Every fine-grained class must
    // be covered by at least one aggregate or by a scripted standalone
    // special; `safe`/`selftest` are curated overlapping subsets and the
    // foreign-arch classes are covered off-arch, so all four groups are
    // excluded from the fine side of the comparison.  Intentional overlap
    // is allowed and reported: hal_apic + apic_tpr are members of both
    // `hal` and `smp` (HAL devices AND SMP coordination primitives), and
    // `testrunner` names both a fine class and its aggregate.
    static constexpr const char *k_aggregates[] = {
        "core",         "ipc",      "capability", "proc_elf",
        "storage",      "servers",  "drivers",    "hal",
        "smp",          "smp_multicpu", "deadline", "ui",
        "logging_debug", "random",  "bench",      "testrunner",
        nullptr,
    };
    static constexpr const char *k_specials[] = {
        "ahci_live", "iommu_live", "task_fpu", "task_tcb_log",
        nullptr,
    };
    size_t sum_covered = 0;
    size_t sum_fine = 0;
    size_t sum_singletons = 0;  // testrunner fine + specials: counted
                                // once each on the fine side
    for (size_t i = 0; i < k_expected_count_size; ++i) {
        const char *name = k_expected_counts[i].name;
        size_t c = arch_count(k_expected_counts[i]);
        if (__builtin_strcmp(name, "safe") == 0 ||
            __builtin_strcmp(name, "selftest") == 0 ||
            __builtin_strcmp(name, "arch_aarch64") == 0 ||
            __builtin_strcmp(name, "arch_riscv64") == 0) {
            continue;
        }
        bool is_covered = false;
        bool is_singleton = false;
        for (const char *const *p = k_aggregates; *p != nullptr; ++p) {
            if (__builtin_strcmp(name, *p) == 0) {
                is_covered = true;
                break;
            }
        }
        if (!is_covered) {
            for (const char *const *p = k_specials; *p != nullptr; ++p) {
                if (__builtin_strcmp(name, *p) == 0) {
                    is_covered = true;
                    is_singleton = true;
                    break;
                }
            }
        } else if (__builtin_strcmp(name, "testrunner") == 0) {
            is_singleton = true;
        }
        if (is_covered) {
            sum_covered += c;
        } else {
            sum_fine += c;
        }
        if (is_singleton) {
            sum_singletons += c;
        }
    }
    if (sum_covered < sum_fine + sum_singletons) {
        Logger::warn("[TCOUNT] CONSISTENCY: aggregates+specials=%u < "
                     "fine+singletons=%u -- a fine class is uncovered",
                     (unsigned)sum_covered,
                     (unsigned)(sum_fine + sum_singletons));
    } else {
        Logger::info(
            "[TCOUNT] CONSISTENCY: aggregates+specials=%u >= "
            "fine+singletons=%u (overlap=%u, incl. hal_apic+apic_tpr in "
            "hal+smp)",
            (unsigned)sum_covered, (unsigned)(sum_fine + sum_singletons),
            (unsigned)(sum_covered - sum_fine - sum_singletons));
    }
}

} // namespace kernel::test