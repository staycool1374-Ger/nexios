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

/// @file bench_irq_latency.cpp
/// @brief Benchmark — interrupt latency measurements.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/keyboard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/irq_latency_histogram.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

static constexpr size_t BENCH_ITERATIONS = 1000;

namespace {

struct BenchResult {
    uint64_t min;
    uint64_t avg;
    uint64_t max;
};

} // anonymous namespace

// Runmode: kernel
// Testidea: Simulate reschedule() overhead (core of ISR-triggered context
// switch). Input: Create two tasks, measure time for reschedule() cycle.
// Expect: Average latency measured in cycles.
// Depends: Scheduler, arch::rdtsc
JARVIS_TEST(bench_reschedule_latency, "PRE: none | POST: none") {
    auto *a = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(a != nullptr);
    Scheduler::add_task(*a);
    auto *b = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(b != nullptr);
    Scheduler::add_task(*b);

    auto *original = Scheduler::current_task();
    BenchResult r = {~0ULL, 0, 0};

    for (size_t i = 0; i < 100; ++i) {
        kernel::test::yield_as(*a);
    }

    for (size_t i = 0; i < BENCH_ITERATIONS; ++i) {
        Scheduler::set_current(*a);
        uint64_t t0 = arch::rdtsc();
        Scheduler::reschedule();
        uint64_t elapsed = arch::rdtsc() - t0;
        if (elapsed < r.min)
            r.min = elapsed;
        if (elapsed > r.max)
            r.max = elapsed;
        r.avg += elapsed;
    }
    r.avg /= BENCH_ITERATIONS;

    Scheduler::set_current(*original);

    Logger::info("reschedule: min=%lu, avg=%lu, max=%lu", r.min, r.avg, r.max);

    kernel::test::terminate_and_drain2(a, b);

    JARVIS_ASSERT(r.avg > 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Measure SpinLock try_lock latency (used in ISR paths).
// Input: try_lock/unlock in a tight loop.
// Expect: Average latency measured.
// Depends: SpinLock, arch::rdtsc
JARVIS_TEST(bench_spinlock_try_lock, "PRE: none | POST: none") {
    sync::SpinLock lock;
    BenchResult r = {~0ULL, 0, 0};
    for (size_t i = 0; i < 100; ++i) {
        lock.try_lock();
        lock.unlock();
    }
    for (size_t i = 0; i < BENCH_ITERATIONS; ++i) {
        uint64_t t0 = arch::rdtsc();
        lock.try_lock();
        lock.unlock();
        uint64_t elapsed = arch::rdtsc() - t0;
        if (elapsed < r.min)
            r.min = elapsed;
        if (elapsed > r.max)
            r.max = elapsed;
        r.avg += elapsed;
    }
    r.avg /= BENCH_ITERATIONS;
    Logger::info("spinlock_try_lock: min=%lu, avg=%lu, max=%lu", r.min, r.avg,
                 r.max);
    JARVIS_ASSERT(r.avg > 0);
    JARVIS_TEST_PASS();
}

#if CONFIG_IRQ_LATENCY_HISTOGRAM
// Runmode: kernel
// Testidea: Dump IRQ latency histogram collected during boot.
// Input: none (reads static histogram data).
// Expect: At least one IRQ recorded (timer tick).
JARVIS_TEST(bench_irq_histogram_dump, "PRE: none | POST: none") {
    kernel::IrqLatencyHistogram::dump();
    JARVIS_TEST_PASS();
}
#endif

void register_bench_irq_latency_tests() {
    Logger::info("Registering IRQ-path latency benchmarks");
    JARVIS_REGISTER_TEST_FLAGS(bench_reschedule_latency, kernel::test::TF_BENCH);
    JARVIS_REGISTER_TEST_FLAGS(bench_spinlock_try_lock, kernel::test::TF_BENCH);
#if CONFIG_IRQ_LATENCY_HISTOGRAM
    JARVIS_REGISTER_TEST_FLAGS(bench_irq_histogram_dump, kernel::test::TF_BENCH);
#endif
}
