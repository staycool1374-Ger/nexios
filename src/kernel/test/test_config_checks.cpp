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

/// @file test_config_checks.cpp
/// @brief Compile-time configuration sanity checks (v0.3.7, check-config
///        extensions).  C-class query tests — no task dispatch, no dynamic
///        allocation, no side effects.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/nexios_config.h>
#include <kernel/task/sporadic_server.hpp>

using namespace kernel;
using task::SporadicServer;

// Runmode: kernel
// Testidea: The scheduler priority ceiling must cover the full configured
//           priority range (O(1) bitmap scheduler supports up to 128 levels)
//           and the task table must be non-empty.
// Input: Compile-time CONFIG_PRIORITY_CEILING / CONFIG_MAX_TASKS values.
// Expect: CONFIG_PRIORITY_CEILING >= 127 and CONFIG_MAX_TASKS > 0.
// Depends: kernel/nexios_config.h
JARVIS_TEST(config_ceiling_ge_max_prio, "PRE: none | POST: none") {
#if defined(CONFIG_HARD_REAL_TIME)
    static_assert(CONFIG_HARD_REAL_TIME >= 0, "profile macro");
#endif
    JARVIS_ASSERT_FMT(CONFIG_PRIORITY_CEILING >= 127,
                      "CONFIG_PRIORITY_CEILING=%d must be >= 127",
                      (int)CONFIG_PRIORITY_CEILING);
    JARVIS_ASSERT_FMT(CONFIG_MAX_TASKS > 0,
                      "CONFIG_MAX_TASKS=%d must be > 0",
                      (int)CONFIG_MAX_TASKS);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The scheduling tick rate must be positive; a zero tick rate
//           would stall the scheduler entirely.
// Input: Compile-time CONFIG_TICK_HZ value.
// Expect: CONFIG_TICK_HZ >= 1.
// Depends: kernel/nexios_config.h
// Note: CONFIG_TIMER_CLOCK_HZ is intentionally NOT referenced — it does not
//       exist in the unified nexios_config.h (timer clock lives in arch HAL).
JARVIS_TEST(config_tick_hz_sane, "PRE: none | POST: none") {
    JARVIS_ASSERT_FMT(CONFIG_TICK_HZ >= 1,
                      "CONFIG_TICK_HZ=%d must be >= 1", (int)CONFIG_TICK_HZ);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The per-task kernel stack must satisfy the architecture minimum.
// Input: Compile-time CONFIG_STACK_SIZE / CONFIG_MIN_STACK_SIZE values.
// Expect: CONFIG_STACK_SIZE >= CONFIG_MIN_STACK_SIZE (and >= 4096 absolute).
// Depends: kernel/nexios_config.h
JARVIS_TEST(config_stack_size_bounds, "PRE: none | POST: none") {
    JARVIS_ASSERT_FMT(CONFIG_STACK_SIZE >= CONFIG_MIN_STACK_SIZE,
                      "CONFIG_STACK_SIZE=%d < CONFIG_MIN_STACK_SIZE=%d",
                      (int)CONFIG_STACK_SIZE, (int)CONFIG_MIN_STACK_SIZE);
    JARVIS_ASSERT_FMT(CONFIG_STACK_SIZE >= 4096,
                      "CONFIG_STACK_SIZE=%d must be >= 4096",
                      (int)CONFIG_STACK_SIZE);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Hard-RT production builds must enable preemption, priority
//           inheritance, and the local APIC timer — the three dependent
//           configurations that guarantee bounded blocking and deterministic
//           tick generation.
// Input: Compile-time CONFIG_PREEMPTION / CONFIG_MUTEX_PIP /
//        CONFIG_USE_APIC_TIMER values.
// Expect: All three are 1.
// Depends: kernel/nexios_config.h
JARVIS_TEST(config_hard_rt_dependents, "PRE: none | POST: none") {
#if defined(CONFIG_HARD_REAL_TIME)
    // Profile-gated: when the hard-RT profile exists, it must force the
    // dependents below.  Currently the profile macro is NOT defined, so
    // this block compiles out and the raw dependents are checked.
    JARVIS_ASSERT(CONFIG_HARD_REAL_TIME == 1);
#endif
    JARVIS_ASSERT_FMT(CONFIG_PREEMPTION == 1,
                      "CONFIG_PREEMPTION=%d must be 1 for hard-RT",
                      (int)CONFIG_PREEMPTION);
    JARVIS_ASSERT_FMT(CONFIG_MUTEX_PIP == 1,
                      "CONFIG_MUTEX_PIP=%d must be 1 for hard-RT",
                      (int)CONFIG_MUTEX_PIP);
    JARVIS_ASSERT_FMT(CONFIG_USE_APIC_TIMER == 1,
                      "CONFIG_USE_APIC_TIMER=%d must be 1 for hard-RT",
                      (int)CONFIG_USE_APIC_TIMER);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SporadicServer budget C must never exceed replenishment period T
//           for any configured server (Liu & Layland feasibility bound).
// Input: A real SporadicServer initialised with C=10, T=100.
// Expect: max_budget() (C) <= period() (T) and remaining budget == C.
// Depends: kernel/task/sporadic_server.hpp
JARVIS_TEST(config_sporadic_budget_le_period, "PRE: none | POST: none") {
    SporadicServer ss;
    ss.init(10, 100, 2);
    JARVIS_ASSERT_FMT(ss.max_budget() <= ss.period(),
                      "SporadicServer C=%lu > T=%lu", ss.max_budget(),
                      ss.period());
    JARVIS_ASSERT_EQ(10ULL, ss.remaining_budget());
    JARVIS_ASSERT_EQ(100ULL, ss.period());
    JARVIS_TEST_PASS();
}

void register_config_checks_tests() {
    Logger::info("Registering config-check tests");
    JARVIS_REGISTER_TEST(config_ceiling_ge_max_prio);
    JARVIS_REGISTER_TEST(config_tick_hz_sane);
    JARVIS_REGISTER_TEST(config_stack_size_bounds);
    JARVIS_REGISTER_TEST(config_hard_rt_dependents);
    JARVIS_REGISTER_TEST(config_sporadic_budget_le_period);
}
