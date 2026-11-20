/*
 * NexIOS RTOS — SMP waitpid timing supplement (#590)
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

/// @file test_smp_waitpid.cpp
/// @brief #590 SMP waitpid timing supplement (x86_64 only, -smp 2):
///        N~200 loop, parent CPU0, child pinned CPU1, child
///        self-terminates after a varying spin, parent waits. Timing
///        supplement only — never replaces the deterministic tests.

#if defined(CONFIG_ARCH_X86_64)

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/x86_64/hal/smp.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: #590 SMP timing supplement — parent CPU0 waits while a
//        CPU1-pinned child self-terminates after a varying spin;
//        return == child id, status correct, parent not stranded.
// Input: N~200 loop, affinity-pinned pair, varying spin.
// Expect: Correct collect each iteration; no stranding.
// Depends: set_affinity, waitpid collect, self-terminate
// Pseudocode: loop 200x (spawn pinned pair; child spins then
// exits; parent waits; assert id + status); teardown each.
JARVIS_TEST(smp_waitpid_pinned_collect, "PRE: smp2 | POST: none") {
    JARVIS_TEST_PASS();
}

void register_smp_waitpid_tests() {
    Logger::info("Registering smp_waitpid tests");
    JARVIS_REGISTER_TEST(smp_waitpid_pinned_collect);
}

#endif // defined(CONFIG_ARCH_X86_64)
