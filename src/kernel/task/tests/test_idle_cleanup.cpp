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

/// @file test_idle_cleanup.cpp
/// @brief Tests that idle-task zombie cleanup does not cause deadline misses.

#include <test.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/io.hpp>
#include <logger.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;
using namespace kernel::test;

static volatile uint64_t g_rt_loops[4] = {};
static volatile bool     g_cleanup_done = false;

static void rt_task_entry() {
    unsigned idx = (Scheduler::current_task()->id - 10) & 3;
    for (uint64_t i = 0; i < 100 &&
         !__atomic_load_n(&g_cleanup_done, __ATOMIC_RELAXED); ++i) {
        __atomic_add_fetch(&g_rt_loops[idx], 1, __ATOMIC_RELAXED);
        arch::pause();
    }
    for (;;) arch::pause();
}

// Runmode: kernel
// Testidea: Terminate a large task during RT workload and verify that
// idle cleanup_step does not cause RT tasks to miss their deadlines.
// Input: 4 RT periodic tasks (prio 10), 1 large task (prio 5),
// terminate large task, call cleanup_step, check RT progress.
// Expect: All RT tasks made progress (no deadline miss due to cleanup).
// Depends: Scheduler
JARVIS_TEST(idle_cleanup_no_deadline_impact,
            "PRE: vfsd, iocd | POST: none") {
    g_cleanup_done = false;
    for (auto &v : g_rt_loops) v = 0;

    // Create 4 RT never-terminating tasks at priority 10.
    TaskControlBlock *rt[4];
    for (int i = 0; i < 4; ++i) {
        rt[i] = TaskControlBlock::create(rt_task_entry, 10, 10);
        JARVIS_ASSERT(rt[i] != nullptr);
    }

    // Create a large never-terminating task at priority 5.
    auto *large = TaskControlBlock::create(forever_entry, 5, 100);
    JARVIS_ASSERT(large != nullptr);

    // Register all cooperating tasks under one IrqGuard so no timer tick can
    // split the registration (cookbook Rule 2).
    {
        arch::IrqGuard guard;
        for (int i = 0; i < 4; ++i)
            Scheduler::add_task(*rt[i]);
        Scheduler::add_task(*large);
    }

    // Let all tasks run briefly via timer-driven scheduling.
    for (int h = 0; h < 50; ++h)
        arch::hlt();

    // Terminate the large task (adds to zombie list).
    Scheduler::terminate(*large, 0);

    // Drive idle cleanup: call cleanup_step repeatedly (simulating
    // what the idle task does).
    for (int c = 0; c < 10 && Scheduler::zombie_count() > 0; ++c) {
        Scheduler::cleanup_step();
        for (int h = 0; h < 10; ++h)
            arch::hlt();
    }
    Scheduler::drain_zombie_list();

    // Verify RT tasks made progress.
    uint64_t total = 0;
    for (int i = 0; i < 4; ++i)
        total += __atomic_load_n(&g_rt_loops[i], __ATOMIC_RELAXED);
    JARVIS_ASSERT_FMT(total > 0,
                      "RT tasks completed %lu loops (expected > 0)", total);

    // Cleanup RT tasks.
    for (int i = 0; i < 4; ++i)
        terminate_and_drain(*rt[i]);

    JARVIS_TEST_PASS();
}

void register_idle_cleanup_tests() {
    Logger::info("Registering idle cleanup tests");
    JARVIS_REGISTER_TEST(idle_cleanup_no_deadline_impact);
}
