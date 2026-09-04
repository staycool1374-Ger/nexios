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

/// @file test_stress_hrt.cpp
/// @brief Hard real-time measurement class (issue #101).
///
/// The class boots QEMU with `-icount shift=0,sleep=off` (see Makefile
/// QEMU_ICOUNT, class-scoped to `stress_hrt`/`hrt`), so in-kernel rdtsc
/// readings are virtualized instruction-count timestamps.  The tests here
/// verify that NexIOS meets hard time criteria under synthetic stress:
/// - `stress_hrt_rdtsc_baseline`  — rdtsc virtualization sanity canary.
/// - `stress_hrt_ipc_latency_hard_bound` — IPC send_sync roundtrip latency
///   of a high-priority RT task measured under a low-priority memory/IPC
///   hammerer, asserted against RELATIVE bounds derived from a measured
///   baseline (never bare absolute cycle constants — they vary across QEMU/
///   TCG versions).  Bound philosophy mirrors bench_jitter / syscall-fastpath:
///   stress_avg <= base_avg * K_AVG_RATIO (or ABS_FLOOR), p95 with headroom,
///   plus a PATHOLOGICAL_CAP wedge detector that only fires on a wedged
///   scheduler.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/sync/spinlock.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

// --- calibration-driven bounds (set from the calibration run, ≥3x headroom) ---
// Calibration (2026-09-03, debug TCG, no icount — see LEARNINGS):
//   rdtsc pair:  nonzero=39 avg=1051 max=3000  (TCG quanta)
//   send_sync:   base avg=3.0M p95=2.1M max=4.76M | stress avg=3.0M max=4.59M
// The stress avg tracks the base avg closely (the hard-RT property); the
// relative asserts use k_avg_ratio=8 on those measured averages.  The
// k_pathological_cap only fires on a wedged scheduler (a wedge dwarfs even a
// 10ms tick = 3.6M cycles at ~362MHz), set at ~4x the observed base max.
constexpr uint64_t k_avg_ratio = 8;
constexpr uint64_t k_abs_floor = 10000000;
constexpr uint64_t k_pathological_cap = 20000000;
constexpr uint64_t k_iterations = 2000;

// --- cross-task coordination (atomics per CODING_STYLE §11.6) ---
uint64_t g_receiver_id = 0;
uint64_t g_rt_done = 0;
uint64_t g_hammer_on = 0;
uint64_t g_base_nonzero = 0;
uint64_t g_stress_nonzero = 0;
uint64_t g_base_sum = 0;
uint64_t g_stress_sum = 0;
uint64_t g_base_max = 0;
uint64_t g_stress_max = 0;
uint64_t g_base_p95 = 0;
uint64_t g_stress_p95 = 0;
uint64_t g_base_fail = 0;
uint64_t g_stress_fail = 0;

// --- fixed 32-bucket log2 histograms (no dynamic allocation) ---
constexpr unsigned k_buckets = 32;
uint64_t g_base_hist[k_buckets] = {};
uint64_t g_stress_hist[k_buckets] = {};

uint64_t hist_bucket(uint64_t cycles) {
    unsigned b = 0;
    while (cycles > 1 && b < k_buckets - 1) {
        cycles >>= 1;
        ++b;
    }
    return b;
}

uint64_t hist_p95(uint64_t *hist, uint64_t total) {
    if (total == 0)
        return 0;
    uint64_t need = (total * 95 + 99) / 100; // ceil(0.95 * total)
    uint64_t acc = 0;
    for (unsigned b = 0; b < k_buckets; ++b) {
        acc += hist[b];
        if (acc >= need)
            return 1ULL << b;
    }
    return 1ULL << (k_buckets - 1);
}

void reset_stats() {
    __atomic_store_n(&g_receiver_id, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_rt_done, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_hammer_on, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_base_nonzero, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_stress_nonzero, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_base_sum, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_stress_sum, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_base_max, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_stress_max, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_base_p95, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_stress_p95, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_base_fail, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_stress_fail, 0, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < k_buckets; ++i) {
        g_base_hist[i] = 0;
        g_stress_hist[i] = 0;
    }
}

} // namespace

