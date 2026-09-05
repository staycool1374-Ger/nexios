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

/// @file test_timer.cpp
/// @brief Timer subsystem tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/irq_guard.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies PIT timer ticks() returns a reasonable value after boot
// init. The PIT was initialized at boot, so ticks() should be non-zero by
// the time any test runs.
// Input: Read Timer::ticks()
// Expect: ticks() > 0
// Depends: kernel::arch::Timer
JARVIS_TEST(pit_init_sets_divisor, "PRE: iocd | POST: none") {
    uint64_t t = arch::Timer::ticks();
    JARVIS_ASSERT_FMT(t > 0,
                      "Timer ticks should be > 0 after boot init, got %lu", t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies successive ticks() calls return non-decreasing values.
// Input: Call ticks() multiple times
// Expect: Each call returns value >= previous call
// Depends: kernel::arch::Timer::ticks()
JARVIS_TEST(pit_ticks_monotonic, "PRE: iocd | POST: none") {
    uint64_t t1 = arch::Timer::ticks();
    uint64_t t2 = arch::Timer::ticks();
    uint64_t t3 = arch::Timer::ticks();
    JARVIS_ASSERT(t2 >= t1);
    JARVIS_ASSERT(t3 >= t2);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies set_ticks_for_test() overrides the tick counter.
// Input: Set test value via set_ticks_for_test(), then call ticks()
// Expect: Returns the test value
// Depends: kernel::arch::Timer::set_ticks_for_test()
JARVIS_TEST(pit_set_ticks_for_test, "PRE: iocd | POST: none") {
    arch::Timer::set_ticks_for_test(42);
    JARVIS_ASSERT_EQ((uint64_t)42, arch::Timer::ticks());
    arch::Timer::set_ticks_for_test(0xDEAD);
    JARVIS_ASSERT_EQ((uint64_t)0xDEAD, arch::Timer::ticks());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies each IRQ0 delivery increments the tick count.
// Input: Trigger IRQ0 handler, check tick count before/after
// Expect: Tick count increases by 1 per IRQ0
// Depends: kernel::arch::Timer::handle_irq()
JARVIS_TEST(pit_irq0_handler_increments_ticks, "PRE: iocd | POST: none") {
    arch::IrqGuard guard;
    arch::Timer::set_ticks_for_test(0);
    JARVIS_ASSERT_EQ((uint64_t)0, arch::Timer::ticks());
    arch::Timer::handle_irq(0);
    JARVIS_ASSERT_EQ((uint64_t)1, arch::Timer::ticks());
    arch::Timer::handle_irq(0);
    JARVIS_ASSERT_EQ((uint64_t)2, arch::Timer::ticks());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies set_frequency accepts different frequency values without
// crashing. The PIT divisor is computed from the requested frequency.
// Input: Call set_frequency with various values
// Expect: No crash, ticks() still accessible
// Depends: kernel::arch::Timer::set_frequency()
JARVIS_TEST(pit_set_frequency_accepts_range, "PRE: iocd | POST: none") {
    arch::Timer::set_frequency(100);
    JARVIS_ASSERT(arch::Timer::ticks() > 0);
    arch::Timer::set_frequency(500);
    JARVIS_ASSERT(arch::Timer::ticks() > 0);
    arch::Timer::set_frequency(1000);
    JARVIS_ASSERT(arch::Timer::ticks() > 0);
    arch::Timer::set_frequency(10000);
    JARVIS_ASSERT(arch::Timer::ticks() > 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all Timer/PIT unit tests with the test framework.
// Input: None
// Expect: All PIT tests registered via JARVIS_REGISTER_TEST
// Depends: kernel test framework
void register_timer_tests() {
    Logger::info("Registering timer tests");
    JARVIS_REGISTER_TEST(pit_init_sets_divisor);
    JARVIS_REGISTER_TEST(pit_ticks_monotonic);
    JARVIS_REGISTER_TEST(pit_set_ticks_for_test);
    JARVIS_REGISTER_TEST(pit_irq0_handler_increments_ticks);
    JARVIS_REGISTER_TEST(pit_set_frequency_accepts_range);
}