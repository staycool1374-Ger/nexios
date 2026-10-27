#pragma once

/// @file test_expected_counts.hpp
/// @brief Expected test-case count tables per architecture.
///
/// Trailing comments are short topic descriptors (what each class
/// covers), not changelogs: count history lives in git log and the
/// linked issues. Keep each to one line, no issue numbers.

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
    {"safe",                157,    0,       0      },  // curated debug/release smoke subset
    {"selftest",            157,    0,       0      },  // same as safe
    {"testrunner",           16,    0,       0      },  // harness, freelist, infra, expected-panic

    // basic
    {"basic_lib",            26,    0,       0      },  // string, utils, type traits, ErrorOr, version, CRC/FDT/term_fmt
    {"basic_atomic",         12,    0,       0      },  // atomic RMW ops, litmus, acquire/release

    // configuration
    {"configuration_build",  10,   8,       8      },  // build system + config sanity (CPUID/CR3 parts x86-only)
    {"release_build",         2,    0,       0      },  // actual -O2 release-build contract (TF_RELEASE)

    // data_structures
    {"data_structures_spsc",  8,    0,       0      },  // SPSC queue primitives
    {"data_structures_buffer_pool", 25, 0,    0      },  // buffer pool alloc/free/transfer, PT-walk regressions
    {"data_structures_buffer_pool_deterministic", 6, 0, 0},  // pre-allocated buffers, zero-copy, no post-init alloc

    // synchronization
    {"synchronization_spinlock", 10, 0,      0      },  // spinlock + stress
    {"synchronization_sync", 20,    0,       0      },  // semaphore, mutex, queue, event group, block pattern, latch
    {"synchronization_locking", 19, 0,       0      },  // locking + stress
    {"synchronization_lock_order",  4, 0,    0      },  // nested spinlock acquisition order
    {"synchronization_lock_validator",  6, 0, 0      },  // lock validator
    {"synchronization_irq_guard",  4, 0,      0      },  // irq guard + audit
    {"synchronization_pip",  10,    0,       0      },  // pip + queue_pip
    {"synchronization_pcp",   8,    0,       0      },  // pcp + mutex_pcp
    {"synchronization_pi_donation",  5, 0,   0      },  // PI donation mutex + semaphore

    // scheduler
    {"scheduler_core",       21,    21,      21     },  // reschedule/remove/reap/quantum/waitpid/FIFO/loadavg/rescue/evict/breaker
    {"scheduler_o1",         13,    0,       0      },  // O(1) priority map / ready queue
    {"scheduler_atomic",      6,    0,       0      },  // atomic context-switch invariants
    {"scheduler_sporadic",   25,    0,       0      },  // sporadic server policy
    {"scheduler_idle",       40,    0,       0      },  // idle task, cleanup, monitor, mem accounting
    {"scheduler_zombie",      5,    5,       5      },  // zombie cleanup + wcet cleanup
    {"scheduler_preemption", 13,    0,       0      },  // preemption, under-syscall, block-arm
    {"scheduler_budget",      6,    0,       0      },  // task budget accounting
    {"scheduler_cpu_load",    5,    0,       0      },  // idle/CPU load metrics
    {"scheduler_starvation",  3,    0,       0      },  // starvation, inversion chain, nested-mutex deadlock load
    {"scheduler_hrt",         4,    0,       0      },  // hard-RT dispatch: rdtsc canary, wake latency, jitter

    // task
    {"task_core",             7,    7,       7      },  // TCB cleanup, page tables, clone with real user parents
    {"task_lifecycle",        9,    0,       0      },  // exit, zombie, reparent
    {"task_fpu",              0,    7,       7      },  // FPU context switch/clone/user isolation (off-x86 only)
    {"task_init",             5,    0,       0      },  // init task exists, reparent, reaper notify/IPC wake
    {"task_tcb_log",          1,    0,       0      },  // TCB write-log tracer
    {"fpu_invariants",        5,    0,       0      },  // FPU/SIMD context invariants

    // syscall
    {"syscall_core",         33,    30,      30     },  // syscall interface, user-task probes, affinity, error strings, ABI
    {"syscall_fuzz",          4,    0,       0      },  // syscall fuzzing
    {"syscall_fastpath",      5,    0,       0      },  // tiered FAST/FULL dispatch
    {"debug_syscall",         29,   29,      29     },  // debugger syscalls: attach/codec/park/mem/grants/launch/traps

    // process
    {"process_lifecycle",    16,   16,      16     },  // process lifecycle, child table, clone with real user parents
    {"process_elf",           9,    0,       0      },  // ELF validation, segments
    {"elf_loader",            25,   25,      25     },  // chunked background loader: success/errors/cancel/authenticity/decay/fork
    {"elf_shared",            9,    0,       0      },  // DT_NEEDED shared-object support
    {"process_signals",       8,    0,       0      },  // signal delivery/handling
    {"process_rlimit",        5,    0,       0      },  // getrlimit, brk
    {"process_waitpid",       3,    0,       0      },  // waitpid zombie/reap
    {"process_pml4_clone",   13,    13,      13     },  // fork deep-copy page tables, independence walk
    {"pt_merge",              6,    6,       6      },  // kernel-half page-table merge
    {"process_secure_exec",   5,    0,       0      },  // exec argv/envp validation

    // cap — capability-based access control (CSpace)
    {"cap_core",             10,    0,       0      },  // CSpace engine: CNode/CSlot lifecycle, decode, revoke
    {"cap_lifecycle",         8,    0,       0      },  // grant, copy, revoke, mint, endpoints, frames
    {"cap_syscall",           9,    0,       0      },  // cap syscall dispatch + handle roundtrip
    {"cap_ipc",               6,    0,       0      },  // cap-gated IPC + frame mapping
    {"cap_untyped",          18,    0,       0      },  // Untyped allocator, sub-range carve, child split
    {"cap_mmio",              20,   0,       0      },  // MMIO caps, I/O delegation, revocation closure
    {"cap_mmio_user",         9,   0,       0      },  // user MMIO map/unmap syscalls + registry
    {"cap_irq",               12,   11,      11     },  // IRQ caps, user-space delivery (x86-only ACK off-x86)
    {"cap_irq_notify",        7,   6,       6      },  // IRQ to IPC Notify bridge (x86-only 7th off-x86)
    {"cap_msix",              13,   0,       0      },  // per-vector MSI-X caps + delivery
    {"cap_iommu",             12,   0,       0      },  // IOMMU DMA protection
    {"iommu_live",            6,    0,       0      },  // live VT-d enablement (q35 variant; not in all)
    {"cap_shm",               7,    0,       0      },  // cap-gated shared-memory rings
    {"cap_death",             9,    0,       0      },  // async task-death notifications
    {"cap_pager",             15,   0,       0      },  // external pager protocol
    {"ipc_fastpath",          14,   0,       0      },  // in-register IPC fastpath
    {"ipc_pipe_blocking",      6,   0,       0      },  // pipe blocking semantics
    {"vfs_procfs",             9,   0,       0      },  // procfs nodes
    {"vfs_tmpfs_corrupt",      7,   0,       0      },  // tmpfs corruption/timeout analogues
    {"vfs_devfs",             10,   0,       0      },  // devfs device nodes
    {"vfs_procfs_ops",         6,   0,       0      },  // procfs per-node op contracts
    {"vfs_initrd_fs",          9,   0,       0      },  // initrd filesystem vnodes
    {"vfs_errors",             6,   0,       0      },  // VfsError API codes
    {"hal_rtc_datetime",       6,    5,       5      },  // RTC date arithmetic (no BCD off-x86)
    {"hal_keyboard_decode",   10,   12,     13      },  // keyboard decode tables/modifiers + serial facade off-x86
    {"hal_serial_logic",      14,    9,      10     },  // UART regs/FIFO/loopback + RX ring + per-arch pump pin
    {"acpi_parse",             5,   0,       0      },  // ACPI/DMAR discovery
    {"smp_madt",               3,   0,       0      },  // ACPI MADT discovery
    {"smp_ipi",                2,   0,       0      },  // APIC IPI path
    {"smp_bringup",            5,   0,       0      },  // AP bring-up
    {"sched_affinity",         7,   0,       0      },  // CPU affinity
    {"smp_sched",              13,   0,       0      },  // AP scheduling, admission, drain
    {"lapic",                6,    0,       0      },  // local APIC
    {"ioapic",               5,    0,       0      },  // I/O APIC
    {"core_isolation",       5,    0,       0      },  // core isolation
    {"load_balancer",        4,    0,       0      },  // load balancer (stubs pending API)
    {"cache_coloring",       4,    0,       0      },  // cache-coloring allocator
    {"smp_sync",             6,    0,       0      },  // SMP sync (part stubs)
    {"smp_verify",           5,    0,       0      },  // SMP verify (part stubs)
    {"pcid",                 4,    0,       0      },  // PCID (stubs pending API)
    {"invpcid",              4,    0,       0      },  // INVPCID (stubs pending API)
    {"lazy_tlb",             4,    0,       0      },  // lazy shootdown policy
    {"ipi_batching",         4,    0,       0      },  // IPI batching
    {"tlb_latency",          4,    0,       0      },  // TLB latency profiling
    {"pml4_sync",            5,    0,       0      },  // PML4 sync (part stubs)
    {"drivers_virtio_blk_req", 13,   0,       0      },  // virtio-blk request path + completion/ISR contracts
    {"drivers_ahci_deep",      10,   0,       0      },  // AHCI protocol + completion/ISR contracts
    {"ahci_live",              5,   0,       0      },  // real AHCI path on q35 variant (not in all)
    {"initrd_parser",          9,   0,       0      },  // cpio parser

    // ipc
    {"ipc_core",             23,    0,       0      },  // queue, priority, notify, event group, sync roundtrip
    {"ipc_blocking",          4,    0,       0      },  // blocking send_sync/handshake
    {"ipc_timeout",           7,    7,       7      },  // wheel-armed bounded receive
    {"ipc_extended",          13,   0,       0      },  // size limits, removal, wake gates, quiescence, reply
    {"ipc_lock_free",         3,    0,       0      },  // lock-free queue
    {"ipc_robustness",        11,   0,       0      },  // malformed, wraparound, concurrent, cleanup, matching
    {"ipc_pipe",              6,    0,       0      },  // kernel pipe object

    // vfs
    {"vfs_core",             25,   0,       0      },  // fdtable, resolve, mount, mkdir, unlink, borrow guards, /bin
    {"vfs_tmpfs",            10,   0,       0      },  // tmpfs + invalid mounts
    {"vfs_fstab",             5,   0,       0      },  // fstab parsing
    {"vfs_fat32",            40,   0,       0      },  // FAT32 unit tests
    {"vfs_fat32_integration", 16,   0,       0      },  // VFS on FAT32, stable-ino pins

    // servers
    {"servers_vfsd",         18,   0,       0      },  // VFS daemon kernel-bypass ops/auth
    {"servers_vfsd_auth",    27,   27,      27     },  // daemon auth, dup/pipe, fs handlers, gates
    {"servers_iocd",          7,   0,       0      },  // IOCD daemon boot/IRQ/MMIO/affinity
    {"servers_daemon_restart", 2,    0,       0      },  // unknown-daemon rejection, terminate/resurrect
    {"servers_health",        5,    0,       0      },  // health status metrics/procfs

    // memory
    {"memory_pmm",            9,    0,       0      },  // PMM alloc/free, window geometry, err wrappers
    {"memory_mempool",        5,    0,       0      },  // MemPool allocator + helpers
    {"memory_slab",           5,    0,       0      },  // slab reclaim
    {"memory_safety",        16,    16,      16     },  // MemPool/PMM invariants, red zones, canaries + kslot unit (P2 unified, all arches)
    {"stack_cookie",          4,    0,       0      },  // stack protector contract pins
    {"pointer_isolation",     5,    0,       0      },  // kernel-leak rejection, count edges, factory
    {"memory_determinism",    4,    0,       0      },  // PMM exhaustion, no-dynamic-alloc cycles
    {"memory_checked_ptr",    4,    0,       0      },  // CheckedPtr + signal frames
    {"memory_resource_exhaustion", 5, 0,      0      },  // fd, task, buffer, pool exhaustion
    {"memory_stack_alloc",   12,    0,       0      },  // stack alloc, guard pages, overflow hook
    {"memory_stack_profiler", 6,    0,       0      },  // stack depth profiling
