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

/// @file test_idle_monitor.cpp
/// @brief Idle-task safety monitor foundation tests (issues #43/#282).
///        Normative contract: docs/specs/idle_monitor.md §1 (M1-M6).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/idle_monitor.hpp>
#include <kernel/nexios_config.h>
#include "test_sched_helpers.hpp"

using namespace kernel;
using namespace kernel::test;

// Runmode: kernel
// Testidea: New P1a monitor fields are zero at creation (both kernel and
// user creation sites; clone + elf-finalize share the same memset pattern,
// verified in code at task.cpp:1424 and elf.cpp:537).
// Input: TaskControlBlock::create() + create_user() fresh TCBs.
// Expect: all 7 fields zero/false.
JARVIS_TEST(idle_monitor_zero_init, "PRE: none | POST: none") {
    auto *t1 = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(t1 != nullptr);
    JARVIS_ASSERT(t1->util_per_mille == 0);
    JARVIS_ASSERT(t1->block_per_mille == 0);
    JARVIS_ASSERT(t1->preempt_per_mille == 0);
    JARVIS_ASSERT(t1->deadline_meets == 0);
    JARVIS_ASSERT(t1->stack_low_water_bytes == 0);
    JARVIS_ASSERT(t1->last_progress_tick == 0);
    JARVIS_ASSERT(t1->stuck_suspected == false);
    TaskControlBlock::destroy(t1);

    auto *t2 = TaskControlBlock::create_user(
        forever_entry, 5, 10, 32_KiB);
    JARVIS_ASSERT(t2 != nullptr);
    JARVIS_ASSERT(t2->util_per_mille == 0);
    JARVIS_ASSERT(t2->block_per_mille == 0);
    JARVIS_ASSERT(t2->preempt_per_mille == 0);
    JARVIS_ASSERT(t2->deadline_meets == 0);
    JARVIS_ASSERT(t2->stack_low_water_bytes == 0);
    JARVIS_ASSERT(t2->last_progress_tick == 0);
    JARVIS_ASSERT(t2->stuck_suspected == false);
    t2->cleanup();
    delete t2;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A full stack scan over intact tasks scans without firing the
// escalation path and publishes low-water snapshots.
// Input: Two BLOCKED registered tasks (never dispatched, never switched
// out) + idle_scan_stack with an unbounded budget.
// Expect: scanned >= 2, wrapped, tasks still valid; never-switched-out
// tasks publish stack_low_water_bytes == 0.
JARVIS_TEST(idle_scan_stack_intact, "PRE: none | POST: none") {
    auto *a = TaskControlBlock::create(forever_entry, 5, 10);
    auto *b = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(a != nullptr && b != nullptr);
    a->state = TaskState::BLOCKED;
    b->state = TaskState::BLOCKED;
    Scheduler::register_task(*a);
    Scheduler::register_task(*b);

    IdleScanCursor cursor = {};
    const IdleScanProgress progress = idle_scan_stack(cursor, 1000000ULL);
    JARVIS_ASSERT(progress.scanned >= 2);
    JARVIS_ASSERT(progress.wrapped == true);
    JARVIS_ASSERT(TaskControlBlock::is_valid(a));
    JARVIS_ASSERT(TaskControlBlock::is_valid(b));
    JARVIS_ASSERT(a->stack_low_water_bytes == 0);
    JARVIS_ASSERT(b->stack_low_water_bytes == 0);

    Scheduler::remove_task(*a);
    Scheduler::remove_task(*b);
    TaskControlBlock::destroy(a);
    TaskControlBlock::destroy(b);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A corrupted canary feeds the EXISTING hook path and nothing
// else (M6): the scanner performs no kill/reap/reprioritize itself.
// Input: BLOCKED registered task with a corrupted kernel-stack canary
// (restored before teardown so the switch-path guard never sees it).
// Expect: task still valid + still BLOCKED after the scan (the strong
// test hook in test_stack_alloc.cpp latches and returns while
// is_test_active(); production fail-stops via the same existing path).
JARVIS_TEST(idle_scan_stack_corrupt_hook_only, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(canary_verify_kernel_stack(t));
    t->state = TaskState::BLOCKED;
    Scheduler::register_task(*t);

    uint64_t saved = 0;
    __builtin_memcpy(&saved, t->kernel_stack, 8);
    const uint64_t corrupt = saved ^ 0xFFu;
    __builtin_memcpy(t->kernel_stack, &corrupt, 8);
    JARVIS_ASSERT(!canary_verify_kernel_stack(t));

    IdleScanCursor cursor = {};
    (void)idle_scan_stack(cursor, 1000000ULL);

    JARVIS_ASSERT(TaskControlBlock::is_valid(t));
    JARVIS_ASSERT(t->state == TaskState::BLOCKED);

    __builtin_memcpy(t->kernel_stack, &saved, 8);
    JARVIS_ASSERT(canary_verify_kernel_stack(t));
    Scheduler::remove_task(*t);
    TaskControlBlock::destroy(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The scanner skips what it must not touch (M4): idle tasks
// and the currently-running task are counted as skipped, never scanned.
// Input: Full unbounded stack scan.
// Expect: skipped >= 1 (at minimum the running harness task itself);
// the registry head (idle) is never reported as scanned-victim.
JARVIS_TEST(idle_scan_skips_idle_and_current, "PRE: none | POST: none") {
    IdleScanCursor cursor = {};
    const IdleScanProgress progress = idle_scan_stack(cursor, 1000000ULL);
    JARVIS_ASSERT(progress.wrapped == true);
    JARVIS_ASSERT(progress.skipped >= 1);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Chunked scanning resumes from the cursor and covers exactly
// the same set as one unbounded pass (M2).
// Input: Full pass (reference totals) vs repeated budget=1 passes.
// Expect: summed scanned/skipped equal the reference; final pass wraps.
JARVIS_TEST(idle_scan_chunk_cursor_resumes, "PRE: none | POST: none") {
    IdleScanCursor ref_cursor = {};
    const IdleScanProgress ref = idle_scan_stack(ref_cursor, 1000000ULL);
    JARVIS_ASSERT(ref.wrapped == true);

    IdleScanCursor cursor = {};
    uint64_t scanned = 0;
    uint64_t skipped = 0;
    bool wrapped = false;
    for (uint64_t i = 0; i < 1000000ULL; ++i) {
        const IdleScanProgress step = idle_scan_stack(cursor, 1);
        scanned += step.scanned;
        skipped += step.skipped;
        if (step.wrapped) {
            wrapped = true;
            break;
        }
    }
    JARVIS_ASSERT(wrapped == true);
    JARVIS_ASSERT(scanned == ref.scanned);
    JARVIS_ASSERT(skipped == ref.skipped);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Stall detection sets the flag and nothing else (M6): no
// watchdog is synthesized, no state change beyond stuck_suspected.
// Input: BLOCKED registered task with last_progress_tick far in the
// past + a fresh task (tick 0, never bumped — P6 owns bump sites).
// Expect: stale task flagged, fresh task not; wdog_armed stays false.
JARVIS_TEST(idle_stall_flag_no_kill, "PRE: none | POST: none") {
    auto *stale = TaskControlBlock::create(forever_entry, 5, 10);
    auto *fresh = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(stale != nullptr && fresh != nullptr);
    stale->state = TaskState::BLOCKED;
    fresh->state = TaskState::BLOCKED;
    Scheduler::register_task(*stale);
    Scheduler::register_task(*fresh);

    const uint64_t threshold = static_cast<uint64_t>(
        CONFIG_IDLE_MONITOR_STALL_THRESHOLD_TICKS);
    stale->last_progress_tick = 100;
    const uint64_t now = 100 + threshold + 1;

    IdleScanCursor cursor = {};
    const IdleScanProgress progress = idle_scan_stall(cursor, 1000000ULL,
                                                      now);
    JARVIS_ASSERT(progress.scanned >= 2);
    JARVIS_ASSERT(stale->stuck_suspected == true);
    JARVIS_ASSERT(fresh->stuck_suspected == false);
    JARVIS_ASSERT(stale->wdog_armed == false);
    JARVIS_ASSERT(stale->state == TaskState::BLOCKED);

    Scheduler::remove_task(*stale);
    Scheduler::remove_task(*fresh);
    TaskControlBlock::destroy(stale);
    TaskControlBlock::destroy(fresh);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Low-water publish is a pure saturating conversion (seam unit
// test, no registry involved).
// Input: Synthetic (low_water_rsp, stack_top) pairs.
// Expect: 0 maps to 0; top maps to 0; normal delta exact; huge delta
// saturates at UINT32_MAX.
JARVIS_TEST(idle_publish_low_water, "PRE: none | POST: none") {
    uint32_t dst = 0xFFFFFFFFu;
    idle_publish_stack_low_water(0, 0x1000u, dst);
    JARVIS_ASSERT(dst == 0);
    idle_publish_stack_low_water(0x1000u, 0x1000u, dst);
    JARVIS_ASSERT(dst == 0);
    idle_publish_stack_low_water(0x1000u, 0x1064u, dst);
    JARVIS_ASSERT(dst == 100);
    idle_publish_stack_low_water(0x1000u, 0x1000001000ULL, dst);
    JARVIS_ASSERT(dst == UINT32_MAX);
    JARVIS_TEST_PASS();
}

void register_idle_monitor_tests() {
    Logger::info("Registering idle monitor tests");
    JARVIS_REGISTER_TEST(idle_monitor_zero_init);
    JARVIS_REGISTER_TEST(idle_scan_stack_intact);
    JARVIS_REGISTER_TEST(idle_scan_stack_corrupt_hook_only);
    JARVIS_REGISTER_TEST(idle_scan_skips_idle_and_current);
    JARVIS_REGISTER_TEST(idle_scan_chunk_cursor_resumes);
    JARVIS_REGISTER_TEST(idle_stall_flag_no_kill);
    JARVIS_REGISTER_TEST(idle_publish_low_water);
}
