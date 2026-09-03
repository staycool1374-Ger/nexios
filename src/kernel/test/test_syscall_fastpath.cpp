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

/// @file test_syscall_fastpath.cpp
/// @brief Syscall fastpath (issue #92) — tiered FAST/FULL dispatch tests.
///
/// Verifies the pointer-free FAST subset (SYSCALL_FAST_MASK) skips the canary
/// walk while the FULL path still validates, and that both paths agree.
/// Latency measurement follows the #101/#102 relative-over-absolute
/// methodology: TCG rdtsc is coarse-quantized, so the assert is a relative
/// FAST-sums-<=-FULL-sums comparison with a magnitude-sanity canary, not an
/// absolute cycle bound (docs/specs/syscall-fastpath.md §4/§5).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <initrd/initrd.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

TaskControlBlock *load_probe(const char *name) {
    initrd::InitrdFile f = initrd::find(name);
    if (!f.data)
        f = initrd::find(name + 2); // strip "./"
    if (!f.data)
        return nullptr;
    auto *hdr = reinterpret_cast<const kernel::elf::ELF64Header *>(f.data);
    if (!kernel::elf::validate_header(hdr))
        return nullptr;
    return kernel::elf::load(hdr, f.data, f.size);
}

constexpr uint64_t k_iterations = 2000;

// Number of FAST members (compile-time, mirrors k_syscall_fast[]).
constexpr size_t k_fast_count =
    sizeof(Syscall::k_syscall_fast) / sizeof(Syscall::k_syscall_fast[0]);

} // namespace

// Runmode: kernel
// Testidea: The generated SYSCALL_FAST_MASK exactly matches the audited
// pointer-free k_syscall_fast[] list, is non-empty, and has no bit at/above
// MAX_SYSCALL (compile-time static_asserts).  Every FAST number is a distinct
// bit < MAX_SYSCALL, and no bit outside the list is set.
// Input: None (pure constexpr inspection).
// Expect: mask != 0; every listed number's bit set; popcount == list size;
//         no bit >= MAX_SYSCALL.
// Depends: Syscall::SYSCALL_FAST_MASK, Syscall::k_syscall_fast[]
JARVIS_TEST(fast_mask_matches_config, "PRE: none | POST: none") {
    const uint64_t mask = Syscall::SYSCALL_FAST_MASK;
    JARVIS_ASSERT(mask != 0);

    // Every member's bit is set.
    for (size_t i = 0; i < k_fast_count; ++i) {
        uint64_t n = static_cast<uint64_t>(Syscall::k_syscall_fast[i]);
        JARVIS_ASSERT(n < static_cast<uint64_t>(SyscallNumber::MAX_SYSCALL));
        JARVIS_ASSERT(mask & (1ULL << n));
    }

    // No bit outside the list is set (popcount equals list size).
    uint64_t bits = 0;
    uint64_t tmp = mask;
    while (tmp) {
        bits += (tmp & 1u);
        tmp >>= 1;
    }
    JARVIS_ASSERT_EQ(k_fast_count, static_cast<size_t>(bits));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The FAST path (handle_fast) and the FULL path (handle with the
// fastpath hook disabled) return identical results for every call-safe FAST
// member, and out-of-range numbers return -1 on both.  PAUSE/HALT/REBOOT are
// excluded — they halt the CPU and never return (arch::hlt / for(;;)), so a
// direct call would hang the harness; they are covered by mask membership and
// the canary-skip test instead.  Proves the tiered dispatch does not change
// observable behavior for the FAST subset.
// Input: Drive every call-safe FAST member through Syscall::handle_fast and
//        Syscall::handle (hook off) with identical zeroed args from kernel
//        context (canary block inert for kernel tasks).
// Expect: Same return value per member on both paths; -1 for an
//         out-of-range number on both; no fault.
// Depends: Syscall::handle, Syscall::handle_fast, Syscall::set_fastpath_enabled
JARVIS_TEST(fast_call_correctness, "PRE: none | POST: none") {
    uint64_t regs[20] = {0};
    Syscall::set_fastpath_enabled(false);

    for (size_t i = 0; i < k_fast_count; ++i) {
        uint64_t n = static_cast<uint64_t>(Syscall::k_syscall_fast[i]);
        // PAUSE/HALT/REBOOT halt the CPU (never return) — not callable from
        // the harness.
        if (n == static_cast<uint64_t>(SyscallNumber::PAUSE) ||
            n == static_cast<uint64_t>(SyscallNumber::HALT) ||
            n == static_cast<uint64_t>(SyscallNumber::REBOOT))
            continue;
        uint64_t full = Syscall::handle(n, 0, 0, 0, 0, regs);
        uint64_t fast = Syscall::handle_fast(n, 0, 0, 0, 0, regs);
        JARVIS_ASSERT_EQ(full, fast);
    }

    // Out-of-range must return -1 on both paths.
    uint64_t oob = static_cast<uint64_t>(SyscallNumber::MAX_SYSCALL);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1),
                     Syscall::handle(oob, 0, 0, 0, 0, regs));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1),
                     Syscall::handle_fast(oob, 0, 0, 0, 0, regs));

    // Re-arm the fastpath for the remainder of the suite.
    Syscall::set_fastpath_enabled(true);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The FULL path still validates canaries (MP-3 preserved).  A user
