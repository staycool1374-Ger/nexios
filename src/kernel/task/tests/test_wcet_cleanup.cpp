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

/// @file test_wcet_cleanup.cpp
/// @brief WCET benchmark for zombie cleanup_step().

#include <test.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/timer.hpp>
#include <logger.hpp>

using namespace kernel;
using namespace kernel::test;

// Runmode: kernel
// Testidea: Measure max cycles of Scheduler::cleanup_step().
// Creates N tasks, terminates them, drains, and measures.
// Depends: Scheduler, TaskControlBlock, arch::Timer
JARVIS_TEST(wcet_cleanup_step, "PRE: none | POST: none") {
    uint64_t max_cycles = 0;
    // Test with 0, 1, 10 zombies
    const uint64_t counts[] = {0, 1, 10};
    constexpr uint64_t ITERS = 100;

    for (size_t ci = 0; ci < sizeof(counts)/sizeof(counts[0]); ++ci) {
        uint64_t n = counts[ci];
        for (uint64_t iter = 0; iter < ITERS; ++iter) {
            // Create N tasks at guard-page-tested priority (maps to 64K stack).
            for (uint64_t j = 0; j < n; ++j) {
                auto *t = TaskControlBlock::create([]() {}, 5, 10);
                if (!t) break;
                Scheduler::add_task(*t);
                Scheduler::terminate(*t, 0);
            }
            uint64_t start = arch::Timer::ticks();
            Scheduler::cleanup_step();
            uint64_t end = arch::Timer::ticks();
            uint64_t elapsed = end - start;
            if (elapsed > max_cycles)
                max_cycles = elapsed;
            Scheduler::drain_zombie_list();
        }
    }

    Logger::info("[WCET] cleanup_step max=%lu ticks (%lu zombies)",
                 max_cycles, counts[2]);
    // No assertion — benchmark only, record in wcet_analysis.md.
    JARVIS_TEST_PASS();
}

void register_wcet_cleanup_tests() {
    Logger::info("Registering WCET cleanup tests");
    JARVIS_REGISTER_TEST(wcet_cleanup_step);
}
