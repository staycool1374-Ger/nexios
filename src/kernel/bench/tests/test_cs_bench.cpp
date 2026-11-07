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

/// @file test_cs_bench.cpp
/// @brief Context-switch benchmarks (issue #303, Phase 6 module 4).
///
///        bench_irq_latency.cpp covers the reschedule-arm proxy (avg>0
///        only) and the boot-collected IRQ histogram; timing_core and
///        jitter cover tick accounting. This file publishes the missing
///        per-path figures with min/max/avg/p99:
///        - task<->task: send_sync ping-pong round-trips (2 full
///          dispatches each), sorted samples, p99.
///        - syscall entry/exit: Ring-3 getpid loop timed by rdtsc in the
///          libc_verify image (CS_BENCH markers).
///        - kernel<->user crossing: Ring-3 single-write round-trip through
///          the vfsd send_sync path, timed in-image (CS_BENCH markers).
///        IRQ entry/exit stays with bench_irq_histogram_dump (boot-
///        collected timer-IRQ latencies, same source — not duplicated).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

// Task<->task ping-pong: sender timestamps each send_sync echo trip.
struct PingPong {
    static constexpr size_t k_trips = 100;
    static uint64_t receiver_id;
    static uint64_t samples[k_trips];
    static uint64_t done;
};

uint64_t PingPong::receiver_id = 0;
uint64_t PingPong::samples[PingPong::k_trips] = {};
uint64_t PingPong::done = 0;

void ping_receiver_entry() {
    for (size_t i = 0; i < PingPong::k_trips; ++i) {
        Message msg;
        bool ok = false;
        // Tick-yielded poll (see test_ipc_throughput.cpp PeerCrossing:
        // a tight spin deadlocks multi-trip ping-pongs).
        for (uint64_t w = 0; w < 500 && !ok; ++w) {
            ok = IPC::recv(msg);
            if (!ok) {
                Scheduler::reschedule();
                arch::hlt();
            }
        }
        JARVIS_ASSERT(ok);
        Message reply{};
        reply.sender_id = Scheduler::current_task()->id;
        reply.type = msg.type;
        reply.data_size = 0;
        JARVIS_ASSERT(IPC::send(msg.sender_id, reply));
    }
}

void ping_sender_entry() {
    for (size_t i = 0; i < PingPong::k_trips; ++i) {
        Message msg{};
        msg.sender_id = Scheduler::current_task()->id;
        msg.type = i;
        msg.data_size = 0;
        Message reply;
        uint64_t t0 = arch::rdtsc();
        bool ok = IPC::send_sync(PingPong::receiver_id, msg, reply);
        PingPong::samples[i] = arch::rdtsc() - t0;
        JARVIS_ASSERT(ok);
        JARVIS_ASSERT(reply.type == i);
    }
    PingPong::done = PingPong::k_trips;
}

} // namespace

// Runmode: kernel
// Testidea: Task<->task round-trip latency distribution over 100 echo
// trips (2 scheduler-mediated dispatches each).
// Input: Driven sender/receiver pair; per-trip rdtsc samples.
// Expect: All trips complete in order; publish min/max/avg/p99 +
// round-trips/s; avg sanity bound (fail-closed, not a perf gate).
// Depends: IPC::send_sync, driven task-pair pattern (cookbook Rule 2)
JARVIS_TEST(cs_bench_task_pingpong_p99, "PRE: none | POST: none") {
    PingPong::done = 0;
    auto *receiver = TaskControlBlock::create(ping_receiver_entry, 11, 10);
    JARVIS_ASSERT(receiver != nullptr);
    PingPong::receiver_id = receiver->id;
    auto *sender = TaskControlBlock::create(ping_sender_entry, 12, 5);
    JARVIS_ASSERT(sender != nullptr);
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*receiver);
        Scheduler::add_task(*sender);
    }

    auto *original = Scheduler::current_task();
    kernel::test::yield_as(*receiver);
    uint64_t t0 = arch::rdtsc();
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(sender);
    kernel::test::wait_for_termination_safe(receiver);
    uint64_t wall = arch::rdtsc() - t0;
    Scheduler::set_current(*original);

    JARVIS_ASSERT(sender->state == TaskState::TERMINATED);
    JARVIS_ASSERT(receiver->state == TaskState::TERMINATED);
    JARVIS_ASSERT(PingPong::done == PingPong::k_trips);

    // Insertion sort (n=100, harness-side, allocation-free).
    for (size_t i = 1; i < PingPong::k_trips; ++i) {
        uint64_t key = PingPong::samples[i];
        size_t j = i;
        while (j > 0 && PingPong::samples[j - 1] > key) {
            PingPong::samples[j] = PingPong::samples[j - 1];
            --j;
        }
        PingPong::samples[j] = key;
    }
    uint64_t mn = PingPong::samples[0];
    uint64_t mx = PingPong::samples[PingPong::k_trips - 1];
    uint64_t sum = 0;
    for (size_t i = 0; i < PingPong::k_trips; ++i)
        sum += PingPong::samples[i];
    uint64_t avg = sum / PingPong::k_trips;
    // p99: index 98 of 100 sorted samples.
    uint64_t p99 = PingPong::samples[98];
    Logger::info("CS task<->task: n=%u min=%lu max=%lu avg=%lu p99=%lu "
                 "cycles",
                 PingPong::k_trips, mn, mx, avg, p99);
    uint64_t hz = 0;
#if defined(CONFIG_ARCH_X86_64)
    hz = arch::Timer::tsc_freq_hz();
#endif
    if (hz > 0 && wall > 0) {
        uint64_t trips_s =
            (static_cast<uint64_t>(PingPong::k_trips) * hz) / wall;
        Logger::info("CS task<->task: %lu trips/s (%lu ctx-switches/s)",
                     trips_s, trips_s * 2);
    }
    JARVIS_ASSERT_FMT(avg < 200000000ULL,
                      "ping-pong avg %lu cycles above sanity bound", avg);
    JARVIS_ASSERT_FMT(p99 >= mn && mx >= p99, "p99 outside [min,max]");

    kernel::test::terminate_and_drain2(sender, receiver);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all context-switch benchmarks (TF_BENCH).
// The Ring-3 syscall/crossing measurements live in the libc_verify
// image (CS_BENCH markers, parsed by test_libc_syscall.cpp); the
// task<->task distribution is measured above.
// Input: None
// Expect: Task ping-pong benchmark registered
// Depends: test framework
void register_cs_bench_tests() {
    Logger::info("Registering context-switch benchmarks");
    JARVIS_REGISTER_TEST_FLAGS(cs_bench_task_pingpong_p99,
                               kernel::test::TF_BENCH);
}
