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
    {"buffer_pool",          25,    0,       0      },  // alloc/free/transfer + v0.3.11 B1-B3 PT-walk regressions
    {"all",                 810,    0,       0      },  // +3 v0.3.11 buffer_pool B1-B3, -2 vacuous stubs, -1 iocd stub
    {"dmesg",                15,    0,       0      },  // DmesgBuffer + error strings + suppression  (daemon_restart_crash gated, T0-7)
    {"scheduler",            62,    0,       0      },  // sched + task + lifecycle + idle_task + zombie_cleanup + health + cpu_load + preemption(7)
    {"deadlock",              1,    0,       0      },  // starvation_deadlock (detect/recovery stubs deleted, T3-1)
    {"lock_protocol",        53,    0,       0      },  // lock_order + budget + pip + pcp + queue_pip + mutex_pcp + lock_validator + locking(13) + locking_stress(6)
    {"timer",                 5,    0,       0      },  // timer tests
    {"wfg",                   0,    0,       0      },  // wfg tests
    {"lock",                  0,    0,       0      },  // (mlock stubs deleted, T3-1)
    {"memory",              48,     0,       0      },  // composite: pmm(5) + mempool(4) + slab_reclaim(5) + checked_ptr(4) + buffer_pool(25) + resource_exhaustion(5)
    {"memory_determinism",   4,     0,       0      },  // PMM exhaustion cycle + no-dynamic-alloc neutral cycle (v0.3.8)
    {"ipc",                 55,     0,       0      },  // IPC + pipe + ipc_blocking + lock-free + robustness + ipc_extended(9)
    {"ipc_blocking",         4,     0,       0      },  // IPC blocking send_sync/handshake tests
    {"zombie_cleanup",       4,     0,       0      },  // zombie list deferred cleanup
    {"vfs",                 131,    0,       0      },  // vfs + tmpfs + fat32 + block + fstab + sync + vfsd + iocd (iocd mmio stub removed, T3-1)
    {"process",             43,     0,       0      },  // process + elf + signals + rlimit + waitpid + pml4_clone
    {"syscall",             28,     0,       0      },  // syscall + syscall_fuzz
    {"arch",                31,     0,       0      },  // cross_arch + GDT + IDT + bootparams + multiboot + address + PIC + HAL
    {"cross_arch",          16,     0,       0      },  // cross-architecture tests
    {"vmm",                 11,     0,       0      },  // VMM unit tests (7 original + 2 unconditional + 2 x86_64 = 11 on x86_64)
    {"pmm",                  5,     0,       0      },  // PMM alloc/free unit tests
    {"mempool",              4,     0,       0      },  // MemPool allocator tests
    {"slab_reclaim",         5,     0,       0      },  // Slab reclaim tests
    {"checked_ptr",          4,     0,       0      },  // Checked pointer + signal frame tests
    {"resource_exhaustion",  5,     0,       0      },  // FdTable, TaskLimit, MaxBuffers, MempoolFrag, PmmExhaustion
    {"device",              22,     0,       0      },  // spsc + irq_guard + framebuffer + rtc + driver (serial/keyboard stubs deleted, T3-1)
    {"shell",               22,     0,       0      },  // shell_interaction + shell_redirect + textutils
    {"net",                 42,     0,       0      },  // net + PCI + virtio + DMA
    {"security",            10,     0,       0      },  // secure_exec + vfsd_authorization (capability stubs deleted, T3-1)
    {"debug",               14,     0,       0      },  // debug + gcov + klog
    {"integration",          0,     0,       0      },  // integration smoke tests
    {"starvation_deadlock",  4,     0,       0      },  // SchedulerStarvation + PriorityInversionChain5 + DeadlockNestedMutexLoad + DeadlockRecoveryResourceReclamation
#if CONFIG_DEADLINE_MONITOR_TASK
    {"deadline_miss",        5,     0,       0      },  // + DeadlineMonitorTaskSpawned + DeadlineMonitorDetectsMiss
#else
    {"deadline_miss",        3,     0,       0      },  // DeadlineMissWhileBlocked + DeadlineMissWhileTerminatedSkipped + DeadlineRearmOnPeriodRollover
#endif
    {"wcet_overrun",         2,     0,       0      },  // WcetOverrunDetectionFires + DeadlineMissWithinWcet
    {"ss_deadline",          2,     0,       0      },  // SsExhaustionTriggersDeadline + SsDeadlineMissDuringReplenish
    {"deadline_recovery",    4,     0,       0      },  // DeadlineActionKillCleansUp + DeadlineDetectionMagicCheck + DeadlineDetectionMcdcCoverage + DeadlineActionNotifyMonitor
    {"deadline_action",      1,     0,       0      },  // single action-dispatch test per build (CONFIG_DEADLINE_ACTION)
    {"wcet",                 1,     0,       0      },  // WCET benchmark for scan_deadlines (P7b)
    {"priority_inheritance", 11,    0,       0      },  // MutexPriorityDonates + MutexChainPropagates + MutexPriStepDown + MutexNestedDrop + SemaphoreInherits + queue_pip(3) + mutex_pcp(3)
    {"preemption_under_syscall", 4,  0,       0      },  // preemption during syscall, tmpfs write, brk, starvation
    {"stress",               3,     0,       0      },  // starvation_deadlock (stress stubs deleted, T3-1)
    {"init",                 3,     0,       0      },  // init tests
    {"build",               10,     0,       0      },  // buildsystem + config checks (v0.3.7)
    {"bench",               25,     0,       0      },  // IPC + microkernel + syscall + IRQ latency + APIC timer + jitter + wcet_memory (v0.3.8)
    {"bench_irq_latency",   3,      0,       0      },  // IRQ latency histogram tests
    {"sporadic",            25,     0,       0      },  // sporadic server tests
    {"atomic",              12,     0,       0      },  // atomic operation tests
    {"apic_timer",           3,     0,       0      },  // APIC timer tick rate, one-shot, stop
    {"irq_alloc",            0,     0,       0      },  // (IRQ-alloc stubs deleted, T3-1)
    {"jitter",               2,     0,       0      },  // Schedule-to-schedule jitter benchmarks
    {"threaded_irq",         0,     0,       0      },  // (threaded-irq stubs deleted, T3-1)
    {"gic",                  0,     0,       0      },  // (GIC stubs deleted, T3-1)
    {"plic",                 0,     0,       0      },  // (PLIC stubs deleted, T3-1)
#if CONFIG_STATIC_POOLS_ONLY
    {"static_pools",          6,     0,       0      },  // CONFIG_STATIC_POOLS_ONLY, MemPool::reserve
#else
    {"static_pools",          4,     0,       0      },  // MemPool::reserve only (no PMM gating without CONFIG_STATIC_POOLS_ONLY)
#endif
    {"stack_profiler",        6,     0,       0      },  // kernel stack depth profiling
    {"stack_alloc",           8,     0,       0      },  // stack allocation, guard pages, overflow hook
    {"page_tables",           9,     0,       0      },  // page-table pool, budget, no sharing
    {"buffer_pool_deterministic", 6, 0,       0      },  // pre-allocated buffers, zero-copy, no alloc after init
    {"no_op_new",             6,     0,       0      },  // no operator new/delete, all MemPool / placement-new
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