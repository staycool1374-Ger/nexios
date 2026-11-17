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

/// @file bench_mk_ipc_baseline.cpp
/// @brief M0 (F17/#570): IPC baseline workload + microkernel-overhead harness.
///
/// Frozen workload descriptor (WL-1..WL-4), relay-chain drivers, tick/counter
/// instrumentation, percentile + switch-rate + core-locality measurements.
/// Measurement-only: zero edits under task/ipc/syscall/arch/memory/sync; the
/// deferred-switch path (INV-MK-5) is observed, never changed. TCG numbers
/// are non-normative; Q3 numeric targets freeze against HW baselines at M3.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_debug_internal.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

// Frozen workload descriptor (the M0 "defined workload" deliverable).
static constexpr uint64_t k_trips = 100;
static constexpr uint64_t k_trips_rate = 200;
static constexpr size_t k_depths[] = {1, 2, 3, 4};
static constexpr size_t k_payload_bytes = CONFIG_IPC_FAST_PAYLOAD_BYTES;
static constexpr uint64_t k_client_prio = 30;
static constexpr uint64_t k_relay_prio0 = 29;
static constexpr size_t k_max_relays = 4;
static constexpr size_t k_max_hops = 8;
static constexpr size_t k_samples = 100;

static constexpr uint64_t k_tag_req(size_t relay_idx) { return 100 + relay_idx; }
static constexpr uint64_t k_tag_rep(size_t relay_idx) { return 200 + relay_idx; }

static_assert(k_payload_bytes <= IPC_MAX_MSG_SIZE,
              "M0 payload must fit the message buffer");

struct RelayCtx {
    size_t index = 0;
    uint64_t next_id = 0;
    uint64_t depth = 0;
    uint64_t trips = 0;
};

struct Stats {
    uint64_t p50 = 0;
    uint64_t p99 = 0;
    uint64_t pmax = 0;
    uint64_t mean = 0;
};

static uint64_t g_trip_ticks[k_samples] = {};
static uint64_t g_trip_ns[k_samples] = {};
static uint64_t g_trips_done = 0;
static uint64_t g_relay_ids[k_max_relays] = {};
static RelayCtx g_relay_ctx[k_max_relays] = {};
static uint64_t g_dispatch_events = 0;
static uint64_t g_client_trips = 0;
static bool g_client_record = false;
static TaskControlBlock *g_chain_relays[k_max_relays] = {};
static TaskControlBlock *g_chain_client = nullptr;
static size_t g_chain_depth = 0;

static void sort_u64(uint64_t *arr, size_t count) {
    for (size_t idx = 1; idx < count; ++idx) {
        uint64_t key = arr[idx];
        size_t pos = idx;
        while (pos > 0 && arr[pos - 1] > key) {
            arr[pos] = arr[pos - 1];
            --pos;
        }
        arr[pos] = key;
    }
}

static Stats summarize(uint64_t *arr, size_t count) {
    Stats result{};
    if (count == 0) {
        return result;
    }
    sort_u64(arr, count);
    result.p50 = arr[(count - 1) * 50 / 100];
    result.p99 = arr[(count - 1) * 99 / 100];
    result.pmax = arr[count - 1];
    uint64_t sum = 0;
    for (size_t idx = 0; idx < count; ++idx) {
        sum += arr[idx];
    }
    result.mean = sum / count;
    return result;
}

static uint64_t ns_from_counter(uint64_t delta) {
    uint64_t freq = arch::Timer::raw_counter_freq_hz();
    if (freq == 0) {
        return 0;
    }
    if (delta > ~0ULL / 1000000000ULL) {
        return ~0ULL;
    }
    return delta * 1000000000ULL / freq;
}

static void drain_inbox() {
    Message tmp{};
    while (IPC::recv(tmp)) {
    }
}

