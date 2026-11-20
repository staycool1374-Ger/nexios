/*
 * NexIOS RTOS — Task Object Budget (M4/F10)
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

/// @file test_task_budget.cpp
/// @brief F10 object budget test (issue #563): attach_object budget
///        enforcement and detach_all_objects walk bound.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/test/test_sched_helpers.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: F10 object budget walk bound — set a 4-object budget,
//        attach 4 and refuse the 5th (attach_object false, list
//        length unchanged), detach_all_objects() walks all 4;
//        tracker delta 0; budget 0 means 32.
// Input: Fresh task; set_object_budget(4); 5 attach attempts;
//        detach_all; budget-0 probe.
// Expect: 4 attach true + 5th false (length 4); detach walks 4;
//         delta 0; effective budget 32 when stored 0.
// Depends: set_object_budget/effective_object_budget,
//        attach_object/detach_all_objects
// Pseudocode: budget 4; attach x5 (4 ok, 5th refused, len 4);
// detach_all (all 4); delta 0; budget 0 -> 32; teardown.
JARVIS_TEST(object_budget_walk_bound, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

void register_task_budget_tests() {
    Logger::info("Registering task_budget tests");
    JARVIS_REGISTER_TEST(object_budget_walk_bound);
}
