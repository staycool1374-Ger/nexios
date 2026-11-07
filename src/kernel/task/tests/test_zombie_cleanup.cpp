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

/// @file test_zombie_cleanup.cpp
/// @brief ZombieList deferred cleanup tests.

#include <test.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <logger.hpp>

using namespace kernel;
using namespace kernel::test;

// --- self-terminating entry ---
static void self_term_entry() {
    Scheduler::terminate(*Scheduler::current_task(), 42);
    for (;;) arch::hlt();
}

// Runmode: kernel
// Testidea: A task that explicitly self-terminates reaches the zombie list
// and idle cleanup frees it.  Tests the self-termination path in
// Scheduler::terminate() which calls release_zombie() before switch_away.
// Input: Create task that calls terminate(self, 42).  Wait for it to run.
// Expect: task state == TERMINATED, exit_code == 42, zombie_count incremented.
// Depends: Scheduler terminate self-path, TaskControlBlock, release_zombie
JARVIS_TEST(test_release_zombie_self, "PRE: none | POST: none") {
    auto *task = TaskControlBlock::create(self_term_entry, 20, 10);
    JARVIS_ASSERT(task != nullptr);
    // Pinned FIXED (issue #19): dispatches by priority exactly as pre-EDF
    // (20 > harness 10); EDF would also dispatch it but the zombie-count
    // asserts below assume no mid-test double-append races.
    JARVIS_ASSERT(Scheduler::set_sched_policy(*task, SchedPolicy::FIXED));
    Scheduler::add_task(*task);

    for (int h = 0; h < 50 && task->state != TaskState::TERMINATED; ++h) {
        Scheduler::reschedule();
        arch::hlt();
    }

    JARVIS_ASSERT(task->state == TaskState::TERMINATED);
    JARVIS_ASSERT(task->exit_code == 42);
    JARVIS_ASSERT(Scheduler::zombie_count() > 0);

    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(Scheduler::zombie_count() == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: When zombie_count exceeds CONFIG_ZOMBIE_STARVATION_LIMIT,
// the on_tick watchdog force-flushes a batch.
// Input: Create LIMIT+2 tasks, terminate all.  Spin-wait >100 ticks.
// Expect: zombie_count drops below the initial count after watchdog fires.
// Depends: Scheduler on_tick watchdog, flush_zombies
JARVIS_TEST(test_zombie_starvation_watchdog, "PRE: none | POST: none") {
    uint64_t const COUNT = CONFIG_ZOMBIE_STARVATION_LIMIT + 2;
    TaskControlBlock *tasks[COUNT];
    for (uint64_t i = 0; i < COUNT; ++i) {
        tasks[i] = TaskControlBlock::create([]() {}, 5, 10);
        JARVIS_ASSERT(tasks[i] != nullptr);
        // Pinned FIXED (issue #19): counting fixtures must not dispatch
        // mid-loop (see zombie_drain_multiple above).
        JARVIS_ASSERT(
            Scheduler::set_sched_policy(*tasks[i], SchedPolicy::FIXED));
        Scheduler::add_task(*tasks[i]);
    }
    for (uint64_t i = 0; i < COUNT; ++i)
        Scheduler::terminate(*tasks[i], 0);

    uint64_t initial = Scheduler::zombie_count();
    JARVIS_ASSERT(initial == COUNT);

    // Wait 300+ ticks so the watchdog fires at least twice
    // (fires every 100 ticks, flushes up to LIMIT/2 per shot).
    for (int h = 0; h < 300; ++h)
        arch::hlt();

    uint64_t remaining = Scheduler::zombie_count();
    JARVIS_ASSERT_FMT(remaining < initial,
                      "Watchdog did not flush: %lu remaining (initial %lu)",
                      remaining, initial);

    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(Scheduler::zombie_count() == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Terminate a task and verify idle cleanup_step frees its resources.
// Input: Create a kernel task, add to scheduler, terminate.
// Expect: clean_step pops the zombie, cleanup + MemPool::free runs.
// Depends: Scheduler, TaskControlBlock, MemPool, PMM
JARVIS_TEST(zombie_cleanup_step_frees_resources, "PRE: none | POST: none") {
    auto *task = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(task != nullptr);
    Scheduler::add_task(*task);

    Scheduler::terminate(*task, 0);

    // The zombie is in the zombie list.  Simulate idle cleanup.
    Scheduler::cleanup_step();

    // After cleanup_step, the TCB's resources should be returned to PMM.
    // The TCB itself (one MemPool block) is freed, but MemPool keeps it
    // internally — PMM pages freed by cleanup (kernel stack, page tables)
    // are the observable change.
    JARVIS_ASSERT(Scheduler::zombie_count() == 0);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verify that drain_zombie_list frees multiple zombies at once.
// Input: Create 3 tasks, terminate them all.
// Expect: drain_zombie_list frees all 3; zombie count reaches 0.
// Depends: Scheduler, TaskControlBlock
JARVIS_TEST(zombie_drain_multiple, "PRE: none | POST: none") {
    TaskControlBlock *tasks[3];
    for (uint64_t i = 0; i < 3; ++i) {
        tasks[i] = TaskControlBlock::create([]() {}, 5, 10);
        JARVIS_ASSERT(tasks[i] != nullptr);
        // Pinned FIXED (issue #19): counting fixtures must not dispatch
        // mid-loop (an EDF dispatch would self-terminate them and the
        // loop's terminate() would double-append zombies).
        JARVIS_ASSERT(
            Scheduler::set_sched_policy(*tasks[i], SchedPolicy::FIXED));
        Scheduler::add_task(*tasks[i]);
    }

    for (uint64_t i = 0; i < 3; ++i)
        Scheduler::terminate(*tasks[i], 0);

    JARVIS_ASSERT(Scheduler::zombie_count() == 3);

    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(Scheduler::zombie_count() == 0);

    JARVIS_TEST_PASS();
}

void register_zombie_cleanup_tests() {
    Logger::info("Registering zombie cleanup tests");
    JARVIS_REGISTER_TEST(zombie_cleanup_step_frees_resources);
    JARVIS_REGISTER_TEST(zombie_drain_multiple);
    JARVIS_REGISTER_TEST(test_release_zombie_self);
    JARVIS_REGISTER_TEST(test_zombie_starvation_watchdog);
}
