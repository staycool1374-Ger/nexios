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

/// @file test_kernel_top.cpp
/// @brief Top-level kernel tests (milestone v0.4.10 issue #131) — the IRQ
///        latency histogram export path in src/kernel/irq_latency_histogram.cpp:
///        `dump()` was never entered by any test class.
/// @note  Observation channel: the histogram reports through `Logger::info`,
///        which during test runs routes to QEMU debugcon (not the UART) and
///        also feeds the klog ring — whose documented consumers include
///        tests.  Each probe clears the klog, runs the code under test, reads
///        it back and asserts on the rendered text, all inside an
///        arch::IrqGuard so no other task can contribute bytes.
/// @note  `record()` is safe to drive with an arbitrary delta only because
///        CONFIG_IRQ_LATENCY_MAX_NS is 0; with a non-zero budget an oversized
///        delta calls Logger::fatal + panic.  The overflow test below
///        therefore asserts the *clamping* behaviour, not the panic path.
/// @note  `IrqThread` is deliberately NOT driven here: `create()` spawns a
///        handler task that test cleanup explicitly spares
///        (test_cleanup.cpp:54, test_isolate.cpp:1550), so it would survive
///        the test boundary and show up as a ResourceTracker task delta.
///        Covering it needs a public teardown (or a tracker-exempt fixture);
///        tracked as a follow-up on #131.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/hal/irq_latency_histogram.hpp>
#include <kernel/random.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/log/ring_buffer.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <string.hpp>

using namespace kernel;
using kernel::IrqLatencyHistogram;

namespace {

/// @brief Read-back buffer for the klog probe.
constexpr size_t k_probe_size = 4096;

bool has(const char *haystack, const char *needle) {
    if (!*needle)
        return true;
    for (size_t i = 0; haystack[i]; ++i) {
        size_t j = 0;
        while (needle[j] && haystack[i + j] == needle[j])
            ++j;
        if (!needle[j])
            return true;
    }
    return false;
}

/// @brief Runs `action` with the scheduler frozen and the klog pre-drained,
///        then returns what the action logged.
template <typename Fn>
size_t capture_log(Fn &&action, char *buffer, size_t size) {
    buffer[0] = '\0';
    size_t length = 0;
    {
        arch::IrqGuard irq_guard{};
        kernel::log::KlogService::instance().clear();
        action();
        length = kernel::log::KlogService::instance().read(buffer, size - 1);
    }
    buffer[length] = '\0';
    return length;
}

} // namespace

