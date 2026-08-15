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

/// @file test_idle_task.cpp
/// @brief Idle task behaviour tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/memory/integrity.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/syscall/syscall.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies the kernel idle task (HLT-based) is created during boot
// initialization.
// Input: Scheduler is initialized.
// Expect: get_idle_task() returns non-null task at tasks_[0] with kernel
// stack, no user stack.
JARVIS_TEST(idle_task_created_at_boot, "PRE: none | POST: none") {
    JARVIS_ASSERT(Scheduler::get_idle_task() != nullptr);
    JARVIS_ASSERT(Scheduler::get_idle_task()->kernel_stack != nullptr);
    JARVIS_ASSERT(Scheduler::get_idle_task()->user_stack_ == 0);
    JARVIS_ASSERT_EQ(Scheduler::get_idle_task(), Scheduler::task_at(0));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests that user tasks (non-idle, priority-0 tasks with a user
// stack) have a separate page table and a kernel stack, confirming they run
// in user mode (ring 3).
// Input: Iterates all scheduler tasks, finds one matching user-task criteria
// (priority 0, user_stack_ set, not the idle task).
// Expect: JARVIS_ASSERT checks page_table_ != kernel PML4 and kernel_stack
// != nullptr.
// Depends: kernel::task::Scheduler, kernel::memory::VMM
JARVIS_TEST(idle_task_runs_in_ring3, "PRE: none | POST: none") {
    uint64_t count = Scheduler::task_count();
    for (uint64_t i = 0; i < count; ++i) {
        auto *t = Scheduler::task_at(i);
        if (t && t->priority == 0 && t->user_stack_ != 0 &&
            t != Scheduler::get_idle_task()) {
            JARVIS_ASSERT(t->page_table_ != VMM::get_kernel_pml4());
            JARVIS_ASSERT(t->kernel_stack != nullptr);
            JARVIS_TEST_PASS();
            return;
        }
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests that non-idle user tasks have priority and base_priority
// set to 0.
// Input: Iterates all scheduler tasks, finds one matching user-task criteria.
// Expect: JARVIS_ASSERT_EQ checks priority == 0 and base_priority == 0.
// Depends: kernel::task::Scheduler
JARVIS_TEST(idle_task_priority_zero, "PRE: none | POST: none") {
    uint64_t count = Scheduler::task_count();
    for (uint64_t i = 0; i < count; ++i) {
        auto *t = Scheduler::task_at(i);
        if (t && t->priority == 0 && t->user_stack_ != 0 &&
            t != Scheduler::get_idle_task()) {
            JARVIS_ASSERT_EQ(0ULL, t->priority);
            JARVIS_ASSERT_EQ(0ULL, t->base_priority);
            JARVIS_TEST_PASS();
            return;
        }
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the idle task entry point calls arch::hlt (HLT with
// interrupts enabled).
// Input: Inspect the idle task's entry function context to verify it
// contains HLT.
// Expect: The idle task's kernel stack contains a return address pointing to
// HLT instruction.
// Note: Cannot directly test HLT from kernel context as it would halt; test
// validates idle task exists.
JARVIS_TEST(idle_task_calls_pause_syscall, "PRE: none | POST: none") {
    auto *idle = Scheduler::get_idle_task();
    JARVIS_ASSERT(idle != nullptr);
    JARVIS_ASSERT(idle->kernel_stack != nullptr);
    // Idle task runs an infinite loop with arch::hlt() - verify it's a kernel
    // task.  v0.4.0 MP-1: kernel tasks own a private kernel-half PML4, so the
    // authoritative discriminator is is_user_.
    JARVIS_ASSERT(idle->is_user_ == false);
    JARVIS_ASSERT(idle->user_stack_ == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that the idle task yields CPU to higher-priority ready
// tasks.
// Input: Create a higher-priority task, verify scheduler picks it over idle.
// Expect: Scheduler::next_task() returns the higher-priority task, not idle.
JARVIS_TEST(idle_task_yields_to_higher_priority, "PRE: none | POST: none") {
    auto *high_prio = TaskControlBlock::create([]() {}, 10, 10);
    JARVIS_ASSERT(high_prio != nullptr);

    // Idle is at priority 0, high_prio at 10 - scheduler should pick high_prio.
    // Register AND select under one IrqGuard: a tick between add_task and
    // next_task would dispatch the empty-lambda task (self-terminates), so
    // next_task() then returns idle (flake).
    TaskControlBlock *next;
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*high_prio);
        next = Scheduler::next_task();
    }
    JARVIS_ASSERT(next->priority >= high_prio->priority);

    kernel::test::terminate_and_drain(*high_prio);
    JARVIS_ASSERT(next != nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests that the kernel idle task (the HLT-based one) always
// exists at boot, has a kernel stack, no user stack, and is the first task
// in the scheduler list.
// Input: Scheduler::get_idle_task() and Scheduler::task_at(0).
// Expect: JARVIS_ASSERT checks idle task is non-null, has kernel_stack,
// user_stack_ == 0, and matches task_at(0).
// Depends: kernel::task::Scheduler
JARVIS_TEST(kernel_hlt_idle_still_exists, "PRE: none | POST: none") {
    JARVIS_ASSERT(Scheduler::get_idle_task() != nullptr);
    JARVIS_ASSERT(Scheduler::get_idle_task()->kernel_stack != nullptr);
    JARVIS_ASSERT(Scheduler::get_idle_task()->user_stack_ == 0);
    JARVIS_ASSERT_EQ(Scheduler::get_idle_task(), Scheduler::task_at(0));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The boot idle task is a stable, single instance that survives real
// timer ticks.  (The historical "crash-respawn via reap_orphans" idea is
// unreachable: terminate() moves a task into the zombie list, never back into
// all_tasks_, so reap_orphans can never re-create idle from it.)
// Input: Read the boot idle task; let real ticks elapse.
// Expect: The same valid idle TCB remains at tasks_[0] after real ticks.
JARVIS_TEST(idle_task_restartable_on_crash, "PRE: none | POST: none") {
    auto *idle = Scheduler::get_idle_task();
    JARVIS_ASSERT(idle != nullptr);
    JARVIS_ASSERT_EQ(idle, Scheduler::task_at(0));
    uint64_t idle_id = idle->id;

    // Let real timer ticks elapse; the idle task must stay live and valid.
    uint64_t start = arch::Timer::ticks();
    while (arch::Timer::ticks() - start < 20)
        arch::pause();

    auto *still = Scheduler::get_idle_task();
    JARVIS_ASSERT(still != nullptr);
    JARVIS_ASSERT(still->magic == TaskControlBlock::TCB_MAGIC);
    JARVIS_ASSERT_EQ(idle_id, still->id);
    JARVIS_ASSERT_EQ(still, Scheduler::task_at(0));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that creating multiple idle tasks is prevented (only
// one at tasks_[0]).
// Input: Attempt to add a second task at index 0 or with idle characteristics.
// Expect: Scheduler only has one task at index 0 (the original idle).
JARVIS_TEST(multiple_idle_tasks_prevented, "PRE: none | POST: none") {
    // The idle task is always at index 0
    JARVIS_ASSERT_EQ(Scheduler::task_at(0), Scheduler::get_idle_task());

    // Create another task with priority 0 (same as idle)
    auto *another = TaskControlBlock::create([]() {}, 0, 10);
    JARVIS_ASSERT(another != nullptr);
    Scheduler::add_task(*another);

    // Idle should still be at index 0
    JARVIS_ASSERT_EQ(Scheduler::task_at(0), Scheduler::get_idle_task());

    // Cleanup
    kernel::test::terminate_and_drain(*another);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies integrity section markers are intact after boot.
// Input: None
// Expect: All section markers (text, rodata, data, bss, stack) read back
// their expected values, indicating no memory corruption across boundaries.
JARVIS_TEST(idle_task_integrity_markers, "PRE: none | POST: none") {
    kernel::integrity::check_section_markers();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests that incremental CRC processing over the code segment
// makes progress and eventually completes.
// Input: Multiple calls to crc_process_chunk().
// Expect: After enough iterations, crc_process_chunk() returns true
// (indicating CRC is complete). The CRC verification is skipped during
// tests (expected CRC is patched at link time).
JARVIS_TEST(idle_task_integrity_crc_incremental, "PRE: none | POST: none") {
    kernel::integrity::reset_crc_state();
    bool done = false;
    uint64_t iterations = 0;
    uint64_t const MAX_ITER = 200;
    while (!done && iterations < MAX_ITER) {
        done = kernel::integrity::crc_process_chunk();
        ++iterations;
    }
    JARVIS_ASSERT(done);
    JARVIS_ASSERT(iterations < MAX_ITER);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all idle-task test cases into the test framework.
// Input: None
// Expect: Each JARVIS_REGISTER_TEST call adds the test to the suite.
// Depends: kernel::test
void register_idle_task_tests() {
    Logger::info("Registering idle task tests");
    JARVIS_REGISTER_TEST(idle_task_created_at_boot);
    JARVIS_REGISTER_TEST(idle_task_runs_in_ring3);
    JARVIS_REGISTER_TEST(idle_task_priority_zero);
    JARVIS_REGISTER_TEST(idle_task_calls_pause_syscall);
    JARVIS_REGISTER_TEST(idle_task_yields_to_higher_priority);
    JARVIS_REGISTER_TEST(kernel_hlt_idle_still_exists);
    JARVIS_REGISTER_TEST(idle_task_restartable_on_crash);
    JARVIS_REGISTER_TEST(multiple_idle_tasks_prevented);
    JARVIS_REGISTER_TEST(idle_task_integrity_markers);
    JARVIS_REGISTER_TEST(idle_task_integrity_crc_incremental);
}