// Runmode: kernel
// Testidea: rdtsc virtualization sanity canary.  Confirms the TSC advances
// during the test window so any latency assert below is meaningful.  Under
// QEMU TCG the TSC advances in coarse quanta, so a large fraction of
// back-to-back deltas may be zero — the canary asserts the counter advances at
// all and stays within a sane magnitude, NOT that every delta is non-zero.
// Input: k_iterations rdtsc-pair deltas with interrupts disabled.
// Expect: at least one non-zero delta; avg and max far below the pathological
//         cap (a handful of quanta; generous for coarse TCG quantization).
// Depends: arch::rdtsc, arch::IrqGuard
JARVIS_TEST(stress_hrt_rdtsc_baseline, "PRE: none | POST: none") {
    uint64_t nonzero = 0;
    uint64_t sum = 0;
    uint64_t max_el = 0;
    {
        arch::IrqGuard guard;
        for (uint64_t i = 0; i < k_iterations; ++i) {
            uint64_t t0 = arch::rdtsc();
            uint64_t t1 = arch::rdtsc();
            uint64_t elapsed = t1 > t0 ? t1 - t0 : 0;
            if (elapsed != 0) {
                ++nonzero;
                sum += elapsed;
                if (elapsed > max_el)
                    max_el = elapsed;
            }
        }
    }
    uint64_t avg = nonzero ? sum / nonzero : 0;
    Logger::info("[STRESS_HRT] rdtsc baseline: nonzero=%llu avg=%llu max=%llu",
                 (unsigned long long)nonzero, (unsigned long long)avg,
                 (unsigned long long)max_el);
    JARVIS_ASSERT(nonzero >= 1);
    JARVIS_ASSERT(avg < k_abs_floor);
    JARVIS_ASSERT(max_el < k_pathological_cap);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: hard RT latency under synthetic load.  A prio-30 RT task measures
// IPC send_sync roundtrips: phase A (baseline, hammerer suspended) and phase
// B (hammerer saturating memory/IPC).  The RT latency must stay within a
// relative factor of its own baseline.  Both phases run with interrupts
// enabled (a cross-task send_sync needs the receiver dispatched); the hammerer
// is gated by an atomic flag so only phase B is perturbed.
// Input: hammerer (prio 1, net-zero alloc/IPC stress loop), receiver (prio 29,
//        N replies), RT task (prio 30, N/2 baseline + N/2 stress send_sync).
// Expect: stress avg <= base avg * k_avg_ratio (or k_abs_floor), p95 within
//         headroom, max below k_pathological_cap (scheduler-wedge detector).
// Depends: IPC::send_sync, Scheduler, MemPool, PMM, arch::rdtsc
JARVIS_TEST(stress_hrt_ipc_latency_hard_bound, "PRE: none | POST: none") {
    reset_stats();

    // Hammerer (prio 1): gated by g_hammer_on.  When on: forever-loop of
    // net-zero memory/IPC activity — every allocation is paired with its free,
    // every IPC send is a self-roundtrip released before the next.  When off:
    // idle (hlt) so phase A is unperturbed.  Exits when g_rt_done is set.
    auto *hammer = TaskControlBlock::create(
        []() {
            while (__atomic_load_n(&g_rt_done, __ATOMIC_ACQUIRE) == 0) {
                if (__atomic_load_n(&g_hammer_on, __ATOMIC_ACQUIRE) == 0) {
                    arch::hlt();
                    continue;
                }
                void *blk = MemPool::alloc(64);
                if (blk != nullptr)
                    MemPool::free(blk);
                uint64_t page = PMM::alloc_page();
                if (page != 0)
                    PMM::free_page(page);
                Message msg;
                msg.sender_id = Scheduler::current_task()->id;
                msg.type = 0xEE;
                msg.priority = 0;
                msg.data_size = 0;
                IPC::send(msg.sender_id, msg);
                Message out;
                IPC::recv(out);
                sync::SpinLock lock;
                lock.lock();
                lock.unlock();
            }
        },
        1, 10);
    JARVIS_ASSERT(hammer != nullptr);

    // Receiver (prio 29): replies to every RT request.
    auto *receiver = TaskControlBlock::create(
        []() {
            for (uint64_t i = 0; i < k_iterations; ++i) {
                Message msg;
                while (!IPC::recv(msg))
                    arch::hlt();
                Message reply;
                reply.sender_id = Scheduler::current_task()->id;
                reply.type = msg.type + 1;
                reply.priority = 0;
                reply.data_size = 0;
                if (!IPC::send(msg.sender_id, reply))
                    return;
            }
        },
        29, 10);
    JARVIS_ASSERT(receiver != nullptr);
    __atomic_store_n(&g_receiver_id, receiver->id, __ATOMIC_RELEASE);

    // RT task (prio 30): phase A baseline (hammerer off), phase B stress
    // (hammerer on), then self-terminate.  Both phases run with IF=1 so the
    // receiver is dispatched between rounds.
    auto *rt = TaskControlBlock::create(
        []() {
            uint64_t half = k_iterations / 2;
            uint64_t local_base_max = 0;
            uint64_t local_stress_max = 0;

            for (uint64_t i = 0; i < k_iterations; ++i) {
                bool stress = i >= half;
                if (stress)
                    __atomic_store_n(&g_hammer_on, 1, __ATOMIC_RELEASE);
                Message msg;
                msg.sender_id = Scheduler::current_task()->id;
                msg.type = stress ? 0x22 : 0x11;
                msg.priority = 0;
                msg.data_size = 0;
                Message reply;
                uint64_t t0 = arch::rdtsc();
                bool ok = IPC::send_sync(
                    __atomic_load_n(&g_receiver_id, __ATOMIC_ACQUIRE),
                    msg, reply);
                uint64_t t1 = arch::rdtsc();
                if (!ok) {
                    __atomic_add_fetch(&(stress ? g_stress_fail : g_base_fail),
                                       1, __ATOMIC_RELAXED);
                    continue;
                }
                uint64_t elapsed = t1 > t0 ? t1 - t0 : 0;
                if (elapsed != 0) {
                    if (stress) {
                        __atomic_add_fetch(&g_stress_nonzero, 1,
                                           __ATOMIC_RELAXED);
                        __atomic_add_fetch(&g_stress_sum, elapsed,
                                           __ATOMIC_RELAXED);
                        if (elapsed > local_stress_max)
                            local_stress_max = elapsed;
                        unsigned b = hist_bucket(elapsed);
                        __atomic_add_fetch(&g_stress_hist[b], 1,
                                           __ATOMIC_RELAXED);
                    } else {
                        __atomic_add_fetch(&g_base_nonzero, 1,
                                           __ATOMIC_RELAXED);
                        __atomic_add_fetch(&g_base_sum, elapsed,
                                           __ATOMIC_RELAXED);
                        if (elapsed > local_base_max)
                            local_base_max = elapsed;
                        unsigned b = hist_bucket(elapsed);
                        __atomic_add_fetch(&g_base_hist[b], 1,
                                           __ATOMIC_RELAXED);
                    }
                }
            }

            __atomic_store_n(&g_base_max, local_base_max, __ATOMIC_RELEASE);
            __atomic_store_n(&g_stress_max, local_stress_max,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&g_rt_done, 1, __ATOMIC_RELEASE);
        },
        30, 10);
    JARVIS_ASSERT(rt != nullptr);

    {
        arch::IrqGuard guard;
        Scheduler::add_task(*receiver);
        Scheduler::add_task(*hammer);
        Scheduler::add_task(*rt);
    }

    // Drive on real ticks.
    uint64_t start = arch::Timer::ticks();
    while (__atomic_load_n(&g_rt_done, __ATOMIC_ACQUIRE) == 0 &&
           (arch::Timer::ticks() - start) < 30000) {
        __atomic_store_n(&scheduler_need_resched, true, __ATOMIC_RELEASE);
        arch::hlt();
    }

    // Cleanup BEFORE asserts (cookbook Rule 5): RT task self-terminated; the
    // hammerer sees g_rt_done and self-terminates.
    kernel::test::wait_for_termination_safe(rt);
    Scheduler::drain_zombie_list();
    kernel::test::wait_for_termination_safe(hammer);
    Scheduler::drain_zombie_list();
    kernel::test::wait_for_termination_safe(receiver);
    Scheduler::drain_zombie_list();

    uint64_t base_nz = __atomic_load_n(&g_base_nonzero, __ATOMIC_ACQUIRE);
    uint64_t stress_nz = __atomic_load_n(&g_stress_nonzero, __ATOMIC_ACQUIRE);
    uint64_t base_sum = __atomic_load_n(&g_base_sum, __ATOMIC_ACQUIRE);
    uint64_t stress_sum = __atomic_load_n(&g_stress_sum, __ATOMIC_ACQUIRE);
    uint64_t base_avg = base_nz ? base_sum / base_nz : 0;
    uint64_t stress_avg = stress_nz ? stress_sum / stress_nz : 0;
    uint64_t base_max = __atomic_load_n(&g_base_max, __ATOMIC_ACQUIRE);
    uint64_t stress_max = __atomic_load_n(&g_stress_max, __ATOMIC_ACQUIRE);
    uint64_t base_p95 = hist_p95(g_base_hist, base_nz);
    uint64_t stress_p95 = hist_p95(g_stress_hist, stress_nz);
    uint64_t base_fail = __atomic_load_n(&g_base_fail, __ATOMIC_ACQUIRE);
    uint64_t stress_fail = __atomic_load_n(&g_stress_fail, __ATOMIC_ACQUIRE);

    Logger::info(
        "[STRESS_HRT] base: nz=%llu avg=%llu p95=%llu max=%llu | "
        "stress: nz=%llu avg=%llu p95=%llu max=%llu",
        (unsigned long long)base_nz, (unsigned long long)base_avg,
        (unsigned long long)base_p95, (unsigned long long)base_max,
        (unsigned long long)stress_nz, (unsigned long long)stress_avg,
        (unsigned long long)stress_p95, (unsigned long long)stress_max);

    JARVIS_ASSERT(base_nz >= k_iterations / 8);
    JARVIS_ASSERT(stress_nz >= k_iterations / 8);
    JARVIS_ASSERT(base_fail == 0);
    JARVIS_ASSERT(stress_fail == 0);
    JARVIS_ASSERT(stress_avg <= base_avg * k_avg_ratio ||
                  stress_avg < k_abs_floor);
    JARVIS_ASSERT(stress_p95 <=
                  (base_p95 * k_avg_ratio > k_abs_floor
                       ? base_p95 * k_avg_ratio
                       : k_abs_floor));
    JARVIS_ASSERT(stress_max < k_pathological_cap);
    JARVIS_ASSERT(arch::interrupts_enabled());
    JARVIS_TEST_PASS();
}

void register_stress_hrt_tests() {
    Logger::info("Registering stress_hrt tests");
    JARVIS_REGISTER_TEST(stress_hrt_rdtsc_baseline);
    JARVIS_REGISTER_TEST(stress_hrt_ipc_latency_hard_bound);
}