#if CONFIG_STATIC_POOLS_ONLY
    {"memory_static_pools",   6,    0,       0      },  // static pools + reserve
#else
    {"memory_static_pools",   4,    0,       0      },  // reserve only (no PMM gating)
#endif
    {"memory_no_op_new",      6,    0,       0      },  // no operator new/delete
    {"memory_page_tables",    13,   0,       0      },  // page-table pool, budget, no sharing, NX/W^X
    {"memory_kernel_isolation", 6,  0,       0      },  // private kernel-half PML4s + teardown
    {"memory_isolation",      3,    0,       0      },  // cross-task/HHDM/guard-page proof
    {"memory_vmm",           18,    15,      15     },  // VMM map/unmap/clone/huge/HHDM, cap maps, take, holes, 1G

    // wcet / deadline
    {"wcet_overrun",          2,    0,       0      },  // overrun detection + deadline miss
    {"wcet_scheduler",        2,    0,       0      },  // WCET benchmarks
    {"bench_wcet_memory",    2,    0,       0      },  // WCET mempool/vmm (TF_BENCH)
#if CONFIG_DEADLINE_MONITOR_TASK
    {"deadline_miss",         5,    0,       0      },  // monitor spawn + miss detection
#else
    {"deadline_miss",         0,    0,       0      },  // none register at MONITOR=0 (monitor-dependent)
