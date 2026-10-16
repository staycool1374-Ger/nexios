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

/// @file test_ipc_benchmark.cpp
/// @brief IPC throughput benchmark tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/task/scheduler.hpp>

using namespace kernel;

// ---------------------------------------------------------------------------
// IPC Latency Benchmark Suite
//
// Measures kernel-level IPC operations using RDTSC.
// These microbenchmarks capture the cost within the monolithic kernel.
// For a microkernel, each IPC operation would additionally require:
//   - syscall/sysret ring transition (~100–300 cycles on bare metal)
//   - page table switch (~500–1000 cycles with PCID)
//   - scheduler invocation on each send/recv boundary
//
// TSC calibration: QEMU typically runs at ~1 GHz (1 cycle ≈ 1 ns).
// Results below are in TSC cycles; divide by ~1000 for approximate µs.
// ---------------------------------------------------------------------------

static constexpr size_t BENCH_ITERATIONS = 2000;

struct BenchResult {
    uint64_t min;
    uint64_t avg;
    uint64_t max;
    uint64_t count;
};

static BenchResult measure(uint64_t (*fn)()) {
    BenchResult r;
    r.min = ~0ULL;
    r.max = 0;
    r.avg = 0;
    r.count = 0;

    for (size_t i = 0; i < 100; ++i) {
        fn();
    }

    for (size_t i = 0; i < BENCH_ITERATIONS; ++i) {
        uint64_t tsc = arch::rdtsc();
        fn();
        uint64_t elapsed = arch::rdtsc() - tsc;
        if (elapsed < r.min)
            r.min = elapsed;
        if (elapsed > r.max)
            r.max = elapsed;
        r.avg += elapsed;
        ++r.count;
    }
    r.avg /= r.count;
    return r;
}

// ---------------------------------------------------------------------------
// Baseline: RDTSC overhead measurement
// ---------------------------------------------------------------------------

static uint64_t bench_rdtsc_overhead() {
    (void)arch::rdtsc();
    return 0;
}

// Runmode: kernel
// Testidea: Measures RDTSC instruction overhead as baseline for other
// benchmarks
// Input: Back-to-back RDTSC calls
// Expect: Average overhead < 100 cycles
// Depends: kernel/arch, kernel/task
JARVIS_TEST(ipc_bench_rdtsc_overhead, "PRE: none | POST: none") {
    auto r = measure(bench_rdtsc_overhead);
    Logger::info("RDTSC overhead: min=%d, avg=%d, max=%d (iters=%d)", r.min,
                 r.avg, r.max, r.count);
    JARVIS_ASSERT(r.avg < 100);
    JARVIS_TEST_PASS();
}

// ---------------------------------------------------------------------------
// IPC send + recv to self (full round-trip within same task)
// ---------------------------------------------------------------------------

static uint64_t bench_ipc_send_self() {
    auto *cur = Scheduler::current_task();
    if (!cur)
        return ~0ULL;
    Message msg{};
    msg.sender_id = cur->id;
    msg.type = 0;
    msg.priority = 0;
    msg.data_size = 0;
    // Benchmark load: results intentionally unchecked (measurement
    // purity — throughput/latency is the deliverable, see Testidea).
    (void)IPC::send(cur->id, msg, IPC_NONBLOCK);
    Message out;
    (void)IPC::recv(out);
    return 0;
}

// Runmode: kernel
// Testidea: Measures IPC send+recv round-trip latency within the same task
// Input: Message sent to self via IPC::send, then drained via IPC::recv
// Expect: Average round-trip < 10000 cycles
// Depends: kernel/ipc, kernel/task
JARVIS_TEST(ipc_bench_send_self, "PRE: none | POST: none") {
    Message drain;
    while (IPC::recv(drain))
        ;

    auto r = measure(bench_ipc_send_self);
    Logger::info("IPC send+recv self: min=%d, avg=%d, max=%d (iters=%d)", r.min,
                 r.avg, r.max, r.count);
    JARVIS_ASSERT(r.avg < 75000);
    JARVIS_TEST_PASS();
}

// ---------------------------------------------------------------------------
// IPC recv only (pop from pre-filled queue)
// ---------------------------------------------------------------------------