static void relay_entry() {
    auto *self = Scheduler::current_task();
    auto *ctx = reinterpret_cast<RelayCtx *>(self->user_data);
    Message msg{};
    Message inner{};
    Message reply{};
    // NOTE (M3 #556 QE): the server-side REPLY_RECV conversion is parked
    // until the deliver-side generation-check kernel fix lands (see #545
    // 2026-11-18: PushedReply unreachable, reply_recv always STALE). The
    // classic recv + trailing send shape below is the pre-M3 baseline.
    for (uint64_t trip = 0; trip < ctx->trips; ++trip) {
        bool got = false;
        for (int wait = 0; wait < 500; ++wait) {
            if (IPC::recv(msg)) {
                got = true;
                break;
            }
            Scheduler::reschedule();
            arch::hlt();
        }
        JARVIS_ASSERT(got);
        JARVIS_ASSERT_EQ(k_tag_req(ctx->index), msg.type);
        if (ctx->index + 1 < ctx->depth) {
            Message fwd = msg;
            fwd.sender_id = self->id;
            fwd.type = k_tag_req(ctx->index + 1);
            __atomic_fetch_add(&g_dispatch_events, 1ULL, __ATOMIC_RELAXED);
            bool ok = IPC::send_sync(ctx->next_id, fwd, inner);
            JARVIS_ASSERT(ok);
            JARVIS_ASSERT_EQ(k_tag_rep(ctx->index + 1), inner.type);
            reply = inner;
        } else {
            JARVIS_ASSERT_EQ(k_payload_bytes, msg.data_size);
            reply = msg;
        }
        reply.sender_id = self->id;
        reply.type = k_tag_rep(ctx->index);
        reply.priority = 0;
        __atomic_fetch_add(&g_dispatch_events, 1ULL, __ATOMIC_RELAXED);
        bool ok = IPC::send(msg.sender_id, reply);
        JARVIS_ASSERT(ok);
    }
    drain_inbox();
}

static void client_entry() {
    auto *self = Scheduler::current_task();
    Message req{};
    Message rep{};
    req.sender_id = self->id;
    req.priority = 0;
    req.data_size = k_payload_bytes;
    for (uint64_t trip = 0; trip < g_client_trips; ++trip) {
        req.type = k_tag_req(0);
        for (size_t idx = 0; idx < k_payload_bytes; ++idx) {
            req.data[idx] = static_cast<uint8_t>((trip + idx) & 0xFF);
        }
        uint64_t c0 = arch::Timer::raw_counter();
        uint64_t t0 = arch::Timer::ticks();
        __atomic_fetch_add(&g_dispatch_events, 1ULL, __ATOMIC_RELAXED);
        bool ok = IPC::send_sync(g_relay_ids[0], req, rep);
        uint64_t c1 = arch::Timer::raw_counter();
        uint64_t t1 = arch::Timer::ticks();
        JARVIS_ASSERT(ok);
        JARVIS_ASSERT_EQ(k_tag_rep(0), rep.type);
        JARVIS_ASSERT_EQ(k_payload_bytes, rep.data_size);
        for (size_t idx = 0; idx < k_payload_bytes; ++idx) {
            JARVIS_ASSERT_EQ(static_cast<uint64_t>(req.data[idx]),
                             static_cast<uint64_t>(rep.data[idx]));
        }
        if (g_client_record && trip < k_samples) {
            g_trip_ticks[trip] = t1 - t0;
            g_trip_ns[trip] = ns_from_counter(c1 - c0);
        }
    }
    g_trips_done = g_client_trips;
    drain_inbox();
}

