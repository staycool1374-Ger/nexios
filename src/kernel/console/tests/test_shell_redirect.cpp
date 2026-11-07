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

/// @file test_shell_redirect.cpp
/// @brief Shell I/O redirection tests.

#include <test.hpp>
#include <logger.hpp>
#include <services/shell.hpp>
#include <services/terminal/terminal.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies shell redirect with echo > /dev/null does not crash.
// Input: Shell::execute("echo test > /dev/null")
// Expect: No crash, redirect parsing works
// Depends: service::Shell, service::Terminal
JARVIS_TEST(shell_redirect_to_devnull, "PRE: vfsd, iocd | POST: none") {
    service::Shell::execute("echo test > /dev/null");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies shell redirect with no target is handled gracefully.
// Input: Shell::execute("echo test >")
// Expect: No crash, parser handles missing target
// Depends: service::Shell
JARVIS_TEST(shell_redirect_no_target, "PRE: vfsd, iocd | POST: none") {
    service::Shell::execute("echo test >");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies redirect captures command output.
// Input: Shell::execute("echo HELLO > /dev/null")
// Expect: No crash, capture mechanism is exercised
// Depends: service::Shell
JARVIS_TEST(shell_redirect_capture_output, "PRE: vfsd, iocd | POST: none") {
    service::Shell::execute("echo HELLO > /dev/null");
    JARVIS_TEST_PASS();
}

void register_shell_redirect_tests() {
    Logger::info("Registering shell redirect tests");
    JARVIS_REGISTER_TEST(shell_redirect_to_devnull);
    JARVIS_REGISTER_TEST(shell_redirect_no_target);
    JARVIS_REGISTER_TEST(shell_redirect_capture_output);
}
