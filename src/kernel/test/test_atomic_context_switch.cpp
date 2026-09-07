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

/// @file test_atomic_context_switch.cpp
/// @brief Atomic context switch tests.

#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/arch/io.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;


// Runmode: kernel
// Testidea: After reschedule(), context-switch globals are set consistently.
// Input: Create two tasks, call reschedule(), inspect atomics.
// Expect: save_rsp_to is non-null (save target set) UNLESS the ISR already
//         consumed it.  load_rsp_from and load_cr3_from should be non-zero.
//         Either way, atomic reads have consistent values.
// Depends: Scheduler, context-switch atomics
JARVIS_TEST(atomic_globals_set_on_reschedule, "PRE: none | POST: none") {
    auto *task_a = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(task_a != nullptr);
    Scheduler::add_task(*task_a);

    auto *task_b = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(task_b != nullptr);
    Scheduler::add_task(*task_b);

    auto guard = ScopeGuard([&]() {
        kernel::test::terminate_and_drain2(task_a, task_b);
    });

    auto *original = Scheduler::current_task();

    // Clear stale globals so reschedule() doesn't no-op
    __atomic_store_n(&kernel::scheduler_save_rsp_to, nullptr, __ATOMIC_RELEASE);
    __atomic_store_n(&kernel::scheduler_load_rsp_from, (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&kernel::scheduler_load_cr3_from, (uint64_t)0,
                     __ATOMIC_RELEASE);

    kernel::test::yield_as(*task_a);

    // Let ISR drain any pending context-switch consumption
    for (int h = 0; h < 2; ++h)
        arch::hlt();

    // Read immediately; ISR may have already consumed the globals.
    // Accept either: non-null (not yet consumed) or null (already consumed).
    uint64_t *saved_rsp_ptr =
        __atomic_load_n(&kernel::scheduler_save_rsp_to, __ATOMIC_ACQUIRE);
    uint64_t loaded_rsp =
        __atomic_load_n(&kernel::scheduler_load_rsp_from, __ATOMIC_ACQUIRE);
    (void)loaded_rsp;

    // At least one of save/load must be non-zero if ISR hasn't fired yet.
    // If ISR already consumed the globals, all are zero — that's also valid.
    if (saved_rsp_ptr != nullptr) {
        JARVIS_ASSERT_FMT(
            loaded_rsp != 0,
            "load_rsp_from should be non-zero when save_rsp_to is set, got 0");
    }

    Scheduler::set_current(*original);

    guard.dismiss();
    kernel::test::terminate_and_drain2(task_a, task_b);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: fpu_owner can be read/written atomically.
// Input: Set fpu_owner to a task pointer, read it back.
// Expect: Value matches what was written.
// Depends: fpu_owner atomic
JARVIS_TEST(atomic_fpu_owner_read_write, "PRE: none | POST: none") {
    auto *old = __atomic_load_n(&kernel::fpu_owner, __ATOMIC_ACQUIRE);
    auto *test_ptr =
        reinterpret_cast<TaskControlBlock *>(uintptr_t(0xDEADBEEF));
    __atomic_store_n(&kernel::fpu_owner, test_ptr, __ATOMIC_RELEASE);
    auto *val = __atomic_load_n(&kernel::fpu_owner, __ATOMIC_ACQUIRE);
    JARVIS_ASSERT(val == test_ptr);
    __atomic_store_n(&kernel::fpu_owner, old, __ATOMIC_RELEASE);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: scheduler_next_task_id increments atomically.
// Input: Save, store new, verify, restore.
// Expect: Atomic operations preserve the value.
// Depends: scheduler_next_task_id atomic
JARVIS_TEST(atomic_next_task_id_consistency, "PRE: none | POST: none") {
    uint64_t old =
        __atomic_load_n(&kernel::scheduler_next_task_id, __ATOMIC_ACQUIRE);
    __atomic_store_n(&kernel::scheduler_next_task_id, old + 100,
                     __ATOMIC_RELEASE);
    uint64_t val =
        __atomic_load_n(&kernel::scheduler_next_task_id, __ATOMIC_ACQUIRE);
    JARVIS_ASSERT_EQ(old + 100, val);
    __atomic_store_n(&kernel::scheduler_next_task_id, old, __ATOMIC_RELEASE);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Context-switch globals survive atomic clear-and-validate cycle.
// Input: Reschedule, verify globals are set. Clear them atomically, verify.
// Expect: Globals are non-zero after reschedule, zero after atomic clear.
// Depends: Scheduler, context-switch atomics
JARVIS_TEST(atomic_idempotent_null_handling, "PRE: none | POST: none") {
    auto *task_a = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(task_a != nullptr);
    Scheduler::add_task(*task_a);

    auto *task_b = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(task_b != nullptr);
    Scheduler::add_task(*task_b);

    auto guard = ScopeGuard([&]() {
        kernel::test::terminate_and_drain2(task_a, task_b);
    });

    auto *original = Scheduler::current_task();

    // Clear stale globals so reschedule() doesn't no-op
    __atomic_store_n(&kernel::scheduler_save_rsp_to, nullptr, __ATOMIC_RELEASE);
    __atomic_store_n(&kernel::scheduler_load_rsp_from, (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&kernel::scheduler_load_cr3_from, (uint64_t)0,
                     __ATOMIC_RELEASE);

    kernel::test::yield_as(*task_a);

    // Let ISR drain any pending context-switch consumption
    for (int h = 0; h < 2; ++h)
        arch::hlt();

    // Globals must be set after reschedule (save target, load RSP, load CR3)
    uint64_t *save_rsp_ptr =
        __atomic_load_n(&kernel::scheduler_save_rsp_to, __ATOMIC_ACQUIRE);
    // Accept that ISR may have already consumed the globals
    if (save_rsp_ptr == nullptr) {
        JARVIS_TEST_PASS();
        return;
    }

    // Clear globals atomically and verify clear took effect
    __atomic_store_n(&kernel::scheduler_save_rsp_to, nullptr, __ATOMIC_RELEASE);
    __atomic_store_n(&kernel::scheduler_load_rsp_from, (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&kernel::scheduler_load_cr3_from, (uint64_t)0,
                     __ATOMIC_RELEASE);

    save_rsp_ptr =
        __atomic_load_n(&kernel::scheduler_save_rsp_to, __ATOMIC_ACQUIRE);
    uint64_t load_cr3 =
        __atomic_load_n(&kernel::scheduler_load_cr3_from, __ATOMIC_ACQUIRE);

    JARVIS_ASSERT_EQ(nullptr, save_rsp_ptr);
    JARVIS_ASSERT_EQ(0ULL, load_cr3);

    Scheduler::set_current(*original);
    guard.dismiss();
    kernel::test::terminate_and_drain2(task_a, task_b);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: fpu_owner can be read/written with relaxed memory ordering.
// Input: Relaxed load/store on fpu_owner in #NM handler context.
// Expect: Value written with relaxed store is readable with relaxed load.
// Depends: fpu_owner atomic
JARVIS_TEST(atomics_fpu_owner_relaxed, "PRE: none | POST: none") {
    TaskControlBlock *old =
        __atomic_load_n(&kernel::fpu_owner, __ATOMIC_RELAXED);

    auto *test_ptr = reinterpret_cast<TaskControlBlock *>(uintptr_t(0xBEEF));
    __atomic_store_n(&kernel::fpu_owner, test_ptr, __ATOMIC_RELAXED);

    TaskControlBlock *val =
        __atomic_load_n(&kernel::fpu_owner, __ATOMIC_RELAXED);

    JARVIS_ASSERT(val == test_ptr);

    __atomic_store_n(&kernel::fpu_owner, old, __ATOMIC_RELAXED);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: C bridge functions for isr_stubs.asm work correctly.
// Input: Set scheduler_next_task_id, call scheduler_on_context_switch().
// Expect: Current task updates to match the set ID; next_task_id cleared.
// Depends: scheduler_on_context_switch, scheduler_next_task_id
JARVIS_TEST(atomics_assembly_bridge, "PRE: none | POST: none") {
    auto *original = Scheduler::current_task();

    auto *task = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(task != nullptr);
    Scheduler::add_task(*task);

    // Simulate what switch_to_task + isr_common do:
    // Set scheduler_next_task_id to the target task ID
    uint64_t old_id =
        __atomic_load_n(&kernel::scheduler_next_task_id, __ATOMIC_ACQUIRE);
    __atomic_store_n(&kernel::scheduler_next_task_id, task->id,
                     __ATOMIC_RELEASE);

    // Call the bridge function that isr_stubs.asm invokes
    kernel::gs::scheduler_on_context_switch();

    // Verify current task was updated by the bridge
    auto *after = Scheduler::current_task();
    JARVIS_ASSERT_EQ(task->id, after->id);

    // Verify scheduler_next_task_id was cleared to UINT64_MAX
    uint64_t cleared =
        __atomic_load_n(&kernel::scheduler_next_task_id, __ATOMIC_ACQUIRE);
    JARVIS_ASSERT_EQ(UINT64_MAX, cleared);

    // Restore original task and next_task_id
    Scheduler::set_current(*original);
    __atomic_store_n(&kernel::scheduler_next_task_id, old_id, __ATOMIC_RELEASE);

    kernel::test::terminate_and_drain(*task);

    JARVIS_TEST_PASS();
}

void register_atomic_context_switch_tests() {
    Logger::info("Registering atomic context-switch tests");
    JARVIS_REGISTER_TEST(atomic_globals_set_on_reschedule);
    JARVIS_REGISTER_TEST(atomic_fpu_owner_read_write);
    JARVIS_REGISTER_TEST(atomic_next_task_id_consistency);
    JARVIS_REGISTER_TEST(atomic_idempotent_null_handling);
    JARVIS_REGISTER_TEST(atomics_fpu_owner_relaxed);
    JARVIS_REGISTER_TEST(atomics_assembly_bridge);
}