#endif
    {"deadline_recovery",     4,    0,       0      },  // recovery, magic check, coverage, notify
    {"deadline_action",       1,    0,       0      },  // single action-dispatch test per build
    {"boot_deterministic",    3,    0,       0      },  // boot stage stubs
    {"task_watchdog",         6,    0,       0      },  // per-task watchdog
    {"watchdog_daemon",       5,    0,       0      },  // daemon supervision
    {"deadline_ss",           3,   0,       0      },  // sporadic-server deadline paths

    // timing
    {"timing_core",          19,    0,       0      },  // tick accounting, alarm, RMS, deadline list
    {"posix_time",           10,    0,       0      },  // clock/nanosleep/timer over HRT wheel

    // hal
    {"hal_core",             14,    0,       0      },  // HAL page tables, context, interrupts, timers, IO, CPUID
    {"hal_bits",             14,    0,       0      },  // bit-manipulation utilities
    {"hal_idt",               6,    0,       0      },  // IDT entries/handlers/IST
    {"exc_table",             5,    0,       0      },  // ISR classification, frame layout, routing, names
    {"hal_timer",             5,    0,       0      },  // PIT/timer subsystem
    {"irq_early_std",         7,    0,       0      },  // early IRQ+timer init conformance
    {"hal_apic",              3,    0,       0      },  // APIC timer
    {"apic_tpr",              6,    0,       0      },  // TPR classes, shadow, guard, reservation, live IPI
    {"hal_rtc",               2,    0,       0      },  // RTC read/BCD

    // drivers
    {"drivers_core",          8,    0,       0      },  // driver registry, keyboard/serial, MMIO caps, bounded waits
    {"drivers_block",        12,    0,       0      },  // block device, ATA_PIO, AHCI
    {"drivers_pci",          16,    4,       4      },  // PCI enumeration, MSI, BARs + off-x86 ECAM regression
    {"drivers_virtio",       11,    0,       0      },  // VirtIO probe, features, queues
    {"drivers_dma",          19,    0,       0      },  // DMA buffer/SG/PRD + engine locking

    // network
    {"network_core",          18,   0,       0      },  // MAC/IPv4/ARP/checksum, mock-NIC RX, ICMP/UDP, malformed rejection

    // shell / ui
    {"shell_interaction",    29,   29,      29     },  // shell commands, key-delivery seam, RX slice + URC seams
    {"shell_redirect",        3,    0,       0      },  // shell I/O redirection
    {"shell_textutils",       1,    0,       0      },  // text utilities
    {"debug_dump",            8,    0,       0      },  // diagnostic dump smoke + first-chance raw
    {"synchronization_err_api", 7, 0,       0      },  // sync err API + SPSC ring
    {"kernel_top",            22, 0,       0      },  // top-level kernel: histogram, random, IrqThread, datetime, state
    {"per_cpu",               4,    0,       0      },  // per-CPU foundation
    {"memory_checked_ptr_api", 10,  0,       0      },  // CheckedPtr/safe-copy instantiations + fault recovery
    {"memory_integrity",      3,    0,       0      },  // section markers, text CRC, stack-marker span
    {"profiler_sampler",     6,    0,       0      },  // sampling profiler API
    {"shell_commands",       34,    0,       0      },  // shell command surface
    {"services_framework",    7,    0,       0      },  // services framework: terminal, cursor, splash, scroll, registry
    {"ui_framebuffer",        6,    0,       0      },  // framebuffer init/draw/clear/scroll + render pin
    {"path_tracer",           7,    0,       0      },  // PathTracer core primitive
    {"scheduler_dispatch_path", 4, 0,      0      },  // dispatch segments
    {"task_path",             6,    0,       0      },  // task create/cleanup segments + teardown chain
    {"vmm_path",              7,    0,       0      },  // VMM map/rollback segments + injector

    // random
    {"random_core",           7,    0,       0      },  // RNG smoke/non-repeating
    {"random_seed",           2,    0,       0      },  // random seed init
    {"random_syscall",        4,    0,       0      },  // getrandom syscall
    {"random_vfs",            2,    0,       0      },  // /dev/random VFS node
    {"random_vfs_write",      2,    0,       0      },  // /dev/random VFS write

    // logging / debug
    {"logging_dmesg",        21,    0,       0      },  // DmesgBuffer, error strings, suppression, severity, taxonomy
    {"logging_klog",          9,    0,       0      },  // kernel log read/wrap/concurrent + silence gate
    {"debug_core",            2,    0,       0      },  // write formats + switch logs
    {"debug_gcov",            4,    0,       0      },  // GCOV metadata

    // arch
    {"arch_cross",           25,    0,       0      },  // cross-architecture tests (x86_64 only)
