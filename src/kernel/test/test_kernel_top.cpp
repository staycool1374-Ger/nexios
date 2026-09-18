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
/// @note  `IrqThread` teardown lives in `src/kernel/irq_thread.cpp`
///        (`IrqThread::destroy`, issue #144); the tests below drive
///        create/try_push_data/for_vector/destroy with handler tasks at a
///        priority that never dispatches, and destroy each instance before
///        asserting, so no ResourceTracker task delta survives the test
///        boundary.  The live-dispatch e2e (isr_entry + task_entry at
///        prio 50) was parked as a stub under issue #148 and reactivated
///        by its Notify::wait() fix.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/hal/irq_latency_histogram.hpp>
#include <kernel/random.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/log/ring_buffer.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/irq_thread.hpp>
#include <string.hpp>

using namespace kernel;
using kernel::IrqLatencyHistogram;

// Forward declaration (definition: src/kernel/kernel.cpp, global scope —
// kernel.hpp:55 declares ::format_datetime, which has no definition).
void format_datetime(char *buf, size_t size, uint64_t wall_ns);

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

/// @brief Vectors with no QEMU hardware source — only the test drives them,
///        so isr_entry/ack/handler observations are deterministic.
constexpr uint8_t k_irq_thread_vector = 200;
constexpr uint8_t k_irq_thread_vector_b = 201;
constexpr uint8_t k_irq_thread_vector_c = 202;
/// @brief Handler-task priority, deliberately BELOW the test task: handler
///        tasks never dispatch (the test never blocks), so create/ring/
///        destroy tests are fully deterministic with no timer-tick
///        dependence.  Only the live-dispatch e2e uses a priority above
///        the test task (issue #148).
constexpr uint64_t k_irq_thread_prio = 5;

/// @brief Custom ISR ack: no real EOI, so tests never touch the interrupt
///        controller.
void test_irq_ack_probe(uint8_t vector) {
    (void)vector;
}

/// @brief Handler probe (passed to create() so instances carry a valid
///        handler; never dispatched at k_irq_thread_prio).
void test_irq_handler_probe(uint64_t vector, uint64_t error_code,
                            uint64_t rip) {
    (void)vector;
    (void)error_code;
    (void)rip;
}

/// @brief Live-dispatch e2e probes (issue #148): cross-task flags, hence
///        atomic access on every side (CODING_STYLE §11.6).
uint64_t g_e2e_ack_count = 0;
uint64_t g_e2e_handler_ran = 0;
uint64_t g_e2e_handler_seen = 0;

/// @brief Custom ISR ack for the e2e: counts invocations, no real EOI.
void test_irq_e2e_ack(uint8_t vector) {
    (void)vector;
    __atomic_add_fetch(&g_e2e_ack_count, 1, __ATOMIC_RELAXED);
}

/// @brief Handler for the e2e: records the vector it was dispatched with.
void test_irq_e2e_handler(uint64_t vector, uint64_t error_code,
                          uint64_t rip) {
    (void)error_code;
    (void)rip;
    __atomic_store_n(&g_e2e_handler_seen, vector, __ATOMIC_RELAXED);
    __atomic_store_n(&g_e2e_handler_ran, 1, __ATOMIC_RELAXED);
}

