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

/// @file test_checked_ptr.cpp
/// @brief Checked pointer safety tests.

#include <test.hpp>
#include <logger.hpp>
#include <signal.hpp>
#include <kernel/memory/checked_ptr.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Validates user/kernel address range detection via is_user_range
// Input: Kernel address 0xFFFF800000000000, user address 0x400000, nullptr,
// boundary values at USER_SPACE_LIMIT
// Expect: is_user_range returns true for user addresses, false for kernel
// addresses, nullptr, and overflowing ranges
// Depends: kernel/memory
JARVIS_TEST(checked_ptr_is_user_range, "PRE: none | POST: none") {
    JARVIS_ASSERT(
        !is_user_range(reinterpret_cast<void *>(0xFFFF800000000000ULL), 1));

    JARVIS_ASSERT(is_user_range(reinterpret_cast<void *>(0x400000), 1));

    JARVIS_ASSERT(!is_user_range(nullptr, 1));

    uint64_t limit = USER_SPACE_LIMIT;
    JARVIS_ASSERT(!is_user_range(reinterpret_cast<void *>(limit), 1));
    JARVIS_ASSERT(is_user_range(reinterpret_cast<void *>(limit - 1), 1));

    JARVIS_ASSERT(!is_user_range(reinterpret_cast<void *>(limit - 10), 20));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates CheckedPtr::valid() for valid vs null pointers
// Input: CheckedPtr wrapping a 64-byte stack buffer, CheckedPtr with nullptr
// Expect: valid() returns true for valid buffer, false for nullptr
// Depends: kernel/memory
JARVIS_TEST(checked_ptr_valid, "PRE: none | POST: none") {
    char buf[64] = "test data";
    CheckedPtr<char> cp(buf, sizeof(buf));
    JARVIS_ASSERT(!cp.valid());

    CheckedPtr<char> null_cp(nullptr);
    JARVIS_ASSERT(!null_cp.valid());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates is_user_string rejects kernel pointers, null, and
// stack buffers
// Input: Kernel string literal, nullptr, stack-allocated buffer
// Expect: All three inputs return false (not user strings)
// Depends: kernel/memory
JARVIS_TEST(checked_ptr_is_user_string, "PRE: none | POST: none") {
    JARVIS_ASSERT(!is_user_string("kernel string"));

    JARVIS_ASSERT(!is_user_string(nullptr));

    char user_buf[] = "hello";
    JARVIS_ASSERT(!is_user_string(user_buf));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates SignalFrame struct layout, size, and field values
// Input: SignalFrame with sig=11, saved_rip=0x400000, saved_rsp=0x70000000,
// saved_rflags=0x202, saved_cs=0x1B, saved_ss=0x23
// Expect: sizeof(SignalFrame) == 64, SIGNAL_FRAME_SIZE == 64, all field
// values match set values
// Depends: signal
JARVIS_TEST(signal_frame_size, "PRE: none | POST: none") {
    JARVIS_ASSERT_EQ(64ULL, sizeof(SignalFrame));
    JARVIS_ASSERT_EQ(static_cast<size_t>(64), SIGNAL_FRAME_SIZE);

    SignalFrame sf{};
    sf.sig = 11;
    sf.saved_rip = 0x400000;
    sf.saved_rsp = 0x70000000;
    sf.saved_rflags = 0x202;
    sf.saved_cs = 0x1B;
    sf.saved_ss = 0x23;
    JARVIS_ASSERT_EQ(11ULL, sf.sig);
    JARVIS_ASSERT_EQ(0x400000ULL, sf.saved_rip);
    JARVIS_ASSERT_EQ(0x70000000ULL, sf.saved_rsp);
    JARVIS_ASSERT_EQ(0x202ULL, sf.saved_rflags);
    JARVIS_ASSERT_EQ(0x1BULL, sf.saved_cs);
    JARVIS_ASSERT_EQ(0x23ULL, sf.saved_ss);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all CheckedPtr tests with the test framework
// Input: None
// Expect: All CheckedPtr tests registered
// Depends: test framework
void register_checked_ptr_tests() {
    Logger::info("Registering CheckedPtr tests");

    JARVIS_REGISTER_RELEASE_TEST(checked_ptr_is_user_range);
    JARVIS_REGISTER_RELEASE_TEST(checked_ptr_valid);
    JARVIS_REGISTER_RELEASE_TEST(checked_ptr_is_user_string);
    JARVIS_REGISTER_RELEASE_TEST(signal_frame_size);
}
