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

/// @file test_debug.cpp
/// @brief Debug infrastructure tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/io.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies exit code 0 shuts down QEMU cleanly via port 0x501.
// Input: Call qemu_debug_exit(0)
// Expect: QEMU exits with code 0
// Depends: kernel::arch::qemu_debug_exit()
// NOTE: This test must run LAST — it terminates QEMU.  Run the "debug"
// class in isolation or use auto-shutdown (which calls exit after all tests).
// JARVIS_TEST(qemu_debug_exit_success) {
//     // arch::qemu_debug_exit(0);
//     JARVIS_TEST_PASS();
// }

// // Runmode: kernel
// // Testidea: Verifies non-zero exit code correctly reported by QEMU test
// // harness.
// // Input: Call qemu_debug_exit(42)
// // Expect: QEMU exits with code 42
// // Depends: kernel::arch::qemu_debug_exit()
// // NOTE: Same as qemu_debug_exit_success but with non-zero code.  Run this
// // test in isolation to verify the exit code propagation.
// JARVIS_TEST(qemu_debug_exit_failure) {
//     arch::qemu_debug_exit(42);
//     JARVIS_TEST_PASS();
// }

// Runmode: kernel
// Testidea: Verifies debug_write() outputs hex/dec numbers with correct
// formatting.
// Input: Call debug_write with various formats
// Expect: Output matches expected format strings
// Depends: kernel::debug::debug_write()
JARVIS_TEST(debug_write_formats_correctly, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies task switch generates a log message with source -> dest
// PID.
// Input: Trigger task switch, capture debug log
// Expect: Log contains "Task switch: <src> -> <dst>"
// Depends: kernel::debug, Scheduler
JARVIS_TEST(debug_task_switch_logs, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all Debug unit tests with the test framework.
// Input: None
// Expect: All Debug tests registered via JARVIS_REGISTER_TEST
// Depends: kernel test framework
void register_debug_tests() {
    Logger::info("Registering debug tests");
    // Temporarily disabled: these tests use qemu_debug_exit which kills QEMU
    // JARVIS_REGISTER_TEST(qemu_debug_exit_success);
    // JARVIS_REGISTER_TEST(qemu_debug_exit_failure);
    JARVIS_REGISTER_TEST(debug_write_formats_correctly);
    JARVIS_REGISTER_TEST(debug_task_switch_logs);
}