// Runmode: kernel
// Testidea: create() must reject a null handler without side effects, and
//           for_vector()/destroy() must report absence for unknown vectors.
// Input: create(vector, prio, nullptr); for_vector/destroy on vector 200
//        with no live instance.
// Expect: create false, for_vector nullptr, destroy false; no task spawned.
// Depends: kernel::IrqThread::create/for_vector/destroy
JARVIS_TEST(kernel_irq_thread_create_validate,
            "PRE: vfsd, iocd | POST: none") {
    bool created = IrqThread::create(k_irq_thread_vector, k_irq_thread_prio,
                                     nullptr);
    auto *found = IrqThread::for_vector(k_irq_thread_vector);
    bool destroyed = IrqThread::destroy(k_irq_thread_vector);
    JARVIS_ASSERT(!created);
    JARVIS_ASSERT(found == nullptr);
    JARVIS_ASSERT(!destroyed);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: create() is idempotent per vector — re-creating a live vector
//           reuses the instance instead of spawning a second handler task.
// Input: create(200) twice with the same handler.
// Expect: Both return true, for_vector resolves to the same instance both
//         times; destroy removes it (for_vector nullptr afterwards).
// Depends: kernel::IrqThread::create/for_vector/destroy
JARVIS_TEST(kernel_irq_thread_duplicate_reuse,
            "PRE: vfsd, iocd | POST: none") {
    IrqThread::destroy(k_irq_thread_vector);
    bool first = IrqThread::create(k_irq_thread_vector, k_irq_thread_prio,
                                   test_irq_handler_probe, test_irq_ack_probe);
    auto *first_inst = IrqThread::for_vector(k_irq_thread_vector);
    bool second = IrqThread::create(k_irq_thread_vector, k_irq_thread_prio,
                                    test_irq_handler_probe,
                                    test_irq_ack_probe);
    auto *second_inst = IrqThread::for_vector(k_irq_thread_vector);
    bool destroyed = IrqThread::destroy(k_irq_thread_vector);
    auto *gone = IrqThread::for_vector(k_irq_thread_vector);
    JARVIS_ASSERT(first);
    JARVIS_ASSERT(second);
    JARVIS_ASSERT(first_inst != nullptr);
    JARVIS_ASSERT(second_inst == first_inst);
    JARVIS_ASSERT(destroyed);
    JARVIS_ASSERT(gone == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The ISR→task ring holds RING_CAPACITY-1 bytes (the classic
//           head+1==tail full/empty disambiguation); the first byte past
//           that is rejected so a chatty ISR cannot corrupt the handler's
//           data stream.
// Input: create(200); push 63 bytes (expect true); push 1 more (expect
//        false); destroy.
// Expect: Full-usable-capacity push accepted, over-capacity push rejected,
//         destroy removes the instance.
// Depends: kernel::IrqThread::create/try_push_data/destroy/for_vector
JARVIS_TEST(kernel_irq_thread_ring_full_reject,
            "PRE: vfsd, iocd | POST: none") {
    IrqThread::destroy(k_irq_thread_vector);
    bool created = IrqThread::create(k_irq_thread_vector, k_irq_thread_prio,
                                     test_irq_handler_probe,
                                     test_irq_ack_probe);
    auto *inst = IrqThread::for_vector(k_irq_thread_vector);
    uint8_t chunk[IrqThread::RING_CAPACITY - 1] = {};
    bool pushed =
        inst && inst->try_push_data(chunk, sizeof(chunk));
    uint8_t extra = 0xAA;
    bool rejected = inst && !inst->try_push_data(&extra, 1);
    bool destroyed = IrqThread::destroy(k_irq_thread_vector);
    auto *gone = IrqThread::for_vector(k_irq_thread_vector);
    JARVIS_ASSERT(created);
    JARVIS_ASSERT(inst != nullptr);
    JARVIS_ASSERT(pushed);
    JARVIS_ASSERT(rejected);
    JARVIS_ASSERT(destroyed);
    JARVIS_ASSERT(gone == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: End-to-end threaded-IRQ path — isr_entry runs the custom ack
//           (no real EOI) and wakes the handler task; task_entry dispatches
//           the registered handler in task context at higher priority.
//           Reactivated by the issue #148 Notify::wait() fix: wait() now
//           blocks scheduler-mediated (Semaphore shape) and consumes a
//           notify that arrived before the first wait, so the dispatch +
//           notify/wake ordering is race-free in both directions.
// Input: create(200, prio 50, e2e handler, e2e ack); bounded settle spin
//        so ticks can dispatch the handler into its wait; direct
//        isr_entry(200,0,0); bounded spin for the handler flag; destroy.
// Expect: Ack ran exactly once, handler observed vector 200, destroy
//         removes the instance — all with no ResourceTracker task delta.
// Depends: kernel::IrqThread::create/isr_entry/task_entry/destroy,
//          Scheduler dispatch of a higher-priority task on timer ticks
JARVIS_TEST(kernel_irq_thread_isr_ack_and_task_entry,
            "PRE: vfsd, iocd | POST: none") {
    constexpr uint64_t k_dispatch_prio = 50;
    constexpr uint32_t k_settle_spins = 200000;
    constexpr uint32_t k_wake_spins = 500000;
    IrqThread::destroy(k_irq_thread_vector);
    __atomic_store_n(&g_e2e_ack_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_e2e_handler_ran, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_e2e_handler_seen, 0, __ATOMIC_RELAXED);
    bool created = IrqThread::create(k_irq_thread_vector, k_dispatch_prio,
                                     test_irq_e2e_handler, test_irq_e2e_ack);
    auto *inst = IrqThread::for_vector(k_irq_thread_vector);
    // Timing only, never asserted: let timer ticks dispatch the handler
    // into its Notify wait.  Either ordering is correct — a notify that
    // lands first stays pending and the first wait consumes it.
    for (uint32_t spin = 0; spin < k_settle_spins; ++spin) {
        arch::pause();
    }
    if (inst != nullptr) {
        IrqThread::isr_entry(k_irq_thread_vector, 0, 0);
    }
    for (uint32_t spin = 0;
         spin < k_wake_spins &&
         __atomic_load_n(&g_e2e_handler_ran, __ATOMIC_RELAXED) == 0;
         ++spin) {
        arch::pause();
    }
    // Teardown BEFORE asserting (asserts return on failure).
    bool destroyed = IrqThread::destroy(k_irq_thread_vector);
    auto *gone = IrqThread::for_vector(k_irq_thread_vector);
    JARVIS_ASSERT(created);
    JARVIS_ASSERT(inst != nullptr);
    JARVIS_ASSERT(__atomic_load_n(&g_e2e_ack_count, __ATOMIC_RELAXED) == 1);
    JARVIS_ASSERT(__atomic_load_n(&g_e2e_handler_ran, __ATOMIC_RELAXED) == 1);
    JARVIS_ASSERT(__atomic_load_n(&g_e2e_handler_seen, __ATOMIC_RELAXED) ==
                  k_irq_thread_vector);
    JARVIS_ASSERT(destroyed);
    JARVIS_ASSERT(gone == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: destroy() compacts the instance table, so destroying a
//           non-last vector keeps the survivors live and the freed slot
//           reusable — create/destroy cycles cannot exhaust the 16 slots.
// Input: create(200)+create(201); destroy(200); push to the moved 201;
//        create(202); destroy(201); destroy(202).
// Expect: 201 stays live and functional across the compaction, 202 is
//         created into the freed space, all three vectors resolve to
//         nullptr afterwards.
// Depends: kernel::IrqThread::create/destroy/for_vector/try_push_data
JARVIS_TEST(kernel_irq_thread_destroy_compacts,
            "PRE: vfsd, iocd | POST: none") {
    IrqThread::destroy(k_irq_thread_vector);
    IrqThread::destroy(k_irq_thread_vector_b);
    IrqThread::destroy(k_irq_thread_vector_c);
    bool first = IrqThread::create(k_irq_thread_vector, k_irq_thread_prio,
                                   test_irq_handler_probe, test_irq_ack_probe);
    bool second = IrqThread::create(k_irq_thread_vector_b, k_irq_thread_prio,
                                    test_irq_handler_probe,
                                    test_irq_ack_probe);
    bool destroyed_first = IrqThread::destroy(k_irq_thread_vector);
    auto *moved = IrqThread::for_vector(k_irq_thread_vector_b);
    uint8_t byte = 0x5A;
    bool pushed = moved && moved->try_push_data(&byte, 1);
    bool third = IrqThread::create(k_irq_thread_vector_c, k_irq_thread_prio,
                                   test_irq_handler_probe, test_irq_ack_probe);
    bool destroyed_second = IrqThread::destroy(k_irq_thread_vector_b);
    bool destroyed_third = IrqThread::destroy(k_irq_thread_vector_c);
    auto *gone_a = IrqThread::for_vector(k_irq_thread_vector);
    auto *gone_b = IrqThread::for_vector(k_irq_thread_vector_b);
    auto *gone_c = IrqThread::for_vector(k_irq_thread_vector_c);
    JARVIS_ASSERT(first);
    JARVIS_ASSERT(second);
    JARVIS_ASSERT(destroyed_first);
    JARVIS_ASSERT(moved != nullptr);
    JARVIS_ASSERT(pushed);
    JARVIS_ASSERT(third);
    JARVIS_ASSERT(destroyed_second);
    JARVIS_ASSERT(destroyed_third);
    JARVIS_ASSERT(gone_a == nullptr);
    JARVIS_ASSERT(gone_b == nullptr);
    JARVIS_ASSERT(gone_c == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Epoch-to-date conversion anchors — wall_ns 0 renders the epoch
//           exactly, and the last millisecond of 1999 exercises the ms field
//           maximum plus the year/month/day rollover.
// Input: format_datetime(0), format_datetime(946684799999000000).
// Expect: "1970-01-01 00:00:00:000" and "1999-12-31 23:59:59:999", exact.
// Depends: ::format_datetime
JARVIS_TEST(kernel_datetime_epoch_and_rollover,
            "PRE: none | POST: none") {
    char buf[32] = {};
    ::format_datetime(buf, sizeof(buf), 0);
    size_t i = 0;
    const char *expect_epoch = "1970-01-01 00:00:00:000";
    while (expect_epoch[i] && buf[i] == expect_epoch[i])
        ++i;
    JARVIS_ASSERT(expect_epoch[i] == '\0' && buf[i] == '\0');

    char buf2[32] = {};
    ::format_datetime(buf2, sizeof(buf2), 946684799999000000ULL);
    const char *expect_roll = "1999-12-31 23:59:59:999";
    i = 0;
    while (expect_roll[i] && buf2[i] == expect_roll[i])
        ++i;
    JARVIS_ASSERT(expect_roll[i] == '\0' && buf2[i] == '\0');
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Leap-year rules through the converter — 2024 (divisible by 4),
//           2000 (divisible by 400, leap despite the century), 2100
//           (divisible by 100 but not 400 — Feb has 28 days, so the day
//           after 02-28 is 03-01), and a common-year February end. Together
//           these hit every branch combination of is_leap().
// Input: format_datetime on four integer-exact wall_ns vectors.
// Expect: "2024-02-29 12:34:56:123", "2000-02-29 00:00:00:000",
//         "2100-03-01 00:00:00:000", "2023-02-28 23:59:59:000", exact.
// Depends: ::format_datetime, is_leap (via format_datetime)
JARVIS_TEST(kernel_datetime_leap_rules, "PRE: none | POST: none") {
    struct Case {
        uint64_t wall_ns;
        const char *expect;
    };
    static const Case cases[] = {
        {1709210096123000000ULL, "2024-02-29 12:34:56:123"},
        {951782400000000000ULL, "2000-02-29 00:00:00:000"},
        {4107542400000000000ULL, "2100-03-01 00:00:00:000"},
        {1677628799000000000ULL, "2023-02-28 23:59:59:000"},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        char buf[32] = {};
        ::format_datetime(buf, sizeof(buf), cases[c].wall_ns);
        size_t i = 0;
        while (cases[c].expect[i] && buf[i] == cases[c].expect[i])
            ++i;
        JARVIS_ASSERT_FMT(cases[c].expect[i] == '\0' && buf[i] == '\0',
                          "datetime case %u mismatch (got '%s')", (unsigned)c,
                          buf);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Conversion guards — null buffer and undersized buffers (< 24)
//           must return without writing anything (no crash, no partial
//           output, sentinel intact).
// Input: format_datetime(nullptr, 32, 0); format_datetime(sentinel, 10/23).
// Expect: No crash; sentinel bytes after index 0 untouched.
// Depends: ::format_datetime
JARVIS_TEST(kernel_datetime_guards, "PRE: none | POST: none") {
    ::format_datetime(nullptr, 32, 0);
    char small[10];
    __builtin_memset(small, 0xAA, sizeof(small));
    ::format_datetime(small, sizeof(small), 0);
    char edge[23];
    __builtin_memset(edge, 0xAA, sizeof(edge));
    ::format_datetime(edge, sizeof(edge), 0);
    for (size_t i = 0; i < sizeof(small); ++i)
        JARVIS_ASSERT(small[i] == static_cast<char>(0xAA));
    for (size_t i = 0; i < sizeof(edge); ++i)
        JARVIS_ASSERT(edge[i] == static_cast<char>(0xAA));
    JARVIS_TEST_PASS();
}

void register_kernel_top_tests() {
    Logger::info("Registering top-level kernel tests");
    JARVIS_REGISTER_TEST(kernel_datetime_epoch_and_rollover);
    JARVIS_REGISTER_TEST(kernel_datetime_leap_rules);
    JARVIS_REGISTER_TEST(kernel_datetime_guards);
    JARVIS_REGISTER_TEST(kernel_irq_latency_empty_dump);
    JARVIS_REGISTER_TEST(kernel_irq_latency_reports_sample_count);
    JARVIS_REGISTER_TEST(kernel_irq_latency_clamps_overflow);
    JARVIS_REGISTER_TEST(kernel_random_fill_and_u64_stream);
    JARVIS_REGISTER_TEST(kernel_irq_thread_create_validate);
    JARVIS_REGISTER_TEST(kernel_irq_thread_duplicate_reuse);
    JARVIS_REGISTER_TEST(kernel_irq_thread_ring_full_reject);
    JARVIS_REGISTER_TEST(kernel_irq_thread_isr_ack_and_task_entry);
    JARVIS_REGISTER_TEST(kernel_irq_thread_destroy_compacts);
}
