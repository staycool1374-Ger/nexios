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

/// @file test_ipc_throughput.cpp
/// @brief IPC throughput benchmarks (issue #303, Phase 6 module 3).
///
///        test_ipc_benchmark.cpp covers latency only (min/avg/max cycles
///        for 0 B + 64 B self-task round-trips). This file publishes the
///        missing throughput figures as msgs/s tables calibrated against
///        the TSC frequency, plus payload-size and queue-depth sweeps.
///        Transport limits pinned here: IPC_MAX_MSG_SIZE = 64 B (a 1 KiB
///        single message is unrepresentable — the sweep tops out at 64 B)
///        and IPC_MAX_QUEUE_MSG = 16 (depth sweep tops out at 16).
///        Kernel<->user crossing is measured from Ring 3 in
///        test_cs_bench.cpp (write-crossing) and test_cross_boundary.cpp
///        (vfsd chains), not duplicated here.

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

constexpr size_t k_warmup = 200;

uint64_t throughput_tsc_hz() {
#if defined(CONFIG_ARCH_X86_64)
    return arch::Timer::tsc_freq_hz();
#else
    return 0;
#endif
}

// Drain every message pending for the current task (test isolation:
// a burst must never leak into the next test via our own inbox).
void drain_self_inbox() {
    Message out;
    while (IPC::recv(out))
        ;
}

} // namespace

