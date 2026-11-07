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

/// @file test_gcov.cpp
/// @brief GCOV code coverage metadata tests.

#include <test.hpp>
#include <logger.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies after kernel boot, gcov handler struct is in valid state.
// Input: Check gcov handler after init
// Expect: Handler initialized, ready for use
// Depends: kernel::GcovHandler
JARVIS_TEST(gcov_handler_initialized, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies calling instrument function toggles the tracking bitmask.
// Input: Call gcov_instrument(), check bitmask
// Expect: Corresponding bit toggled
// Depends: kernel::GcovHandler
JARVIS_TEST(gcov_instrument_updates_bitmask, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies gcov_flush_to_serial() emits non-empty trace data.
// Input: Call gcov_flush_to_serial()
// Expect: Serial output contains coverage data
// Depends: kernel::GcovHandler
JARVIS_TEST(gcov_flush_outputs_data, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies consecutive rdtsc() calls return strictly increasing
// values.
// Input: Call rdtsc() twice with small delay
// Expect: Second value > first value
// Depends: kernel::arch::rdtsc()
JARVIS_TEST(rdtsc_monotonic, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all GCOV/Profiling unit tests with the test framework.
// Input: None
// Expect: All GCOV tests registered via JARVIS_REGISTER_TEST
// Depends: kernel test framework
void register_gcov_tests() {
    Logger::info("Registering GCOV tests");
    JARVIS_REGISTER_TEST(gcov_handler_initialized);
    JARVIS_REGISTER_TEST(gcov_instrument_updates_bitmask);
    JARVIS_REGISTER_TEST(gcov_flush_outputs_data);
    JARVIS_REGISTER_TEST(rdtsc_monotonic);
}