static bool run_chain(size_t depth, uint64_t trips, bool record,
                      uint64_t client_mask, uint64_t relay_mask) {
    if (depth == 0 || depth > k_max_relays || trips == 0) {
        return false;
    }
    g_client_trips = trips;
    g_client_record = record;
    g_trips_done = 0;
    g_dispatch_events = 0;
    g_chain_client = nullptr;
    g_chain_depth = 0;
    for (size_t idx = 0; idx < k_max_relays; ++idx) {
        g_relay_ids[idx] = 0;
        g_relay_ctx[idx] = RelayCtx{};
        g_chain_relays[idx] = nullptr;
    }
    for (size_t idx = 0; idx < depth; ++idx) {
        g_chain_relays[idx] =
            TaskControlBlock::create(relay_entry, k_relay_prio0 - idx, 10);
        if (g_chain_relays[idx] == nullptr) {
            return false;
        }
        if (!Scheduler::set_sched_policy(*g_chain_relays[idx],
                                         SchedPolicy::FIXED)) {
            return false;
        }
        g_relay_ctx[idx].index = idx;
        g_relay_ctx[idx].depth = depth;
        g_relay_ctx[idx].trips = trips;
        g_chain_relays[idx]->user_data = &g_relay_ctx[idx];
        if (relay_mask != 0) {
            if (Scheduler::set_affinity_err(*g_chain_relays[idx], relay_mask) !=
                errors::SCHED_ERR_OK) {
                return false;
            }
        }
    }
    for (size_t idx = 0; idx + 1 < depth; ++idx) {
        g_relay_ctx[idx].next_id = g_chain_relays[idx + 1]->id;
        g_relay_ids[idx] = g_chain_relays[idx]->id;
    }
    g_relay_ids[depth - 1] = g_chain_relays[depth - 1]->id;
    g_chain_client = TaskControlBlock::create(client_entry, k_client_prio, 5);
    if (g_chain_client == nullptr) {
        return false;
    }
    if (!Scheduler::set_sched_policy(*g_chain_client, SchedPolicy::FIXED)) {
        return false;
    }
    if (client_mask != 0) {
        if (Scheduler::set_affinity_err(*g_chain_client, client_mask) !=
            errors::SCHED_ERR_OK) {
            return false;
        }
    }
    auto *original = Scheduler::current_task();
    {
        arch::IrqGuard guard;
        for (size_t idx = 0; idx < depth; ++idx) {
            Scheduler::add_task(*g_chain_relays[idx]);
        }
        Scheduler::add_task(*g_chain_client);
    }
    test::yield_as(*g_chain_relays[depth - 1]);
    Scheduler::reschedule();
    test::wait_for_termination_safe(g_chain_client);
    for (size_t idx = 0; idx < depth; ++idx) {
        test::wait_for_termination_safe(g_chain_relays[idx]);
    }
    Scheduler::set_current(*original);
    g_chain_depth = depth;
    return true;
}

static void teardown_chain() {
    for (size_t idx = 0; idx < g_chain_depth; ++idx) {
        test::terminate_if_live(g_chain_relays[idx]);
    }
    test::terminate_if_live(g_chain_client);
    Scheduler::drain_zombie_list();
    g_chain_depth = 0;
}

} // namespace

