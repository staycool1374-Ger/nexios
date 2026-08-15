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
    {"safe",                132,    0,       0      },  // curated TF_RELEASE subset
    {"selftest",            132,    0,       0      },  // same as safe
    {"testrunner",           16,    0,       0      },  // harness + freelist + infra + expected-panic (v0.3.8)
    {"all",                 929,    0,       0      },  // 927 + drivers_virtio(+2); dump_class_counts verified

    // basic
    {"basic_lib",            15,    0,       0      },  // string/utils/type-traits/ErrorOr/version
    {"basic_atomic",         12,    0,       0      },  // atomic RMW ops, litmus, acquire/release

    // configuration
    {"configuration_build",  10,    0,       0      },  // buildsystem + compile-time config sanity (v0.3.7)

    // data_structures
    {"data_structures_spsc",  8,    0,       0      },  // SPSC queue primitives
    {"data_structures_buffer_pool", 25, 0,    0      },  // alloc/free/transfer + v0.3.11 B1-B3 PT-walk regressions
    {"data_structures_buffer_pool_deterministic", 6, 0, 0},  // pre-allocated buffers, zero-copy, no alloc after init

    // synchronization
    {"synchronization_spinlock", 10, 0,      0      },  // spinlock(9) + spinlock_stress(1)
    {"synchronization_sync", 13,    0,       0      },  // semaphore/mutex/queue/eventgroup primitives
    {"synchronization_locking", 19, 0,       0      },  // locking(13) + locking_stress(6)
    {"synchronization_lock_order",  4, 0,    0      },  // nested SpinLock acquisition order
    {"synchronization_lock_validator",  6, 0, 0      },  // lock validator
    {"synchronization_irq_guard",  4, 0,    0      },  // irq_guard(3) + irqguard_audit(1)
    {"synchronization_pip",  10,    0,       0      },  // pip(7) + queue_pip(3)
    {"synchronization_pcp",   8,    0,       0      },  // pcp(5) + mutex_pcp(3)
    {"synchronization_pi_donation",  5, 0,   0      },  // PI donation mutex+semaphore

    // scheduler
    {"scheduler_core",       16,    0,       0      },  // reschedule/remove/reap/quantum/waitpid/FIFO
    {"scheduler_o1",         13,    0,       0      },  // O(1) priority map / ready queue
    {"scheduler_atomic",      6,    0,       0      },  // atomic context-switch invariants
    {"scheduler_sporadic",   25,    0,       0      },  // sporadic server scheduling policy
    {"scheduler_idle",       11,    0,       0      },  // idle_task(10) + idle_cleanup(1)
    {"scheduler_zombie",      5,    0,       0      },  // zombie_cleanup(4) + wcet_cleanup(1)
    {"scheduler_preemption", 11,    0,       0      },  // preemption(7) + preemption_under_syscall(4)
    {"scheduler_budget",      6,    0,       0      },  // task budget accounting
    {"scheduler_cpu_load",    5,    0,       0      },  // idle/CPU load metrics
    {"scheduler_starvation",  3,    0,       0      },  // SchedulerStarvation + PriorityInversionChain5 + DeadlockNestedMutexLoad

    // task
    {"task_core",             6,    0,       0      },  // TCB cleanup/page tables/clone
    {"task_lifecycle",        9,    0,       0      },  // exit/zombie/reparent
    {"task_fpu",              0,    0,       0      },  // FPU test files excluded from x86_64 build (GCC 16); reserved home
    {"task_init",             3,    0,       0      },  // init task exists/reparents
    {"task_tcb_log",          1,    0,       0      },  // TCB write-log tracer

    // syscall
    {"syscall_core",         15,    0,       0      },  // syscall interface (exit test disabled in source)
    {"syscall_fuzz",          4,    0,       0      },  // syscall fuzzing

    // process
    {"process_lifecycle",    16,    0,       0      },  // process lifecycle, child table (12 + 4 MP-1/7)
    {"process_elf",           9,    0,       0      },  // ELF loader validation/segments
    {"elf_loader",            8,    0,       0      },  // background chunked ELF loader (success/errors/cancel/cycles/yield)
    {"process_signals",       8,    0,       0      },  // signal delivery/handling
    {"process_rlimit",        5,    0,       0      },  // getrlimit/brk
    {"process_waitpid",       3,    0,       0      },  // waitpid zombie/reap
    {"process_pml4_clone",   10,    0,       0      },  // fork deep-copy page tables (7 + 3 MP-7 named)
    {"process_secure_exec",   5,    0,       0      },  // exec argv/envp validation

    // cap — capability-based access control (CSpace)
    {"cap_core",             10,    0,       0      },  // CSpace engine: CNode/CSlot lifecycle, handle decode, revoke
    {"cap_lifecycle",         8,    0,       0      },  // grant/copy/revoke/mint + Endpoint/FrameCap objects
    {"cap_syscall",           8,    0,       0      },  // SYS_CAP_GRANT/COPY/REVOKE/MINT dispatch
    {"cap_ipc",               6,    0,       0      },  // cap-gated IPC + frame mapping

    // ipc
    {"ipc_core",             23,    0,       0      },  // queue/priority/notify/eventgroup/sync roundtrip
    {"ipc_blocking",          4,    0,       0      },  // IPC blocking send_sync/handshake tests
    {"ipc_extended",          9,    0,       0      },  // size limits, mid-queue removal, timeout, inversion
    {"ipc_lock_free",         3,    0,       0      },  // lock-free queue
    {"ipc_robustness",        6,    0,       0      },  // misformed/wraparound/concurrent/cleanup
    {"ipc_pipe",              6,    0,       0      },  // kernel pipe object

    // vfs
    {"vfs_core",             20,    0,       0      },  // fdtable/resolve/mount/mkdir/unlink
    {"vfs_tmpfs",            10,    0,       0      },  // tmpfs(6) + invalid_mount(2) + mount_unmount_failure(2)
    {"vfs_fstab",             5,    0,       0      },  // fstab parsing
    {"vfs_fat32",            40,    0,       0      },  // FAT32 fs unit tests
    {"vfs_fat32_integration", 14,    0,       0      },  // VFS-on-FAT32

    // servers
    {"servers_vfsd",         18,    0,       0      },  // VFS daemon kernel-bypass ops/auth (crash-restart tests disabled in source)
    {"servers_vfsd_auth",     5,    0,       0      },  // VFS daemon authorization
    {"servers_iocd",          7,    0,       0      },  // IOCD daemon boots/IRQ/MMIO/affinity (crash-restart disabled in source)
    {"servers_daemon_restart", 0,    0,       0      },  // daemon-restart crash test #if 0-disabled in source; reserved home
    {"servers_health",        5,    0,       0      },  // SYS_HEALTH_STATUS metrics/procfs

    // memory
    {"memory_pmm",            5,    0,       0      },  // PMM alloc/free unit tests (hosts 0-test delegate)
    {"memory_mempool",        4,    0,       0      },  // MemPool allocator tests
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
    {"memory_kernel_isolation", 4,  0,       0      },  // v0.4.0 MP-1 private kernel-half PML4s
    {"memory_isolation",      3,    0,       0      },  // v0.4.0 MP-5 cross-task / HHDM / guard-page proof
    {"memory_vmm",           10,    0,       0      },  // VMM map/unmap/clone/huge-page/hhdm

    // wcet / deadline
    {"wcet_overrun",          2,    0,       0      },  // WcetOverrunDetectionFires + DeadlineMissWithinWcet
    {"wcet_scheduler",        1,    0,       0      },  // WCET benchmark for scan_deadlines (P7b)
    {"bench_wcet_memory",    2,    0,       0      },  // WCET mempool/vmm (TF_BENCH)