// Runmode: kernel
// Testidea: Sustained in-task send+recv rate (0 B payload) as msgs/s,
// calibrated by the TSC frequency.
// Input: 20000 self round-trips timed by rdtsc.
// Expect: All complete; rate > 20000 msgs/s (fail-closed sanity bound —
// the published figure, not the bound, is the deliverable).
// Depends: IPC::send/recv, Timer::tsc_freq_hz
JARVIS_TEST(ipc_throughput_self_rate, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    drain_self_inbox();

    Message msg{};
    msg.sender_id = cur->id;
    for (size_t i = 0; i < k_warmup; ++i) {
        JARVIS_ASSERT(IPC::send(cur->id, msg, IpcFlags::NONBLOCK));
        Message out;
        JARVIS_ASSERT(IPC::recv(out));
    }

    constexpr size_t k_n = 20000;
    uint64_t t0 = arch::rdtsc();
    for (size_t i = 0; i < k_n; ++i) {
        bool sent = IPC::send(cur->id, msg, IpcFlags::NONBLOCK);
        Message out;
        bool ok = IPC::recv(out);
        JARVIS_ASSERT(ok && sent);
    }
    uint64_t dt = arch::rdtsc() - t0;
    JARVIS_ASSERT(dt > 0);
    drain_self_inbox();

    uint64_t hz = throughput_tsc_hz();
    uint64_t cycles_per_msg = dt / k_n;
    Logger::info("THROUGHPUT self 0B: msgs=%u cycles=%lu cycles/msg=%lu",
                 k_n, dt, cycles_per_msg);
    if (hz > 0) {
        uint64_t per_s = (static_cast<uint64_t>(k_n) * hz) / dt;
        Logger::info("THROUGHPUT self 0B: %lu msgs/s (tsc=%lu Hz)", per_s,
                     hz);
        JARVIS_ASSERT_FMT(per_s > 20000,
                          "self throughput %lu msgs/s below sanity bound",
                          per_s);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Per-message cost across the full representable payload range.
// Input: 3000 self round-trips each at 0/1/16/32/64 B payloads.
// Expect: Every size completes; each avg < 100000 cycles.
// Depends: IPC::send/recv (payload copy path)
JARVIS_TEST(ipc_throughput_size_sweep, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    drain_self_inbox();

    static const size_t k_sizes[] = {0, 1, 16, 32, 64};
    for (size_t s = 0; s < 5; ++s) {
        Message msg{};
        msg.sender_id = cur->id;
        msg.type = s;
        msg.data_size = k_sizes[s];
        for (size_t i = 0; i < k_sizes[s]; ++i)
            msg.data[i] = static_cast<uint8_t>(i & 0xFF);
        for (size_t i = 0; i < k_warmup; ++i) {
            JARVIS_ASSERT(IPC::send(cur->id, msg, IpcFlags::NONBLOCK));
            Message out;
            JARVIS_ASSERT(IPC::recv(out));
        }
        constexpr size_t k_n = 3000;
        uint64_t t0 = arch::rdtsc();
        for (size_t i = 0; i < k_n; ++i) {
            bool sent = IPC::send(cur->id, msg, IpcFlags::NONBLOCK);
            Message out;
            bool ok = IPC::recv(out);
            JARVIS_ASSERT(ok && sent);
            JARVIS_ASSERT(out.data_size == k_sizes[s]);
        }
        uint64_t dt = arch::rdtsc() - t0;
        uint64_t avg = dt / k_n;
        Logger::info("THROUGHPUT size %u B: avg=%lu cycles/msg", k_sizes[s],
                     avg);
        JARVIS_ASSERT_FMT(avg < 100000,
                          "size %u: avg %lu cycles above sanity bound",
                          k_sizes[s], avg);
    }
    drain_self_inbox();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Drain cost at several queue occupancies (depth sweep 1/4/8/16:
// the transport maximum is IPC_MAX_QUEUE_MSG = 16).
// Input: Prefill the inbox to each depth, time the full non-blocking drain.
// Expect: Every depth drains exactly; per-msg drain avg < 100000 cycles.
// Depends: MessageQueue push/pop via IPC::send/recv
JARVIS_TEST(ipc_throughput_depth_sweep, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    drain_self_inbox();

    static const size_t k_depths[] = {1, 4, 8, 16};
    for (size_t d = 0; d < 4; ++d) {
        for (size_t i = 0; i < k_depths[d]; ++i) {
            Message msg{};
            msg.sender_id = cur->id;
            msg.type = i;
            msg.data_size = 0;
            bool ok = IPC::send(cur->id, msg, IpcFlags::NONBLOCK);
            JARVIS_ASSERT_FMT(ok, "prefill %u/%u rejected", i, k_depths[d]);
        }
        uint64_t t0 = arch::rdtsc();
        size_t drained = 0;
        Message out;
        while (IPC::recv(out))
            ++drained;
        uint64_t dt = arch::rdtsc() - t0;
        JARVIS_ASSERT_FMT(drained == k_depths[d],
                          "depth %u: drained %u", k_depths[d], drained);
        uint64_t avg = (dt == 0) ? 0 : dt / drained;
        Logger::info("THROUGHPUT depth %u: drained=%u avg=%lu cycles/msg",
                     k_depths[d], drained, avg);
        JARVIS_ASSERT_FMT(avg < 100000,
                          "depth %u: drain avg %lu above sanity bound",
                          k_depths[d], avg);
    }
    JARVIS_TEST_PASS();
}

namespace {

// Scheduler-mediated peer crossing: the sender issues send_sync, the
// receiver echoes. Each round-trip is two full dispatches.
struct PeerCrossing {
    static constexpr size_t k_trips = 20;
    static uint64_t receiver_id;
    static uint64_t completed;
};

uint64_t PeerCrossing::receiver_id = 0;
uint64_t PeerCrossing::completed = 0;

void peer_receiver_entry() {
    for (size_t i = 0; i < PeerCrossing::k_trips; ++i) {
        Message msg;
        bool ok = false;
        // Tick-yielded poll (NOT a tight spin): each hlt surrenders the
        // CPU until the next timer tick so the send_sync-blocked sender
        // is dispatched, consumes the previous reply and posts the next
        // message. A tight spin would exhaust on the empty inbox while
        // the sender is still blocked awaiting our reply (self-deadlock:
        // we wait for msg i+1 before sending reply i). Bounded —
        // fail-closed, never hangs the suite.
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

void peer_sender_entry() {
    for (size_t i = 0; i < PeerCrossing::k_trips; ++i) {
        Message msg{};
        msg.sender_id = Scheduler::current_task()->id;
        msg.type = i;
        msg.data_size = 0;
        Message reply;
        bool ok = IPC::send_sync(PeerCrossing::receiver_id, msg, reply);
        JARVIS_ASSERT(ok);
        JARVIS_ASSERT(reply.type == i);
    }
    PeerCrossing::completed = PeerCrossing::k_trips;
}

} // namespace

// Runmode: kernel
// Testidea: Scheduler-mediated kernel<->kernel crossing rate: send_sync
// ping-pong between two tasks, timed end to end.
// Input: 20 echo round-trips (2 dispatches each) timed by rdtsc.
// Expect: All trips complete with matching types; publish trips/s (=
// crossings/s / 2) and ctx-switches/s (= trips/s * 2).
// Depends: IPC::send_sync, driven task-pair pattern (cookbook Rule 2)
JARVIS_TEST(ipc_throughput_peer_crossing, "PRE: none | POST: none") {
    PeerCrossing::completed = 0;
    auto *receiver =
        TaskControlBlock::create(peer_receiver_entry, 11, 10);
    JARVIS_ASSERT(receiver != nullptr);
    PeerCrossing::receiver_id = receiver->id;
    auto *sender = TaskControlBlock::create(peer_sender_entry, 12, 5);
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
    uint64_t dt = arch::rdtsc() - t0;
    Scheduler::set_current(*original);

    JARVIS_ASSERT(sender->state == TaskState::TERMINATED);
    JARVIS_ASSERT(receiver->state == TaskState::TERMINATED);
    JARVIS_ASSERT(PeerCrossing::completed == PeerCrossing::k_trips);

    uint64_t hz = throughput_tsc_hz();
    uint64_t per_trip = dt / PeerCrossing::k_trips;
    Logger::info("THROUGHPUT peer crossing: trips=%u cycles=%lu "
                 "cycles/trip=%lu",
                 PeerCrossing::k_trips, dt, per_trip);
    if (hz > 0 && dt > 0) {
        uint64_t trips_s =
            (static_cast<uint64_t>(PeerCrossing::k_trips) * hz) / dt;
        Logger::info("THROUGHPUT peer crossing: %lu trips/s (%lu "
                     "ctx-switches/s)",
                     trips_s, trips_s * 2);
        JARVIS_ASSERT_FMT(trips_s > 0, "peer crossing made no progress");
    }

    kernel::test::terminate_and_drain2(sender, receiver);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all IPC throughput benchmarks (TF_BENCH: run in the
// bench class, filtered out of normal classes).
// Input: None
// Expect: All throughput benchmarks registered
// Depends: test framework
void register_ipc_throughput_tests() {
    Logger::info("Registering IPC throughput benchmarks");
    JARVIS_REGISTER_TEST_FLAGS(ipc_throughput_self_rate,
                               kernel::test::TF_BENCH);
    JARVIS_REGISTER_TEST_FLAGS(ipc_throughput_size_sweep,
                               kernel::test::TF_BENCH);
    JARVIS_REGISTER_TEST_FLAGS(ipc_throughput_depth_sweep,
                               kernel::test::TF_BENCH);
    JARVIS_REGISTER_TEST_FLAGS(ipc_throughput_peer_crossing,
                               kernel::test::TF_BENCH);
}