#if defined(CONFIG_ARCH_AARCH64)
    {"arch_aarch64",          0,   38,       0      },  // EL0/EL1 faults, frames, fork, PL011, barriers (aarch64 only)
#endif
#if defined(CONFIG_ARCH_RISCV64)
    {"arch_riscv64",          0,    0,      31      },  // S-mode traps, loader probes, UART, RTC (M-mode CSRs deferred)
#endif

    // bench
    {"bench_ipc",             7,    0,       0      },  // IPC throughput (TF_BENCH)
    {"bench_ipc_throughput",  4,    0,       0      },  // msgs/s + size/depth sweeps + peer crossing, TF_BENCH
    {"bench_context_switch",  1,    0,       0      },  // task-to-task p99 distribution, TF_BENCH
    {"bench_syscall",         3,    0,       0      },  // syscall latency (TF_BENCH)
    {"bench_irq",             3,    0,       0      },  // IRQ latency histogram (TF_BENCH)
    {"bench_path_tracer",     1,    0,       0      },  // path-tracer per-step overhead, TF_BENCH
    {"bench_jitter",          2,    0,       0      },  // schedule-to-schedule jitter
    {"bench_microkernel",     5,    0,       0      },  // minimal privileged surface / isolation / jitter / drift

    // hrt — hard real-time measurement
    {"stress_hrt",            2,    0,       0      },  // rdtsc canary + IPC under stress (TF_KERNEL)
    {"stress_24h",            2,    0,       0      },  // soak driver SHORT horizon + liveness
    {"hrt_monotonic",         6,    0,       0      },  // HRT monotonic clock + wall bound
    {"timer_wheel",           8,    0,       0      },  // event-timer wheel
    {"task_metering",         9,    0,       0      },  // per-task exec-ns metering
    {"sched_edf",             7,    0,       0      },  // deadline-aware preemptive scheduling
    {"sched_admission",       11,   0,       0      },  // enforced admission + fork guards
    {"sched_admission_verify", 7,   0,       0      },  // admission journeys
    {"aperiodic_servers",     7,    0,       0      },  // DS/BG server modes

    // Structural/semantic aggregates. Values are filled from measured
    // `dump-counts` output; 0 disables validation.
    // core_* — split of the former `core` aggregate. Values filled from
    // measured `dump-counts` output; 0 disables validation.
    {"core_basic",          98,   0,       0      },  // basic + data_structures + stack_cookie + pointer_isolation
    {"core_sync",           93,   0,       0      },  // synchronization_* incl. pip/pcp/donation/err_api
    {"core_sched",          112,  0,       0      },  // scheduler_*
    {"core_task",           52,   0,       0      },  // task_*, fpu_invariants, kernel_top, per_cpu
    {"core_memory",         124,  0,       0      },  // memory_* incl. NX/W^X pins
    {"core_syscall",        85,   0,       0      },  // syscall_*, debug_syscall, tls
    {"core_tracing",        25,   0,       0      },  // path_tracer + dispatch/task/vmm segments
    {"ipc",                 87,   0,       0      },  // ipc_* incl. fastpath + pipe_blocking
    {"capability",          149,  0,       0      },  // cap_* excl. iommu_live
    {"proc_elf",            131,  81,      81     },  // process_*, elf_*, pt_merge, libc_* (off-x86 native-EM)
    {"libc_verify",          8,    0,       0      },  // hosted Ring-3 verify (x86_64 only)
    {"libc_stdio",            5,   0,       0      },  // fmt/snprintf/sscanf/fprintf-to-fd/full-run (x86_64 only)
    {"libc_malloc",           5,    0,       0      },  // malloc/align/calloc/realloc/cycle (x86_64 only)
    {"libc_string",           3,    0,       0      },  // ring3 + kernel string sweep
    {"libc_thread",           4,    0,       0      },  // atomics + errno cell (x86_64 only)
    {"libc_syscall",          4,    0,       0      },  // image size, getpid, write-cross, no-corruption
    {"cross_boundary",        3,    0,       0      },  // ring3 vfsd/iocd chains + keyboard delivery (x86_64 only)
    {"storage",             167,  0,       0      },  // vfs_* + initrd_parser + ramdisk + fat32 pins + readdir overlay
    {"servers",             66,   66,      66     },  // servers_* + services_framework
    {"drivers",             107,   62,      62     },  // drivers_* + virtio_blk_req + ahci_deep + net
    {"hal",                 130,   83,      85     },  // hal_*, exc_table, acpi, arch_cross, serial/rtc/kb groups
    {"smp",                 81,   0,       0      },  // single-CPU smp/lapic/ioapic/cache/pcid/tlb
    {"smp_multicpu",        18,   0,       0      },  // smp_bringup + smp_sched + drain (2-CPU)
    {"deadline",            148,  0,       0      },  // wcet/deadline/timing/hrt/servers + posix_time
    {"ui",                  87,   87,      87     },  // shell_* + framebuffer + debug_dump + sampler
    {"logging_debug",       36,   0,       0      },  // dmesg + klog + debug + gcov
    {"random",              17,   0,       0      },  // random_*
    {"bench",               28,   0,       0      },  // bench_* (TF_BENCH)
    {"syscall_affinity",    4,    0,       0      },  // SET/GET affinity syscalls
    {"tls",                 13,   12,      12     },  // TLS publish/apply + errno cell
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
        "core_basic",   "core_sync",  "core_sched", "core_task",
        "core_memory",  "core_syscall", "core_tracing",
        "ipc",      "capability", "proc_elf",
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