// Runmode: kernel
// Testidea: Frozen WL descriptor drift guard (WL-1..WL-4 shape constants).
// Input: None (compile-time descriptor only).
// Expect: Payload/depth/trip/priority constants match the WL table; the
//         workload line is emitted for the baseline record.
// Depends: docs/specs/ipc-baseline.md WL table
JARVIS_TEST(mk_baseline_workload_shape, "PRE: none | POST: none") {
    JARVIS_ASSERT(k_payload_bytes == CONFIG_IPC_FAST_PAYLOAD_BYTES);
    JARVIS_ASSERT(k_payload_bytes <= IPC_MAX_MSG_SIZE);
    JARVIS_ASSERT_EQ(1U, k_depths[0]);
    JARVIS_ASSERT_EQ(4U, k_depths[3]);
    JARVIS_ASSERT(k_depths[0] < k_depths[1]);
    JARVIS_ASSERT(k_depths[1] < k_depths[2]);
    JARVIS_ASSERT(k_depths[2] < k_depths[3]);
    JARVIS_ASSERT(2 * k_depths[3] <= k_max_hops);
    JARVIS_ASSERT(k_trips >= 100);
    JARVIS_ASSERT(k_relay_prio0 < k_client_prio);
    JARVIS_ASSERT(k_client_prio > 20);
    JARVIS_ASSERT(k_client_prio <= CONFIG_DM_PRIO_MAX);
    JARVIS_ASSERT(CONFIG_TICK_HZ > 0);
    JARVIS_ASSERT(k_tag_req(0) != k_tag_rep(0));
    JARVIS_ASSERT(k_tag_req(0) != k_tag_rep(3));
    JARVIS_ASSERT(k_tag_req(3) != k_tag_rep(0));
    Logger::info(
        "[MK-BASELINE] workload payload_bytes=%lu depths=1,2,3,4 trips=%lu "
        "client_prio=%lu relay_prio=29..26 tick_hz=%lu",
        static_cast<uint64_t>(k_payload_bytes), k_trips, k_client_prio,
        static_cast<uint64_t>(CONFIG_TICK_HZ));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Per-hop trip latency (ticks + raw-counter ns, p50/p99/pmax, slope).
// Input: Relay-chain depths 1..4, 100 trips per depth via send_sync.
// Expect: Trip accounting exact, monotone percentiles, p50 >= 1 tick,
//         wedge ceiling, deeper chains not faster; slope published.
// Depends: IPC::send_sync, arch::Timer::ticks/raw_counter (M0 observes only)
JARVIS_TEST(mk_baseline_hop_latency, "PRE: none | POST: none") {
    uint64_t medians[4] = {};
    for (size_t d = 0; d < 4; ++d) {
        size_t depth = k_depths[d];
        for (size_t idx = 0; idx < k_samples; ++idx) {
            g_trip_ticks[idx] = 0;
            g_trip_ns[idx] = 0;
        }
        bool ok = run_chain(depth, k_trips, true, 0, 0);
        JARVIS_ASSERT(ok);
        JARVIS_ASSERT_EQ(k_trips, g_trips_done);
        Stats tick_stats = summarize(g_trip_ticks, k_samples);
        Stats ns_stats = summarize(g_trip_ns, k_samples);
        medians[d] = tick_stats.p50;
        JARVIS_ASSERT(tick_stats.pmax >= tick_stats.p99);
        JARVIS_ASSERT(tick_stats.p99 >= tick_stats.p50);
        JARVIS_ASSERT(ns_stats.pmax >= ns_stats.p99);
        JARVIS_ASSERT(ns_stats.p99 >= ns_stats.p50);
        JARVIS_ASSERT(tick_stats.p50 >= 1);
        uint64_t hops = 2 * depth;
        JARVIS_ASSERT(tick_stats.p50 <= hops * 8 + 8);
        uint64_t per_hop_x100 = tick_stats.p50 * 100 / hops;
        Logger::info(
            "[MK-BASELINE] latency depth=%lu hops_per_trip=%lu trips=%lu "
            "ticks_p50=%lu ticks_p99=%lu ticks_pmax=%lu "
            "per_hop_ticks_x100=%lu ns_p50=%lu ns_p99=%lu ns_pmax=%lu",
            static_cast<uint64_t>(depth), hops, k_trips, tick_stats.p50,
            tick_stats.p99, tick_stats.pmax, per_hop_x100, ns_stats.p50,
            ns_stats.p99, ns_stats.pmax);
        teardown_chain();
    }
    JARVIS_ASSERT(medians[3] >= medians[0]);
    uint64_t slope_x100 = 0;
    if (medians[3] >= medians[0]) {
        slope_x100 = (medians[3] - medians[0]) * 100 / 6;
    }
    Logger::info(
        "[MK-BASELINE] slope_ticks_per_hop_x100=%lu median_d1=%lu "
        "median_d4=%lu",
        slope_x100, medians[0], medians[3]);
    // NOTE (M3 #556 QE): the slope_ticks_per_hop_x100 < 100 structural
    // gate rides with the parked REPLY_RECV relay (see relay_entry);
    // re-enable together with it once PushedReply works.
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Switch rate over a fixed 200-trip depth-2 window (diag or proxy).
// Input: Depth-2 chain, 200 trips; diag counter on DEBUG, entry proxy else.
// Expect: Hop count exact, switches >= hops, source labelled.
// Depends: sched_debug::g_diag_switch_count (DEBUG) with entry fallback
JARVIS_TEST(mk_baseline_switch_rate, "PRE: none | POST: none") {
    const uint64_t depth = 2;
    const uint64_t hops = 2 * k_trips_rate * depth;
#if defined(CONFIG_DEBUG)
    uint64_t sw0 = sched_debug::g_diag_switch_count;
#endif
    uint64_t c0 = arch::Timer::raw_counter();
    bool ok = run_chain(depth, k_trips_rate, false, 0, 0);
    uint64_t c1 = arch::Timer::raw_counter();
    JARVIS_ASSERT(ok);
    JARVIS_ASSERT_EQ(k_trips_rate, g_trips_done);
    JARVIS_ASSERT(hops == 2 * k_trips_rate * depth);
    JARVIS_ASSERT(hops > 0);
    uint64_t window_ns = (c1 >= c0) ? ns_from_counter(c1 - c0) : 0;
    JARVIS_ASSERT(window_ns > 0);
#if defined(CONFIG_DEBUG)
    uint64_t switches = sched_debug::g_diag_switch_count - sw0;
    const char *source = "diag";
#else
    uint64_t switches = g_dispatch_events;
    const char *source = "entry";
#endif
    JARVIS_ASSERT(switches >= hops);
#if defined(CONFIG_DEBUG)
    JARVIS_ASSERT(switches > 0);
#else
    JARVIS_ASSERT(switches == hops);
#endif
    uint64_t hops_per_s =
        (window_ns == 0) ? 0 : hops * 1000000000ULL / window_ns;
    uint64_t switches_per_s =
        (window_ns == 0) ? 0 : switches * 1000000000ULL / window_ns;
    Logger::info(
        "[MK-BASELINE] rate depth=%lu trips=%lu hops=%lu switches=%lu "
        "source=%s window_ns=%lu hops_per_s=%lu switches_per_s=%lu",
        depth, k_trips_rate, hops, switches, source, window_ns, hops_per_s,
        switches_per_s);
    teardown_chain();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Core-locality leg (same-core asserts, split-core when available).
// Input: Depth-1 chain, same-core masks; split masks only if SMP is up.
// Expect: Same-core affinities honoured; split emits record, else an
//         explicit unavailable note referencing Q11 (pass, never assert).
// Depends: Scheduler::set_affinity_err/get_affinity, up_cpu_count
JARVIS_TEST(mk_baseline_core_locality, "PRE: none | POST: none") {
    uint64_t c0 = arch::Timer::raw_counter();
    bool ok = run_chain(1, k_trips, true, 0x1, 0x1);
    uint64_t c1 = arch::Timer::raw_counter();
    JARVIS_ASSERT(ok);
    JARVIS_ASSERT_EQ(k_trips, g_trips_done);
    JARVIS_ASSERT_EQ(0x1ULL, Scheduler::get_affinity(*g_chain_client));
    JARVIS_ASSERT_EQ(0x1ULL, Scheduler::get_affinity(*g_chain_relays[0]));
    Stats tick_stats = summarize(g_trip_ticks, k_samples);
    uint64_t window_ns = (c1 >= c0) ? ns_from_counter(c1 - c0) : 0;
    uint64_t hops = 2 * k_trips;
    uint64_t hops_per_s =
        (window_ns == 0) ? 0 : hops * 1000000000ULL / window_ns;
    Logger::info(
        "[MK-BASELINE] core same_core hops_per_s=%lu p50_ticks=%lu "
        "up_cpus=%lu",
        hops_per_s, tick_stats.p50, Scheduler::up_cpu_count());
    teardown_chain();
    if (Scheduler::up_cpu_count() > 1) {
        bool split_ok = run_chain(1, k_trips, true, 0x1, 0x2);
        JARVIS_ASSERT(split_ok);
        JARVIS_ASSERT_EQ(k_trips, g_trips_done);
        JARVIS_ASSERT_EQ(0x1ULL, Scheduler::get_affinity(*g_chain_client));
        JARVIS_ASSERT_EQ(0x2ULL, Scheduler::get_affinity(*g_chain_relays[0]));
        Stats split_stats = summarize(g_trip_ticks, k_samples);
        Logger::info("[MK-BASELINE] core split p50_ticks=%lu up_cpus=%lu",
                     split_stats.p50, Scheduler::up_cpu_count());
        teardown_chain();
    } else {
        Logger::info(
            "[MK-BASELINE] core split=unavailable up_cpus=%lu note=Q11/#525",
            Scheduler::up_cpu_count());
    }
    JARVIS_TEST_PASS();
}

void register_bench_mk_baseline_tests() {
    JARVIS_REGISTER_TEST_FLAGS(mk_baseline_workload_shape, kernel::test::TF_BENCH);
    JARVIS_REGISTER_TEST_FLAGS(mk_baseline_hop_latency, kernel::test::TF_BENCH);
    JARVIS_REGISTER_TEST_FLAGS(mk_baseline_switch_rate, kernel::test::TF_BENCH);
    JARVIS_REGISTER_TEST_FLAGS(mk_baseline_core_locality, kernel::test::TF_BENCH);
}
