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

/// @file test_rtc.cpp
/// @brief RTC (Real-Time Clock) driver tests.

#if defined(CONFIG_ARCH_X86_64) || defined(CONFIG_ARCH_AARCH64) || \
    defined(CONFIG_ARCH_RISCV64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/rtc.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verify RTC read_seconds returns a Unix timestamp within a
// reasonable range.
// Input: read_seconds() reads hardware RTC registers.
// Expect: Returned value is between year 2020 (~1577836800) and year 2200
// (~7258118400) epoch timestamps.
// Depends: arch::RTC
JARVIS_TEST(rtc_read_seconds, "PRE: iocd | POST: none") {
    uint64_t secs = arch::RTC::read_seconds();
    JARVIS_ASSERT(secs > 1577836800ULL);
    JARVIS_ASSERT(secs < 7258118400ULL);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verify BCD-to-binary conversion for various BCD values including
// edge and max byte values.
// Input: bcd_to_bin on 0x00, 0x09, 0x10, 0x15, 0x23, 0x59.
// Expect: Correct binary values (0x00->0, 0x10->10, 0x59->0x3B, etc.).
// Depends: arch::RTC
// NOTE (issue #355 H2): bcd_to_bin exists only on the x86 RTC HAL —
// non-x86 arches have no BCD conversion API.
#if defined(CONFIG_ARCH_X86_64)
JARVIS_TEST(rtc_bcd_conversion, "PRE: iocd | POST: none") {
    JARVIS_ASSERT_EQ(0x00, arch::RTC::bcd_to_bin(0x00));
    JARVIS_ASSERT_EQ(0x09, arch::RTC::bcd_to_bin(0x09));
    JARVIS_ASSERT_EQ(0x0A, arch::RTC::bcd_to_bin(0x10));
    JARVIS_ASSERT_EQ(0x0F, arch::RTC::bcd_to_bin(0x15));
    JARVIS_ASSERT_EQ(0x17, arch::RTC::bcd_to_bin(0x23));
    JARVIS_ASSERT_EQ(0x3B, arch::RTC::bcd_to_bin(0x59));
    JARVIS_TEST_PASS();
}
#endif // CONFIG_ARCH_X86_64 (bcd_to_bin is x86-HAL-only)

// Runmode: kernel
// Testidea: Register all RTC tests with the test framework.
// Depends: arch::RTC
void register_rtc_tests() {
    Logger::info("Registering RTC tests");
    JARVIS_REGISTER_TEST(rtc_read_seconds);
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_REGISTER_TEST(rtc_bcd_conversion);
#endif
}
#endif