#if CONFIG_DEADLINE_MONITOR_TASK
    {"deadline_miss",         5,    0,       0      },  // + DeadlineMonitorTaskSpawned + DeadlineMonitorDetectsMiss
#else
    {"deadline_miss",         3,    0,       0      },  // DeadlineMissWhileBlocked + DeadlineMissWhileTerminatedSkipped + DeadlineRearmOnPeriodRollover
#endif
    {"deadline_recovery",     4,    0,       0      },  // DeadlineActionKillCleansUp + DeadlineDetectionMagicCheck + DeadlineDetectionMcdcCoverage + DeadlineActionNotifyMonitor
    {"deadline_action",       1,    0,       0      },  // single action-dispatch test per build (CONFIG_DEADLINE_ACTION)
    {"deadline_ss",           2,    0,       0      },  // SsExhaustionTriggersDeadline + SsDeadlineMissDuringReplenish

    // timing
    {"timing_core",          18,    0,       0      },  // tick accounting, alarm, rate-monotonic, deadline list

    // hal
    {"hal_core",             14,    0,       0      },  // HAL page tables/context/interrupts/timers/io/cpuid
    {"hal_bits",             14,    0,       0      },  // bit-manipulation utilities
    {"hal_idt",               6,    0,       0      },  // IDT entries/handlers/IST
    {"hal_timer",             5,    0,       0      },  // PIT/timer subsystem
    {"hal_apic",              3,    0,       0      },  // APIC timer tick rate, one-shot, stop
    {"hal_rtc",               2,    0,       0      },  // RTC read/BCD

    // drivers
    {"drivers_core",          6,    0,       0      },  // driver registry, IOCD boots, keyboard/serial, MMIO caps
    {"drivers_block",        11,    0,       0      },  // block device, ATA_PIO, AHCI
    {"drivers_pci",          16,    0,       0      },  // PCI enumeration/MSI/BARs (bounded-time test commented out)
    {"drivers_virtio",       11,    0,       0      },  // VirtIO probe/feature/queue + FLAW-03 net lock
    {"drivers_dma",          19,    0,       0      },  // DMA buffer/SG/PRD + FLAW-01/02 engine locking

    // network
    {"network_core",          5,    0,       0      },  // MAC/IPv4/ARP/checksum

    // shell / ui
    {"shell_interaction",    18,    0,       0      },  // shell commands
    {"shell_redirect",        3,    0,       0      },  // shell I/O redirection
    {"shell_textutils",       1,    0,       0      },  // text utilities
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
    {"arch_cross",           21,    0,       0      },  // cross-architecture tests (16 + 2 SMEP-gated + 3 SMAP-gated, x86_64 only)
