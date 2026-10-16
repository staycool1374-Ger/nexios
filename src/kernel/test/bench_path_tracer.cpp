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

/// @file bench_path_tracer.cpp
/// @brief Benchmark — XOR path-tracer step overhead (issue #347 Slice E).
///
/// Measures the per-step cost of path_step() (one XOR into the own-CPU
/// chain) and path_validate(). The issue budget is ~1 cycle/step on
/// hardware; under QEMU TCG (no icount here) the wall is dilated and
/// jittery, so the assertion is a loose upper bound that still catches a
/// runaway (e.g. an accidental allocation or lock in the step path).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/core/path_tracer.hpp>

using namespace kernel;

namespace {

constexpr size_t kBenchSteps = 100000;

} // namespace

// Runmode: kernel
// Testidea: Per-step XOR overhead of the tracer.
// Input: kBenchSteps path_step() calls on the own-CPU chain.
// Expect: mean cycles/step under a loose TCG-tolerant bound; validate
//         cost reported alongside. TF_BENCH (never a correctness gate).
// Depends: kernel::path_step, arch::rdtsc
JARVIS_TEST(bench_path_trace_step_overhead, "PRE: none | POST: none") {
    path_reset();

    // Warm-up so the first-call cache miss does not skew the mean.
    for (size_t i = 0; i < 1024; ++i) {
        path_step(1ULL << (i & 63));
    }
    path_reset();

    uint64_t t0 = arch::rdtsc();
    for (size_t i = 0; i < kBenchSteps; ++i) {
        path_step(1ULL << (i & 63));
    }
    uint64_t step_cycles = arch::rdtsc() - t0;
    uint64_t step_per = step_cycles / kBenchSteps;
    path_reset();

    t0 = arch::rdtsc();
    for (size_t i = 0; i < kBenchSteps; ++i) {
        (void)path_validate(0);
    }
    uint64_t val_per = (arch::rdtsc() - t0) / kBenchSteps;

    Logger::raw_write("[BENCH-PATH] step=");
    Logger::print_dec(step_per);
    Logger::raw_write(" cyc/step validate=");
    Logger::print_dec(val_per);
    Logger::raw_write(" cyc/call\n");

    // Loose bound: the step is one XOR + an array index. Even under TCG
    // dilation this must stay far below a context switch.
    JARVIS_ASSERT(step_per < 100);
    JARVIS_ASSERT(val_per < 100);
    path_reset();
    JARVIS_TEST_PASS();
}

void register_bench_path_tracer_tests() {
    JARVIS_REGISTER_TEST_FLAGS(bench_path_trace_step_overhead,
                               kernel::test::TF_BENCH);
}
