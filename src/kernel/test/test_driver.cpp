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

/// @file test_driver.cpp
/// @brief Driver framework tests.

#include <test.hpp>
#include <logger.hpp>
#include <string.hpp>
#include <kernel/driver/driver.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/serial.hpp>
#include <kernel/arch/keyboard.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Tests that an existing driver ("keyboard") can be found by name
// and that a nonexistent driver returns nullptr
// Input: "keyboard" (existing driver name), "nonexistent_driver_xyz"
// (non-existing driver name)
// Expect: DriverRegistry::find("keyboard") returns a non-null pointer with
// name matching "keyboard"; find("nonexistent_driver_xyz") returns nullptr
// Depends: DriverRegistry
JARVIS_TEST(driver_registry_find, "PRE: iocd | POST: none") {
    auto *drv = DriverRegistry::find("keyboard");
    JARVIS_ASSERT(drv != nullptr);
    JARVIS_ASSERT_EQ(0, strcmp(drv->name, "keyboard"));
    drv = DriverRegistry::find("nonexistent_driver_xyz");
    JARVIS_ASSERT(drv == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the driver registry contains at least 4 drivers and
// that both "keyboard" and "timer" are present
// Input: DriverRegistry::count() and DriverRegistry::get(i) for each index
// 0..cnt-1
// Expect: count >= 4, has_kbd == true, has_timer == true
// Depends: DriverRegistry
JARVIS_TEST(driver_registry_count, "PRE: iocd | POST: none") {
    size_t cnt = DriverRegistry::count();
    JARVIS_ASSERT(cnt >= 4);
    bool has_kbd = false, has_timer = false;
    for (size_t i = 0; i < cnt; ++i) {
        auto *d = DriverRegistry::get(i);
        if (d) {
            if (strcmp(d->name, "keyboard") == 0)
                has_kbd = true;
            if (strcmp(d->name, "timer") == 0)
                has_timer = true;
        }
    }
    JARVIS_ASSERT(has_kbd);
    JARVIS_ASSERT(has_timer);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - Verifies the IOCD server boots successfully
// Input: None (stub)
// Expect: JARVIS_TEST_PASS() (stub, no actual assertion yet)
// Depends: IOCD server, IPC
JARVIS_TEST(iocd_server_boots, "PRE: iocd | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - Verifies the keyboard driver is integrated in IOCD
// Input: None (stub)
// Expect: JARVIS_TEST_PASS() (stub, no actual assertion yet)
// Depends: IOCD server, keyboard driver, IPC
JARVIS_TEST(keyboard_driver_in_iocd, "PRE: iocd | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - Verifies the serial driver is integrated in IOCD
// Input: None (stub)
// Expect: JARVIS_TEST_PASS() (stub, no actual assertion yet)
// Depends: IOCD server, serial driver, IPC
JARVIS_TEST(serial_driver_in_iocd, "PRE: iocd | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - Verifies the driver server can access MMIO regions via
// capability-based permissions
// Input: None (stub)
// Expect: JARVIS_TEST_PASS() (stub, no actual assertion yet)
// Depends: Driver server, capability system, MMIO
JARVIS_TEST(driver_server_mmio_via_caps, "PRE: iocd | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: FLAW-08 — Serial::puts/putchar return promptly on a live COM1
//           (the bounded THRE polls must not hang and must emit).
// Input: Serial::puts with newline + mixed case + digits
// Expect: returns without hanging
// Depends: arch::Serial
JARVIS_TEST(serial_putchar_bounded_ok, "PRE: iocd | POST: none") {
    arch::Serial::puts("test-driver\n");
    arch::Serial::puts("ABCabc0129\n");
    arch::Serial::putchar('X');
    arch::Serial::putchar('\n');
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: FLAW-10 — Keyboard::init() must return (bounded PS/2 drain) and
//           leave the ring empty with modifiers cleared.
// Input: Keyboard::init(); getchar into a buffer; check mods
// Expect: returns; ring empty (getchar false); mods cleared
// Depends: arch::Keyboard
JARVIS_TEST(keyboard_init_bounded_drain, "PRE: iocd | POST: none") {
    arch::Keyboard::init();
    char c;
    bool got = arch::Keyboard::getchar(c);
    JARVIS_ASSERT(!got); // ring drained by init
    JARVIS_ASSERT(!arch::Keyboard::is_shifted());
    JARVIS_ASSERT(!arch::Keyboard::is_ctrl());
    JARVIS_ASSERT(!arch::Keyboard::is_alt());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all driver tests with the test framework
// Input: None
// Expect: All driver tests are registered (no direct assertion, only
// logging)
// Depends: Test framework, Logger, Scheduler, DriverRegistry
void register_driver_tests() {
    Logger::info("Registering driver tests");
    JARVIS_REGISTER_TEST(driver_registry_find);
    JARVIS_REGISTER_TEST(driver_registry_count);
    JARVIS_REGISTER_TEST(iocd_server_boots);
    JARVIS_REGISTER_TEST(keyboard_driver_in_iocd);
    JARVIS_REGISTER_TEST(serial_driver_in_iocd);
    JARVIS_REGISTER_TEST(driver_server_mmio_via_caps);
    JARVIS_REGISTER_TEST(serial_putchar_bounded_ok);
    JARVIS_REGISTER_TEST(keyboard_init_bounded_drain);
}