#if defined(CONFIG_ARCH_AARCH64)
    {"arch_aarch64",          0,    0,       0      },
#endif
#if defined(CONFIG_ARCH_RISCV64)
    {"arch_riscv64",          0,    0,       0      },
#endif

    // bench
    {"bench_ipc",             7,    0,       0      },  // IPC throughput (TF_BENCH)
    {"bench_syscall",         3,    0,       0      },  // syscall latency (TF_BENCH)
    {"bench_irq",             3,    0,       0      },  // IRQ latency histogram (TF_BENCH)
    {"bench_jitter",          2,    0,       0      },  // schedule-to-schedule jitter
    {"bench_microkernel",     5,    0,       0      },  // minimal privileged surface / isolation / jitter / drift
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
    size_t all_count = 0;
    size_t sum_individual = 0;
    for (size_t i = 0; i < k_expected_count_size; ++i) {
        size_t c = arch_count(k_expected_counts[i]);
        if (__builtin_strcmp(k_expected_counts[i].name, "all") == 0) {
            all_count = c;
        } else if (__builtin_strcmp(k_expected_counts[i].name, "safe") != 0) {
            sum_individual += c;
        }
    }
    if (all_count > 0 && sum_individual < all_count) {
        Logger::warn("[TCOUNT] CONSISTENCY: sum(individual)=%u < all=%u -- "
                     "some tests missing from individual class entries",
                     (unsigned)sum_individual, (unsigned)all_count);
    } else if (all_count > 0) {
        Logger::info(
            "[TCOUNT] CONSISTENCY: sum(individual)=%u >= all=%u (overlap=%u)",
            (unsigned)sum_individual, (unsigned)all_count,
            (unsigned)(sum_individual - all_count));
    }
}

} // namespace kernel::test