// Runmode: kernel
// Testidea: A freshly initialised histogram has no samples; dump() must
//           render the zero-sample header without touching stale bucket
//           contents or dividing by an uncalibrated scale.
// Input: init() then dump(), observed through the klog.
// Expect: The report carries the histogram header and the "(0 samples"
//         count, and renders no bucket lines.
// Depends: kernel::IrqLatencyHistogram, log::KlogService
JARVIS_TEST(kernel_irq_latency_empty_dump,
            "PRE: vfsd, iocd | POST: none") {
    char report[k_probe_size];
    const size_t length = capture_log(
        []() {
            IrqLatencyHistogram::init();
            IrqLatencyHistogram::dump();
        },
        report, sizeof(report));

    JARVIS_ASSERT(length > 0);
    JARVIS_ASSERT(has(report, "IRQ latency histogram"));
    JARVIS_ASSERT(has(report, "(0 samples"));
    JARVIS_ASSERT(!has(report, "ns]:"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: record() samples the ISR entry-to-exit latency; dump() must
//           report exactly the number of samples taken and render a bucket
//           line for the latency range they fell into.
// Input: init(), three record(rdtsc()) calls (near-zero delta, so they land
//        in the lowest bucket), then dump().
// Expect: The header reports "(3 samples" and at least one bucket line
//         "[lo-hi ns]:" is rendered.
// Depends: kernel::IrqLatencyHistogram, arch::rdtsc, arch::Timer::tsc_freq_hz
JARVIS_TEST(kernel_irq_latency_reports_sample_count,
            "PRE: vfsd, iocd | POST: none") {
    const uint64_t tsc_freq = arch::Timer::tsc_freq_hz();

    char report[k_probe_size];
    const size_t length = capture_log(
        []() {
            IrqLatencyHistogram::init();
            IrqLatencyHistogram::record(arch::rdtsc());
            IrqLatencyHistogram::record(arch::rdtsc());
            IrqLatencyHistogram::record(arch::rdtsc());
            IrqLatencyHistogram::dump();
        },
        report, sizeof(report));

    // A zero TSC frequency would make record() divide by zero — the histogram
    // is only meaningful once Timer::init() has calibrated it.
    JARVIS_ASSERT(tsc_freq != 0);
    JARVIS_ASSERT(length > 0);
    JARVIS_ASSERT(has(report, "IRQ latency histogram"));
    JARVIS_ASSERT(has(report, "(3 samples"));
    JARVIS_ASSERT(has(report, "ns]:"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Latencies beyond the histogram range must be clamped into the
//           LAST bucket rather than indexed out of bounds — a sample whose
//           delta exceeds BUCKETS * scale has to land in the top range and
//           still be reported exactly once.
// Input: init(), one record() with a deliberately huge entry TSC (delta far
//        beyond the 100 us range), then dump().
// Expect: The header reports one sample, exactly one bucket line is
//         rendered, and that line is the top range (its lower bound is
//         (BUCKETS-1) * RANGE_NS / BUCKETS = 98437 ns).
// Depends: kernel::IrqLatencyHistogram, arch::rdtsc
JARVIS_TEST(kernel_irq_latency_clamps_overflow,
            "PRE: vfsd, iocd | POST: none") {
    constexpr uint64_t k_huge_delta = 1000000000ULL; // 1e9 TSC cycles
    constexpr uint64_t k_top_bucket_lo =
        (IrqLatencyHistogram::BUCKETS - 1) * IrqLatencyHistogram::RANGE_NS /
        IrqLatencyHistogram::BUCKETS;

    char report[k_probe_size];
    const size_t length = capture_log(
        [k_huge_delta]() {
            IrqLatencyHistogram::init();
            IrqLatencyHistogram::record(arch::rdtsc() - k_huge_delta);
            IrqLatencyHistogram::dump();
        },
        report, sizeof(report));

    // Count exact occurrences of the bucket-line terminator "ns]:" (a
    // substring search from every index would match once per position
    // preceding an occurrence and over-count).
    size_t bucket_lines = 0;
    for (size_t i = 0; report[i]; ++i) {
        if (report[i] == 'n' && report[i + 1] == 's' && report[i + 2] == ']' &&
            report[i + 3] == ':') {
            ++bucket_lines;
            i += 3;
        }
    }

    // Top-range lower bound rendered as decimal — verify the exact top bucket.
    char expected_lo[16] = {};
    size_t pos = 0;
    uint64_t value = k_top_bucket_lo;
    while (value > 0 && pos < sizeof(expected_lo) - 1) {
        expected_lo[pos++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    for (size_t i = 0; i < pos / 2; ++i) {
        const char tmp = expected_lo[i];
        expected_lo[i] = expected_lo[pos - 1 - i];
        expected_lo[pos - 1 - i] = tmp;
    }
    expected_lo[pos] = '\0';

    JARVIS_ASSERT(length > 0);
    JARVIS_ASSERT(has(report, "(1 samples"));
    JARVIS_ASSERT(bucket_lines == 1);
    JARVIS_ASSERT(has(report, expected_lo));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The kernel RNG must fill a buffer of the requested length and
//           produce a non-constant stream — `random_fill` is what /dev/random
//           and the ASLR/capability paths draw from, and `random_u64` is its
//           scalar wrapper.  A stuck or constant generator would silently
//           weaken every caller.
// Input: random_fill() into a 32-byte buffer (twice), then eight
//        random_u64() draws.
// Expect: The two 32-byte fills differ; at least two of the eight scalar
//         draws differ; the filled buffer is not left all-zero.
// Depends: kernel::random_fill, kernel::random_u64
JARVIS_TEST(kernel_random_fill_and_u64_stream,
            "PRE: vfsd, iocd | POST: none") {
    uint8_t first[32] = {};
    uint8_t second[32] = {};

    random_fill(first, sizeof(first));
    random_fill(second, sizeof(second));

    bool fills_differ = memcmp(first, second, sizeof(first)) != 0;
    bool not_all_zero = false;
    for (size_t i = 0; i < sizeof(first); ++i) {
        if (first[i] != 0) {
            not_all_zero = true;
            break;
        }
    }

    uint64_t draws[8] = {};
    for (size_t i = 0; i < 8; ++i)
        draws[i] = random_u64();

    bool scalars_vary = false;
    for (size_t i = 1; i < 8; ++i) {
        if (draws[i] != draws[0]) {
            scalars_vary = true;
            break;
        }
    }

    JARVIS_ASSERT(fills_differ);
    JARVIS_ASSERT(not_all_zero);
    JARVIS_ASSERT(scalars_vary);
    JARVIS_TEST_PASS();
}

void register_kernel_top_tests() {
    Logger::info("Registering top-level kernel tests");
    JARVIS_REGISTER_TEST(kernel_irq_latency_empty_dump);
    JARVIS_REGISTER_TEST(kernel_irq_latency_reports_sample_count);
    JARVIS_REGISTER_TEST(kernel_irq_latency_clamps_overflow);
    JARVIS_REGISTER_TEST(kernel_random_fill_and_u64_stream);
}