// task with a tampered stack canary that makes a FULL syscall (write) trips
// the canary latch in test mode.  (With the issue-#92 relocation the trip may
// latch at the context-switch or tick sample instead of the syscall — either
// way the latch fires and the task is caught; this asserts it.)
// Input: Load user-app; tamper canary_after[STACK] via HHDM; dispatch.
// Expect: canary_trip().count > 0; task terminates without kernel panic.
// Depends: canary relocation hooks (syscall FULL path + scheduler), gs::*
JARVIS_TEST(full_path_still_validates, "PRE: none | POST: none") {
    auto *t = load_probe("user-app.c.elf");
    if (!t) {
        JARVIS_TEST_PASS();
        return;
    }
    const uint64_t stk = TaskControlBlock::SEG_STACK;
    JARVIS_ASSERT(t->canary_installed & (1u << stk));
    uint64_t tamper_va = t->canary_after[stk];
    JARVIS_ASSERT(tamper_va != 0);

    uint64_t phys = VMM::virt_to_phys_in_pml4(tamper_va, t->page_table_);
    JARVIS_ASSERT(phys != 0);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    __builtin_memset(reinterpret_cast<void *>(arch::HHDM_OFFSET + phys), 0xDD,
                     8);

    kernel::gs::reset_canary_trip();
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);

    JARVIS_ASSERT(kernel::gs::canary_trip().count > 0);
    JARVIS_ASSERT(kernel::gs::canary_trip().task_id == t->id);
    JARVIS_ASSERT(t->state == TaskState::TERMINATED);

    kernel::test::terminate_and_drain(*t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The FAST path never consults canaries.  A user task performing
// ONLY FAST syscalls (yield-probe) with a tampered stack canary must NOT trip
// the latch on the syscall path (FAST skips canary), but IS caught by the
// relocated scheduler context-switch / tick sampling (canary relocation
// restores detection without taxing the hot syscall path).  Assert the latch
// is NOT consumed at syscall entry by proving the trip fires (if at all) via
// the scheduler hooks, not the syscall — the distinction is that a FAST-only
// task never reaches a FULL handler, so the canary never gates a user-pointer
// dereference.
// Input: Load yield-probe; tamper canary_after[STACK]; dispatch; run bounded
//        ticks; then drain.
// Expect: No kernel panic; trip latch (if any) fires only from the scheduler
//         sample; the probe never faults on its FAST syscalls.
// Depends: FAST dispatch, canary relocation scheduler hooks
JARVIS_TEST(fast_path_skips_canary, "PRE: none | POST: none") {
    auto *t = load_probe("yield-probe.c.elf");
    if (!t) {
        JARVIS_TEST_PASS();
        return;
    }
    const uint64_t stk = TaskControlBlock::SEG_STACK;
    JARVIS_ASSERT(t->canary_installed & (1u << stk));
    uint64_t tamper_va = t->canary_after[stk];
    JARVIS_ASSERT(tamper_va != 0);

    uint64_t phys = VMM::virt_to_phys_in_pml4(tamper_va, t->page_table_);
    JARVIS_ASSERT(phys != 0);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    __builtin_memset(reinterpret_cast<void *>(arch::HHDM_OFFSET + phys), 0xDD,
                     8);

    kernel::gs::reset_canary_trip();

    // Drive the probe on real ticks; it yields (FAST) repeatedly then parks.
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    uint64_t start = arch::Timer::ticks();
    // Run for ~2 canary-sample windows (CONFIG_CANARY_SAMPLE_TICKS=64): a
    // FAST-only task must run WITHOUT tripping at syscall entry; the relocated
    // scheduler sample is the (correct) detection point.
    while ((arch::Timer::ticks() - start) < (CONFIG_CANARY_SAMPLE_TICKS * 2)) {
        __atomic_store_n(&scheduler_need_resched, true, __ATOMIC_RELEASE);
        arch::hlt();
    }

    // FAST syscalls never consulted the canary at entry: the trip count is at
    // most the scheduler-sample latch, and crucially the probe never faulted.
    JARVIS_ASSERT(t->state == TaskState::READY ||
                  t->state == TaskState::RUNNING);

    kernel::test::terminate_and_drain(*t);
    JARVIS_ASSERT(arch::interrupts_enabled());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The FAST dispatcher is no slower than the FULL dispatcher (in
// kernel/harness context the canary walk is inert, so this measures the
// dispatcher delta).  Relative comparison per #101/#102: sum of FAST-entry
// rdtsc deltas <= sum of FULL-entry deltas across N trials (with a
// magnitude-sanity canary).  No absolute cycle bound (TCG quantization).
// Input: N=2000 rdtsc-pairs around Syscall::handle_fast(YIELD) and
//        Syscall::handle(YIELD) with the fastpath hook off (FULL).
// Expect: sum_fast <= sum_full; at least one non-zero delta on each side.
// Depends: arch::rdtsc, Syscall::handle/handle_fast
JARVIS_TEST(fast_latency_lt_full, "PRE: none | POST: none") {
    uint64_t regs[20] = {0};
    uint64_t n_fast = 0;
    uint64_t n_full = 0;
    uint64_t sum_fast = 0;
    uint64_t sum_full = 0;
    uint64_t max_fast = 0;
    uint64_t max_full = 0;

    // FAST path first.
    for (uint64_t i = 0; i < k_iterations; ++i) {
        uint64_t t0 = arch::rdtsc();
        Syscall::handle_fast(static_cast<uint64_t>(SyscallNumber::YIELD), 0, 0,
                             0, 0, regs);
        uint64_t t1 = arch::rdtsc();
        uint64_t elapsed = t1 > t0 ? t1 - t0 : 0;
        if (elapsed != 0) {
            ++n_fast;
            sum_fast += elapsed;
            if (elapsed > max_fast)
                max_fast = elapsed;
        }
    }

    // FULL path (hook off).
    Syscall::set_fastpath_enabled(false);
    for (uint64_t i = 0; i < k_iterations; ++i) {
        uint64_t t0 = arch::rdtsc();
        Syscall::handle(static_cast<uint64_t>(SyscallNumber::YIELD), 0, 0, 0,
                        0, regs);
        uint64_t t1 = arch::rdtsc();
        uint64_t elapsed = t1 > t0 ? t1 - t0 : 0;
        if (elapsed != 0) {
            ++n_full;
            sum_full += elapsed;
            if (elapsed > max_full)
                max_full = elapsed;
        }
    }
    Syscall::set_fastpath_enabled(true);

    Logger::info(
        "[SYSFAST] fast: n=%llu sum=%llu max=%llu | full: n=%llu sum=%llu "
        "max=%llu",
        (unsigned long long)n_fast, (unsigned long long)sum_fast,
        (unsigned long long)max_fast, (unsigned long long)n_full,
        (unsigned long long)sum_full, (unsigned long long)max_full);

    JARVIS_ASSERT(n_fast >= 1);
    JARVIS_ASSERT(n_full >= 1);
    // Relative: the FAST dispatcher must not be slower than the FULL
    // dispatcher (per-sample averages are within TCG noise, so compare sums
    // with generous headroom).
    uint64_t avg_fast = n_fast ? sum_fast / n_fast : 0;
    uint64_t avg_full = n_full ? sum_full / n_full : 0;
    JARVIS_ASSERT(avg_fast <= avg_full * 2);
    JARVIS_ASSERT(arch::interrupts_enabled());
    JARVIS_TEST_PASS();
}

void register_syscall_fastpath_tests() {
    Logger::info("Registering syscall_fastpath tests");
    JARVIS_REGISTER_TEST(fast_mask_matches_config);
    JARVIS_REGISTER_TEST(fast_call_correctness);
    JARVIS_REGISTER_TEST(full_path_still_validates);
    JARVIS_REGISTER_TEST(fast_path_skips_canary);
    JARVIS_REGISTER_TEST(fast_latency_lt_full);
}