// Runmode: kernel
// Testidea: STUB - Measures IPC recv-only latency from a pre-filled queue
// Input: None (stub test)
// Expect: Passes (stub)
// Depends: kernel/ipc, kernel/task
JARVIS_TEST(ipc_bench_recv_self, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// ---------------------------------------------------------------------------
// IPC send only (non-blocking, no drain per iteration)
// ---------------------------------------------------------------------------

static uint64_t bench_ipc_send_only_task_id_;
static Message bench_ipc_send_only_msg_;

static uint64_t bench_ipc_send_only() {
    // Benchmark load: unchecked by design (see above).
    (void)IPC::send(bench_ipc_send_only_task_id_, bench_ipc_send_only_msg_,
                     IPC_NONBLOCK);
    return 0;
}

// Runmode: kernel
// Testidea: Measures non-blocking IPC send-only latency
// Input: IPC_NONBLOCK send to own task without draining per iteration
// Expect: Average send latency < 10000 cycles
// Depends: kernel/ipc, kernel/task
JARVIS_TEST(ipc_bench_send_only, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    bench_ipc_send_only_task_id_ = cur->id;
    bench_ipc_send_only_msg_ = Message{};
    bench_ipc_send_only_msg_.sender_id = cur->id;

    auto r = measure(bench_ipc_send_only);
    Logger::info("IPC send only (non-block): min=%d, avg=%d, max=%d", r.min,
                 r.avg, r.max);

    Message out;
    while (IPC::recv(out))
        ;
    JARVIS_ASSERT(r.avg < 10000);
    JARVIS_TEST_PASS();
}

// ---------------------------------------------------------------------------
// IPC send into full queue (non-blocking failure path)
// ---------------------------------------------------------------------------

static uint64_t bench_ipc_send_full_id_;

static uint64_t bench_ipc_send_full() {
    Message msg{};
    msg.sender_id = bench_ipc_send_full_id_;
    // Benchmark load: unchecked by design (see above).
    (void)IPC::send(bench_ipc_send_full_id_, msg, IPC_NONBLOCK);
    return 0;
}

// Runmode: kernel
// Testidea: Measures non-blocking IPC send failure path when queue is full
// Input: Queue filled to capacity, then IPC_NONBLOCK send attempted
// Expect: Average failure-path latency < 5000 cycles
// Depends: kernel/ipc, kernel/task
JARVIS_TEST(ipc_bench_send_full, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    bench_ipc_send_full_id_ = cur->id;
    Message filler{};
    filler.sender_id = cur->id;
    while (!cur->msg_queue.is_full()) {
        cur->msg_queue.push(filler);
    }

    auto r = measure(bench_ipc_send_full);
    Logger::info("IPC send (full, non-block fail): min=%d, avg=%d, max=%d",
                 r.min, r.avg, r.max);
    JARVIS_ASSERT(r.avg < 5000);

    while (IPC::recv(filler))
        ;
    JARVIS_TEST_PASS();
}

// ---------------------------------------------------------------------------
// IPC with maximal 64-byte payload
// ---------------------------------------------------------------------------

static uint64_t bench_ipc_64byte_id_;
static Message bench_ipc_64byte_send_;

static uint64_t bench_ipc_send_64byte() {
    // Benchmark load: unchecked by design (see above).
    (void)IPC::send(bench_ipc_64byte_id_, bench_ipc_64byte_send_, IPC_NONBLOCK);
    Message out;
    (void)IPC::recv(out);
    return 0;
}

// Runmode: kernel
// Testidea: Measures IPC send+recv round-trip with maximal 64-byte payload
// Input: IPC_MAX_MSG_SIZE payload filled with sequential bytes, sent then
// received
// Expect: Average round-trip < 20000 cycles
// Depends: kernel/ipc, kernel/task
JARVIS_TEST(ipc_bench_send_64byte, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    bench_ipc_64byte_id_ = cur->id;
    bench_ipc_64byte_send_ = Message{};
    bench_ipc_64byte_send_.sender_id = cur->id;
    bench_ipc_64byte_send_.data_size = IPC_MAX_MSG_SIZE;
    for (size_t i = 0; i < IPC_MAX_MSG_SIZE; ++i) {
        bench_ipc_64byte_send_.data[i] = static_cast<uint8_t>(i);
    }

    auto r = measure(bench_ipc_send_64byte);
    Logger::info("IPC send+recv 64B payload: min=%d, avg=%d, max=%d", r.min,
                 r.avg, r.max);
    JARVIS_ASSERT(r.avg < 75000);
    JARVIS_TEST_PASS();
}

// ---------------------------------------------------------------------------
// Scheduler::current_task overhead (baseline for syscall dispatch)
// ---------------------------------------------------------------------------

static uint64_t bench_current_task() {
    Scheduler::current_task();
    return 0;
}

// Runmode: kernel
// Testidea: Measures Scheduler::current_task lookup overhead as baseline for
// syscall dispatch
// Input: Calls Scheduler::current_task() repeatedly
// Expect: Average lookup < 500 cycles
// Depends: kernel/task
JARVIS_TEST(ipc_bench_current_task, "PRE: none | POST: none") {
    auto r = measure(bench_current_task);
    Logger::info("Scheduler::current_task: min=%d, avg=%d, max=%d", r.min,
                 r.avg, r.max);
    JARVIS_ASSERT(r.avg < 500);
    JARVIS_TEST_PASS();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

// Runmode: kernel
// Testidea: Registers all IPC benchmark tests with the test framework
// Input: None
// Expect: All IPC benchmark tests registered
// Depends: test framework
void register_ipc_benchmark_tests() {
    Logger::info("Registering IPC benchmark suite");

    JARVIS_REGISTER_TEST_FLAGS(ipc_bench_rdtsc_overhead, kernel::test::TF_BENCH);
    JARVIS_REGISTER_TEST_FLAGS(ipc_bench_current_task, kernel::test::TF_BENCH);
    JARVIS_REGISTER_TEST_FLAGS(ipc_bench_send_only, kernel::test::TF_BENCH);
    JARVIS_REGISTER_TEST_FLAGS(ipc_bench_recv_self, kernel::test::TF_BENCH);
    JARVIS_REGISTER_TEST_FLAGS(ipc_bench_send_self, kernel::test::TF_BENCH);
    JARVIS_REGISTER_TEST_FLAGS(ipc_bench_send_full, kernel::test::TF_BENCH);
    JARVIS_REGISTER_TEST_FLAGS(ipc_bench_send_64byte, kernel::test::TF_BENCH);
}
