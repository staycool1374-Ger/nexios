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

/// @file bench_syscall_latency.cpp
/// @brief Benchmark — system call latency measurements.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/sync/mutex.hpp>
#include <kernel/sync/spsc_ring.hpp>

using namespace kernel;

static constexpr size_t BENCH_ITERATIONS = 10000;

namespace {

struct BenchResult {
    uint64_t min;
    uint64_t avg;
    uint64_t max;
};

} // anonymous namespace

template <typename F> static BenchResult measure(F fn) {
    BenchResult r = {~0ULL, 0, 0};
    for (size_t i = 0; i < 100; ++i)
        fn();
    for (size_t i = 0; i < BENCH_ITERATIONS; ++i) {
        uint64_t t0 = arch::rdtsc();
        fn();
        uint64_t elapsed = arch::rdtsc() - t0;
        if (elapsed < r.min)
            r.min = elapsed;
        if (elapsed > r.max)
            r.max = elapsed;
        r.avg += elapsed;
    }
    r.avg /= BENCH_ITERATIONS;
    return r;
}

// Runmode: kernel
// Testidea: Benchmark SpinLock lock/unlock latency.
// Input: lock() then unlock() in a tight loop, measure via RDTSC.
// Expect: Average latency measured.
// Depends: SpinLock, arch::rdtsc
JARVIS_TEST(bench_spinlock_acquire_release, "PRE: none | POST: none") {
    sync::SpinLock lock;
    auto r = measure([&lock]() { SpinLockGuard<sync::SpinLock> g(lock); });
    Logger::info("spinlock_lock_unlock: min=%lu, avg=%lu, max=%lu", r.min,
                 r.avg, r.max);
    JARVIS_ASSERT(r.avg > 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Benchmark Mutex lock/unlock latency.
// Input: lock() then unlock() in a tight loop, measure via RDTSC.
// Expect: Average latency measured (mutex includes blocking path).
// Depends: Mutex, arch::rdtsc
JARVIS_TEST(bench_mutex_acquire_release, "PRE: none | POST: none") {
    sync::Mutex mtx;
    auto r = measure([&mtx]() {
        mtx.lock();
        mtx.unlock();
    });
    Logger::info("mutex_lock_unlock: min=%lu, avg=%lu, max=%lu", r.min, r.avg,
                 r.max);
    JARVIS_ASSERT(r.avg > 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Benchmark SPSC ring push/pop latency.
// Input: try_push then try_pop in a tight loop, measure via RDTSC.
// Expect: Average latency measured.
// Depends: SPSCRing, arch::rdtsc
JARVIS_TEST(bench_spsc_push_pop, "PRE: none | POST: none") {
    SPSCRing<uint64_t, 256> ring;
    auto r = measure([&ring]() {
        ring.try_push(42);
        uint64_t v;
        ring.try_pop(v);
    });
    Logger::info("spsc_push_pop: min=%lu, avg=%lu, max=%lu", r.min, r.avg,
                 r.max);
    JARVIS_ASSERT(r.avg > 0);
    JARVIS_TEST_PASS();
}

void register_bench_syscall_latency_tests() {
    Logger::info("Registering syscall latency benchmarks");
    JARVIS_REGISTER_TEST_FLAGS(bench_spinlock_acquire_release, kernel::test::TF_BENCH);
    JARVIS_REGISTER_TEST_FLAGS(bench_mutex_acquire_release, kernel::test::TF_BENCH);
    JARVIS_REGISTER_TEST_FLAGS(bench_spsc_push_pop, kernel::test::TF_BENCH);
}
