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
#include <kernel/core/global_state.hpp>
#include <kernel/task/scheduler.hpp>
#include <scope_guard.hpp>
#include <string.hpp>

using namespace kernel;
using kernel::IrqLatencyHistogram;

// Forward declaration (definition: src/kernel/kernel.cpp, global scope —
// kernel.hpp:55 declares ::format_datetime, which has no definition).
void format_datetime(char *buf, size_t size, uint64_t wall_ns);

// Local declaration: scheduler_diag_rsp_abort is defined extern "C" in
// src/kernel/core/global_state.cpp but has no header declaration (the
// header carries only its doc comment).  Declared here so the #136
// diagnostic-hook test can enter it.
extern "C" void scheduler_diag_rsp_abort() noexcept;

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
constexpr uint8_t k_irq_thread_vector_d = 203;
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

/// @brief Synchronous-ack probe: runs in isr_entry on the calling task, so
///        no atomics are needed (the prio-5 handler never dispatches while
///        the test task runs).
uint64_t g_sync_ack_count = 0;
void test_irq_sync_ack(uint8_t vector) {
    (void)vector;
    g_sync_ack_count += 1;
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
// Testidea: Unknown vectors are total no-ops — lookup misses, isr_entry
//           returns before any ack/EOI or wake, destroy reports false, and
//           the task-guard rejects null and non-handler tasks.
// Input: for_vector/isr_entry/destroy on never-created vector 203;
//        is_irq_thread_task(nullptr) and on the running test task.
// Expect: nullptr lookups, destroy false, guard false everywhere.
// Depends: kernel::IrqThread::for_vector/isr_entry/destroy,
//          is_irq_thread_task
JARVIS_TEST(kernel_irq_thread_unknown_vector_noop,
            "PRE: vfsd, iocd | POST: none") {
    JARVIS_ASSERT(IrqThread::for_vector(k_irq_thread_vector_d) == nullptr);
    IrqThread::isr_entry(k_irq_thread_vector_d, 0, 0);
    JARVIS_ASSERT(IrqThread::for_vector(k_irq_thread_vector_d) == nullptr);
    JARVIS_ASSERT(!IrqThread::destroy(k_irq_thread_vector_d));
    JARVIS_ASSERT(!IrqThread::is_irq_thread_task(nullptr));
    JARVIS_ASSERT(
        !IrqThread::is_irq_thread_task(Scheduler::current_task()));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: With a custom ack, isr_entry runs the ack synchronously on the
//           calling task — no dispatch needed.  Fully deterministic: the
//           prio-5 handler never preempts the running test task.
// Input: create(203, prio 5, probe handler, counting ack); isr_entry(203).
// Expect: Ack ran exactly once before destroy; instance removed after.
// Depends: kernel::IrqThread::create/isr_entry/destroy/for_vector
JARVIS_TEST(kernel_irq_thread_isr_ack_synchronous,
            "PRE: vfsd, iocd | POST: none") {
    IrqThread::destroy(k_irq_thread_vector_d);
    g_sync_ack_count = 0;
    bool created = IrqThread::create(k_irq_thread_vector_d,
                                     k_irq_thread_prio,
                                     test_irq_handler_probe,
                                     test_irq_sync_ack);
    auto *inst = IrqThread::for_vector(k_irq_thread_vector_d);
    JARVIS_ASSERT(created);
    JARVIS_ASSERT(inst != nullptr);
    IrqThread::isr_entry(k_irq_thread_vector_d, 0, 0);
    bool destroyed = IrqThread::destroy(k_irq_thread_vector_d);
    auto *gone = IrqThread::for_vector(k_irq_thread_vector_d);
    JARVIS_ASSERT_EQ(1ULL, g_sync_ack_count);
    JARVIS_ASSERT(destroyed);
    JARVIS_ASSERT(gone == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The task guard recognises the live handler task (found by its
//           "irq203" name via the registry iterator), and destroy() is
//           one-shot — the second call reports false instead of touching
//           the compacted table.
// Input: create(203); scan TaskIter for irq203; guard check; destroy twice.
// Expect: Guard true on the handler; first destroy true, second false,
//         lookup null afterwards.  The freed TCB is never dereferenced.
// Depends: kernel::IrqThread::create/destroy/for_vector,
//          is_irq_thread_task, Scheduler::TaskIter
JARVIS_TEST(kernel_irq_thread_task_guard_and_double_destroy,
            "PRE: vfsd, iocd | POST: none") {
    IrqThread::destroy(k_irq_thread_vector_d);
    bool created = IrqThread::create(k_irq_thread_vector_d,
                                     k_irq_thread_prio,
                                     test_irq_handler_probe,
                                     test_irq_ack_probe);
    JARVIS_ASSERT(created);
    TaskControlBlock *handler = nullptr;
    Scheduler::TaskIter task_iter{};
    while (auto *candidate = task_iter.next()) {
        if (has(candidate->name, "irq203")) {
            handler = candidate;
            break;
        }
    }
    JARVIS_ASSERT(handler != nullptr);
    JARVIS_ASSERT(IrqThread::is_irq_thread_task(handler));
    bool first_destroy = IrqThread::destroy(k_irq_thread_vector_d);
    bool second_destroy = IrqThread::destroy(k_irq_thread_vector_d);
    JARVIS_ASSERT(first_destroy);
    JARVIS_ASSERT(!second_destroy);
    JARVIS_ASSERT(IrqThread::for_vector(k_irq_thread_vector_d) == nullptr);
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

// Runmode: kernel
// Testidea: verify_and_write enforces its WriteClass on a scratch slot —
//           IDEMPOTENT rejects same-value, BOOT_ONLY rejects outside BOOT,
//           NEVER_WRITE always rejects, PLAIN/RANGE_CHECKED accept (range
//           enforcement lives in the setters, not the primitive).
// Input: Local uint64_t slot driven through every WriteClass.
// Expect: Accept/reject matrix above; rejected writes leave slot intact.
// Depends: kernel::gs::verify_and_write (issue #136)
JARVIS_TEST(global_verify_write_rules, "PRE: none | POST: none") {
    uint64_t slot = 10;
    const gs::WriteContext running{gs::StatePhase::RUNNING, 0};
    const gs::WriteContext boot{gs::StatePhase::BOOT, 0};
    JARVIS_ASSERT(!gs::verify_and_write(slot, uint64_t{10},
                                        gs::WriteClass::IDEMPOTENT, running,
                                        "probe"));
    JARVIS_ASSERT_EQ(10ULL, slot);
    JARVIS_ASSERT(gs::verify_and_write(slot, uint64_t{11},
                                       gs::WriteClass::IDEMPOTENT, running,
                                       "probe"));
    JARVIS_ASSERT_EQ(11ULL, slot);
    JARVIS_ASSERT(!gs::verify_and_write(slot, uint64_t{12},
                                        gs::WriteClass::BOOT_ONLY, running,
                                        "probe"));
    JARVIS_ASSERT_EQ(11ULL, slot);
    JARVIS_ASSERT(gs::verify_and_write(slot, uint64_t{12},
                                       gs::WriteClass::BOOT_ONLY, boot,
                                       "probe"));
    JARVIS_ASSERT_EQ(12ULL, slot);
    JARVIS_ASSERT(!gs::verify_and_write(slot, uint64_t{13},
                                        gs::WriteClass::NEVER_WRITE, boot,
                                        "probe"));
    JARVIS_ASSERT_EQ(12ULL, slot);
    JARVIS_ASSERT(gs::verify_and_write(slot, uint64_t{14},
                                       gs::WriteClass::PLAIN, running,
                                       "probe"));
    JARVIS_ASSERT_EQ(14ULL, slot);
    JARVIS_ASSERT(gs::verify_and_write(slot, uint64_t{15},
                                       gs::WriteClass::RANGE_CHECKED, running,
                                       "probe"));
    JARVIS_ASSERT_EQ(15ULL, slot);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Boot-only setters reject post-boot writes — the boot epoch and
//           multiboot record are immutable once RUNNING.
// Input: try_set_boot_epoch / try_set_multiboot with a RUNNING context.
// Expect: Both return false; getters and boot_info() unaffected.
// Depends: kernel::gs BootState accessors (issue #136)
JARVIS_TEST(global_boot_epoch_boot_only, "PRE: none | POST: none") {
    const uint64_t epoch_before = gs::get_boot_epoch();
    const uint64_t magic_before = gs::get_multiboot_magic();
    const uint64_t info_before = gs::get_multiboot_info_ptr();
    const gs::WriteContext running{gs::StatePhase::RUNNING, 0};
    JARVIS_ASSERT(!gs::try_set_boot_epoch(epoch_before + 1, running));
    JARVIS_ASSERT_EQ(epoch_before, gs::get_boot_epoch());
    JARVIS_ASSERT(!gs::try_set_multiboot(0x36D76289ULL, 0x1000ULL, running));
    JARVIS_ASSERT_EQ(magic_before, gs::get_multiboot_magic());
    JARVIS_ASSERT_EQ(info_before, gs::get_multiboot_info_ptr());
    BootInfo &first = gs::boot_info();
    BootInfo &second = gs::boot_info();
    JARVIS_ASSERT(&first == &second);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The NIC pointer is RANGE_CHECKED — user-half addresses are
//           rejected, null is always accepted (clears the registration).
// Input: try_set_nic with a low-canonical pointer, then nullptr.
// Expect: Rejection leaves get_nic() unchanged; null stores; old value
//         restored via ScopeGuard so later classes see the boot NIC.
// Depends: kernel::gs NetState accessors (issue #136)
JARVIS_TEST(global_nic_range_checked, "PRE: none | POST: none") {
    ::net::Nic *before = gs::get_nic();
    ScopeGuard restore_nic([before]() { gs::try_set_nic(before); });
    auto *low_nic = reinterpret_cast<::net::Nic *>(0x1000ULL);
    JARVIS_ASSERT(!gs::try_set_nic(low_nic));
    JARVIS_ASSERT(gs::get_nic() == before);
    JARVIS_ASSERT(gs::try_set_nic(nullptr));
    JARVIS_ASSERT(gs::get_nic() == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Same RANGE_CHECKED contract for the FAT32 partition pointer.
// Input: try_set_fat32_partition with a low-canonical pointer, then null.
// Expect: Rejection leaves the getter unchanged; null stores; boot value
//         restored via ScopeGuard.
// Depends: kernel::gs VfsState accessors (issue #136)
JARVIS_TEST(global_fat32_range_checked, "PRE: none | POST: none") {
    kernel::fat32::Fat32Partition *before = gs::get_fat32_partition();
    ScopeGuard restore_part(
        [before]() { gs::try_set_fat32_partition(before); });
    auto *low_part =
        reinterpret_cast<kernel::fat32::Fat32Partition *>(0x2000ULL);
    JARVIS_ASSERT(!gs::try_set_fat32_partition(low_part));
    JARVIS_ASSERT(gs::get_fat32_partition() == before);
    JARVIS_ASSERT(gs::try_set_fat32_partition(nullptr));
    JARVIS_ASSERT(gs::get_fat32_partition() == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The canary-trip latch (MP-3) sets atomically field-by-field and
//           resets to zero; the SMAP recovery IP is readable by tests.
// Input: reset, set(0xA11CE, 3, 0x1234), read, reset.
// Expect: Fields match after set; all zero after reset; latch left clean.
// Depends: kernel::gs FaultState accessors (issue #136)
JARVIS_TEST(global_canary_latch_set_reset, "PRE: none | POST: none") {
    gs::reset_canary_trip();
    gs::set_canary_trip(0xA11CEULL, 3, 0x1234ULL);
    const kernel::CanaryTrip &trip = gs::canary_trip();
    JARVIS_ASSERT_EQ(0xA11CEULL, trip.task_id);
    JARVIS_ASSERT(trip.segment == 3);
    JARVIS_ASSERT_EQ(0x1234ULL, trip.rip);
    JARVIS_ASSERT(trip.count >= 1);
    gs::reset_canary_trip();
    const kernel::CanaryTrip &clean = gs::canary_trip();
    JARVIS_ASSERT_EQ(0ULL, clean.task_id);
    JARVIS_ASSERT_EQ(0ULL, clean.count);
    JARVIS_ASSERT_EQ(0ULL, clean.rip);
    (void)gs::user_access_recover_ip();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: TestState getters/setters round-trip; every mutation is restored
//           via ScopeGuard so the harness (class name, bench filter,
//           shutdown flag, VFS marker) is unaffected.
// Input: Toggle filter_bench / auto_shutdown / vfs_touched, set a probe
//        class name, snapshot the kernel-entry timestamp.
// Expect: Getters reflect each write; entry timestamp non-zero.
// Depends: kernel::gs TestState accessors (issue #136)
JARVIS_TEST(global_teststate_save_restore, "PRE: none | POST: none") {
    const bool bench_before = gs::get_filter_bench();
    const bool shutdown_before = gs::get_class_auto_shutdown();
    const bool touched_before = gs::get_vfs_touched();
    const char *class_before = gs::get_current_class();
    ScopeGuard restore_state([=]() {
        gs::set_filter_bench(bench_before);
        gs::set_class_auto_shutdown(shutdown_before);
        gs::mark_vfs_touched(touched_before);
        gs::set_current_class(class_before);
    });
    gs::set_filter_bench(!bench_before);
    JARVIS_ASSERT(gs::get_filter_bench() == !bench_before);
    gs::set_class_auto_shutdown(!shutdown_before);
    JARVIS_ASSERT(gs::get_class_auto_shutdown() == !shutdown_before);
    gs::mark_vfs_touched(true);
    JARVIS_ASSERT(gs::get_vfs_touched());
    gs::set_current_class("gs_probe");
    JARVIS_ASSERT(has(gs::get_current_class(), "gs_probe"));
    gs::set_kernel_entry_ns();
    JARVIS_ASSERT(gs::get_kernel_entry_ns() != 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The ISR-epilogue diagnostic hooks and the apply-side re-check
//           are directly callable and side-effect-free when no deferred
//           switch is armed (CONFIG_DEBUG_IPC_SCHED is off, so the diag
//           hooks are no-ops; validate takes the id==UINT64_MAX early-out).
//           Runs under IrqGuard to match the hooks' IF=0 contract.
// Input: Call each hook; capture the validate verdict.
// Expect: No crash; verdict is 0 (no arm) or 1 (valid arm), never else.
//         scheduler_on_context_switch is NOT called: with a pending arm it
//         would retarget current_task without switching registers.
// Depends: kernel::gs AsmSwitchState hooks (issue #136)
JARVIS_TEST(global_diag_hooks_safe, "PRE: none | POST: none") {
    {
        arch::IrqGuard irq_guard{};
        gs::scheduler_diag_pre_save();
        gs::scheduler_diag_depth_skip();
        scheduler_diag_rsp_abort();
        gs::scheduler_record_skip(0, 0);
        gs::scheduler_abort_switch_fixup();
        const int verdict = gs::scheduler_validate_pending_switch();
        JARVIS_ASSERT(verdict == 0 || verdict == 1);
    }
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
    JARVIS_REGISTER_TEST(kernel_irq_thread_unknown_vector_noop);
    JARVIS_REGISTER_TEST(kernel_irq_thread_isr_ack_synchronous);
    JARVIS_REGISTER_TEST(kernel_irq_thread_task_guard_and_double_destroy);
    JARVIS_REGISTER_TEST(global_verify_write_rules);
    JARVIS_REGISTER_TEST(global_boot_epoch_boot_only);
    JARVIS_REGISTER_TEST(global_nic_range_checked);
    JARVIS_REGISTER_TEST(global_fat32_range_checked);
    JARVIS_REGISTER_TEST(global_canary_latch_set_reset);
    JARVIS_REGISTER_TEST(global_teststate_save_restore);
    JARVIS_REGISTER_TEST(global_diag_hooks_safe);
}
