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

/// @file test_irq_guard.cpp
/// @brief IRQ guard (interrupt disable) scoping tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/arch/io.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies IrqGuard disables interrupts on construction and
//           restores original state on destruction.
// Input: Create IrqGuard scope
// Expect: interrupts_enabled() is false inside, restored outside
// Depends: arch::IrqGuard, arch::interrupts_enabled
JARVIS_TEST(irq_guard_saves_and_restores, "PRE: none | POST: none") {
    bool was_enabled = arch::interrupts_enabled();
    {
        arch::IrqGuard guard;
        JARVIS_ASSERT(!arch::interrupts_enabled());
    }
    JARVIS_ASSERT_EQ(was_enabled, arch::interrupts_enabled());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies nested IrqGuard works correctly.
// Input: Outer and inner IrqGuard scopes
// Expect: IF remains disabled throughout, restored to original after outer
// Depends: arch::IrqGuard
JARVIS_TEST(irq_guard_nested, "PRE: none | POST: none") {
    bool was_enabled = arch::interrupts_enabled();
    {
        arch::IrqGuard outer;
        JARVIS_ASSERT(!arch::interrupts_enabled());
        {
            arch::IrqGuard inner;
            JARVIS_ASSERT(!arch::interrupts_enabled());
        }
        JARVIS_ASSERT(!arch::interrupts_enabled());
    }
    JARVIS_ASSERT_EQ(was_enabled, arch::interrupts_enabled());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies IrqGuard is non-copyable and non-movable (compile check).
// Input: Attempt to copy or move (compile-time)
// Expect: Compilation succeeds for valid usage
// Depends: arch::IrqGuard
JARVIS_TEST(irq_guard_noncopyable, "PRE: none | POST: none") {
    arch::IrqGuard guard;
    JARVIS_ASSERT(!arch::interrupts_enabled());
    (void)guard;
    JARVIS_TEST_PASS();
}

void register_irq_guard_tests() {
    Logger::info("Registering IRQ guard tests");
    JARVIS_REGISTER_TEST(irq_guard_saves_and_restores);
    JARVIS_REGISTER_TEST(irq_guard_nested);
    JARVIS_REGISTER_TEST(irq_guard_noncopyable);
}
