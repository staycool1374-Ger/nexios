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

/// @file test_timer_wheel.cpp
/// @brief Event-timer wheel tests (issue #17, v0.4.7).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/nexios_config.h>
#include <kernel/arch/timer.hpp>
#include <kernel/time/timer_wheel.hpp>

using namespace kernel;

namespace {

// Expiry base derived from the LIVE clock: armed timers must sit above the
// real monotonic time, otherwise the production on_tick hook (real ticks
// with real now_ns) expires them early and the test becomes ISR timing
// luck. +10 s is unreachable inside a millisecond-scale test.
uint64_t future_base() {
    return arch::Timer::ns_monotonic() + 10000000000ULL;
}

/// @brief Counting callback: increments the pointed-to counter.
void flag_fire(void* context) {
    ++(*static_cast<uint64_t*>(context));
}

/// @brief Fire-order log for earliest-first verification.
uint64_t g_fire_order[16] = {};
uint64_t g_fire_pos = 0;

/// @brief Order-recording callback: appends the pointed-to id.
void order_fire(void* context) {
    const uint64_t id = *static_cast<const uint64_t*>(context);
    if (g_fire_pos < 16) {
        g_fire_order[g_fire_pos] = id;
        ++g_fire_pos;
    }
}

} // namespace

// Runmode: kernel
// Testidea: arm-then-expire fires the callback exactly once at expiry.
// Input: arm(cb, expiry=live_base+100), tick live_base+99/+100/+200
// Expect: silent before expiry, exactly one fire at expiry, silent after
// Depends: time::TimerWheel::arm/cancel/on_tick
JARVIS_TEST(wheel_arm_expire_fires_once, "PRE: none | POST: none") {
    time::TimerWheel::snapshot_reset();
    const uint64_t base_ns = future_base();
    uint64_t fired = 0;
    time::TimerWheel::Handle handle{};
    JARVIS_ASSERT(time::TimerWheel::arm(0, base_ns + 100, flag_fire,
                                        &fired, &handle));
    JARVIS_ASSERT_EQ((uint64_t)1, time::TimerWheel::live_count(0));
    JARVIS_ASSERT_EQ((uint32_t)0,
                     time::TimerWheel::on_tick(base_ns + 99, 0));
    JARVIS_ASSERT_EQ((uint64_t)0, fired);
    JARVIS_ASSERT_EQ((uint32_t)1,
                     time::TimerWheel::on_tick(base_ns + 100, 0));
    JARVIS_ASSERT_EQ((uint64_t)1, fired);
    JARVIS_ASSERT_EQ((uint32_t)0,
                     time::TimerWheel::on_tick(base_ns + 200, 0));
    JARVIS_ASSERT_EQ((uint64_t)1, fired);
    JARVIS_ASSERT_EQ((uint64_t)0, time::TimerWheel::live_count(0));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: cancel-before-expiry never fires and invalidates the handle.
// Input: arm, cancel, on_tick past expiry; cancel again
// Expect: fired == 0, first cancel true, second cancel false
// Depends: time::TimerWheel::cancel
JARVIS_TEST(wheel_cancel_before_expiry, "PRE: none | POST: none") {
    time::TimerWheel::snapshot_reset();
    const uint64_t base_ns = future_base();
    uint64_t fired = 0;
    time::TimerWheel::Handle handle{};
    JARVIS_ASSERT(time::TimerWheel::arm(0, base_ns + 50, flag_fire,
                                        &fired, &handle));
    JARVIS_ASSERT(time::TimerWheel::cancel(handle));
    JARVIS_ASSERT_EQ((uint32_t)0,
                     time::TimerWheel::on_tick(base_ns + 1000, 0));
    JARVIS_ASSERT_EQ((uint64_t)0, fired);
    JARVIS_ASSERT(!time::TimerWheel::cancel(handle));
    JARVIS_ASSERT_EQ((uint64_t)0, time::TimerWheel::live_count(0));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: recycled slots reject stale-generation handles.
// Input: arm A, cancel A, arm B (same slot), cancel with A's handle
// Expect: stale cancel false, live handle cancels true
// Depends: time::TimerWheel generation tags
JARVIS_TEST(wheel_stale_generation_rejected, "PRE: none | POST: none") {
    time::TimerWheel::snapshot_reset();
    const uint64_t base_ns = future_base();
    uint64_t fired = 0;
    time::TimerWheel::Handle handle_a{};
    time::TimerWheel::Handle handle_b{};
    JARVIS_ASSERT(time::TimerWheel::arm(0, base_ns + 50, flag_fire,
                                        &fired, &handle_a));
    JARVIS_ASSERT(time::TimerWheel::cancel(handle_a));
    // Free-list is LIFO: the next arm deterministically reuses A's slot.
    JARVIS_ASSERT(time::TimerWheel::arm(0, base_ns + 60, flag_fire,
                                        &fired, &handle_b));
    JARVIS_ASSERT_EQ(handle_a.slot, handle_b.slot);
    JARVIS_ASSERT(handle_a.generation != handle_b.generation);
    JARVIS_ASSERT(!time::TimerWheel::cancel(handle_a));
    JARVIS_ASSERT(time::TimerWheel::cancel(handle_b));
    JARVIS_ASSERT_EQ((uint64_t)0, time::TimerWheel::live_count(0));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: full wheel rejects arm; freed slots recycle with new generation.
// Input: arm kMaxTimersPerCpu timers, arm once more, cancel one, arm again
// Expect: overflow arm false; post-cancel arm true with bumped generation
// Depends: time::TimerWheel::arm capacity bound
JARVIS_TEST(wheel_exhaustion_and_recycle, "PRE: none | POST: none") {
    time::TimerWheel::snapshot_reset();
    const uint64_t base_ns = future_base();
    uint64_t fired = 0;
    time::TimerWheel::Handle handles[time::TimerWheel::kMaxTimersPerCpu];
    for (uint32_t arm_idx = 0; arm_idx < time::TimerWheel::kMaxTimersPerCpu;
         ++arm_idx) {
        JARVIS_ASSERT(time::TimerWheel::arm(0, base_ns + 1000 + arm_idx,
                                            flag_fire, &fired,
                                            &handles[arm_idx]));
    }
    JARVIS_ASSERT_EQ((uint64_t)time::TimerWheel::kMaxTimersPerCpu,
                     time::TimerWheel::live_count(0));
    time::TimerWheel::Handle overflow{};
    JARVIS_ASSERT(!time::TimerWheel::arm(0, base_ns + 2000, flag_fire,
                                         &fired, &overflow));
    JARVIS_ASSERT(time::TimerWheel::cancel(handles[0]));
    time::TimerWheel::Handle recycled{};
    JARVIS_ASSERT(time::TimerWheel::arm(0, base_ns + 3000, flag_fire,
                                        &fired, &recycled));
    JARVIS_ASSERT_EQ(handles[0].slot, recycled.slot);
    JARVIS_ASSERT(handles[0].generation != recycled.generation);
    time::TimerWheel::snapshot_reset();
    JARVIS_ASSERT_EQ((uint64_t)0, time::TimerWheel::live_count(0));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: pop cap defers overflow expiries to the next tick without loss.
// Input: arm 10 timers at staggered expiries, on_tick twice past all
// Expect: first tick fires the 8 earliest in order, second fires the rest
// Depends: time::TimerWheel::on_tick pop cap
JARVIS_TEST(wheel_pop_cap_defers_without_loss, "PRE: none | POST: none") {
    time::TimerWheel::snapshot_reset();
    const uint64_t base_ns = future_base();
    g_fire_pos = 0;
    uint64_t ids[10] = {};
    for (uint64_t id_idx = 0; id_idx < 10; ++id_idx) {
        ids[id_idx] = id_idx;
        time::TimerWheel::Handle handle{};
        JARVIS_ASSERT(time::TimerWheel::arm(0, base_ns + 10 + id_idx,
                                            order_fire, &ids[id_idx],
                                            &handle));
    }
    JARVIS_ASSERT_EQ((uint32_t)8,
                     time::TimerWheel::on_tick(base_ns + 19, 0));
    JARVIS_ASSERT_EQ((uint64_t)8, g_fire_pos);
    for (uint64_t order_idx = 0; order_idx < 8; ++order_idx) {
        JARVIS_ASSERT_EQ(order_idx, g_fire_order[order_idx]);
    }
    JARVIS_ASSERT_EQ((uint32_t)2,
                     time::TimerWheel::on_tick(base_ns + 19, 0));
    JARVIS_ASSERT_EQ((uint64_t)10, g_fire_pos);
    JARVIS_ASSERT_EQ((uint64_t)8, g_fire_order[8]);
    JARVIS_ASSERT_EQ((uint64_t)9, g_fire_order[9]);
    JARVIS_ASSERT_EQ((uint64_t)0, time::TimerWheel::live_count(0));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: non-BSP arm is fail-closed (production never ticks AP wheels).
// Input: arm on cpu 1, on_tick cpu 1 past expiry
// Expect: arm false, live_count 0, tick silent, fired == 0
// Depends: time::TimerWheel per-CPU routing
JARVIS_TEST(wheel_cpu_isolation, "PRE: none | POST: none") {
    time::TimerWheel::snapshot_reset();
    const uint64_t base_ns = future_base();
#if CONFIG_MAX_CPUS < 2
    time::TimerWheel::Handle bad_handle{};
    uint64_t fired = 0;
    JARVIS_ASSERT(!time::TimerWheel::arm(CONFIG_MAX_CPUS, base_ns + 50,
                                         flag_fire, &fired, &bad_handle));
    JARVIS_ASSERT_EQ((uint32_t)0,
                     time::TimerWheel::on_tick(base_ns + 1000,
                                               CONFIG_MAX_CPUS));
    JARVIS_ASSERT_EQ((uint64_t)0,
                     time::TimerWheel::live_count(CONFIG_MAX_CPUS));
#else
    // Production services only the BSP wheel (Scheduler::on_tick AP
    // early-return; ap_tick has no wheel hook), so arm() must fail
    // closed for non-BSP CPUs instead of reporting success without expiry.
    uint64_t fired = 0;
    time::TimerWheel::Handle handle{};
    JARVIS_ASSERT(!time::TimerWheel::arm(1, base_ns + 50, flag_fire,
                                         &fired, &handle));
    JARVIS_ASSERT_EQ((uint64_t)0, time::TimerWheel::live_count(1));
    JARVIS_ASSERT_EQ((uint32_t)0,
                     time::TimerWheel::on_tick(base_ns + 1000, 1));
    JARVIS_ASSERT_EQ((uint64_t)0, fired);
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: snapshot_reset disarms everything and invalidates handles.
// Input: arm 3 timers, snapshot_reset, on_tick far future
// Expect: fired == 0, live_count == 0, old cancels false
// Depends: time::TimerWheel::snapshot_reset
JARVIS_TEST(wheel_snapshot_reset_clears, "PRE: none | POST: none") {
    time::TimerWheel::snapshot_reset();
    const uint64_t base_ns = future_base();
    uint64_t fired = 0;
    time::TimerWheel::Handle handles[3];
    for (uint32_t arm_idx = 0; arm_idx < 3; ++arm_idx) {
        JARVIS_ASSERT(time::TimerWheel::arm(0, base_ns + 100 + arm_idx,
                                            flag_fire, &fired,
                                            &handles[arm_idx]));
    }
    time::TimerWheel::snapshot_reset();
    JARVIS_ASSERT_EQ((uint64_t)0, time::TimerWheel::live_count(0));
    JARVIS_ASSERT_EQ((uint32_t)0,
                     time::TimerWheel::on_tick(base_ns + 100000, 0));
    JARVIS_ASSERT_EQ((uint64_t)0, fired);
    for (uint32_t cancel_idx = 0; cancel_idx < 3; ++cancel_idx) {
        JARVIS_ASSERT(!time::TimerWheel::cancel(handles[cancel_idx]));
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: arm/cancel/expire burst leaves zero live entries (leak check).
// Input: arm N, cancel half, expire rest via on_tick
// Expect: live_count == 0 at end (ResourceTracker zero-delta automatic)
// Depends: time::TimerWheel::live_count
JARVIS_TEST(wheel_burst_leaves_zero_live, "PRE: none | POST: none") {
    time::TimerWheel::snapshot_reset();
    const uint64_t base_ns = future_base();
    uint64_t fired = 0;
    constexpr uint32_t k_burst = 16;
    time::TimerWheel::Handle handles[k_burst];
    for (uint32_t arm_idx = 0; arm_idx < k_burst; ++arm_idx) {
        JARVIS_ASSERT(time::TimerWheel::arm(0, base_ns + 10 + arm_idx,
                                            flag_fire, &fired,
                                            &handles[arm_idx]));
    }
    for (uint32_t cancel_idx = 0; cancel_idx < k_burst; cancel_idx += 2) {
        JARVIS_ASSERT(time::TimerWheel::cancel(handles[cancel_idx]));
    }
    for (int round = 0;
         round < 4 && time::TimerWheel::live_count(0) > 0; ++round) {
        time::TimerWheel::on_tick(base_ns + 100, 0);
    }
    JARVIS_ASSERT_EQ((uint64_t)0, time::TimerWheel::live_count(0));
    JARVIS_ASSERT_EQ((uint64_t)(k_burst / 2), fired);
    JARVIS_TEST_PASS();
}

void register_timer_wheel_tests() {
    Logger::info("Registering timer_wheel tests");
    JARVIS_REGISTER_TEST(wheel_arm_expire_fires_once);
    JARVIS_REGISTER_TEST(wheel_cancel_before_expiry);
    JARVIS_REGISTER_TEST(wheel_stale_generation_rejected);
    JARVIS_REGISTER_TEST(wheel_exhaustion_and_recycle);
    JARVIS_REGISTER_TEST(wheel_pop_cap_defers_without_loss);
    JARVIS_REGISTER_TEST(wheel_cpu_isolation);
    JARVIS_REGISTER_TEST(wheel_snapshot_reset_clears);
    JARVIS_REGISTER_TEST(wheel_burst_leaves_zero_live);
}
