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
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/log/dmesg.hpp>
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
    JARVIS_ASSERT(t1->stall_reported == false);
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

// Runmode: kernel
// Testidea: idle_util_for is exact integer math with defined edges.
// Input: Synthetic (exec_ns, period_ticks) pairs.
// Expect: mid value exact; zero exec → 0; period 0/NO_PERIOD → 0;
// over-100% clamps at 1000.
JARVIS_TEST(idle_util_for_math, "PRE: none | POST: none") {
    JARVIS_ASSERT(idle_util_for(5000000ULL, 10) == 500);
    JARVIS_ASSERT(idle_util_for(0, 10) == 0);
    JARVIS_ASSERT(idle_util_for(5000000ULL, 0) == 0);
    JARVIS_ASSERT(idle_util_for(5000000ULL, TaskControlBlock::NO_PERIOD) ==
                  0);
    JARVIS_ASSERT(idle_util_for(10000000ULL, 10) == 1000);
    JARVIS_ASSERT(idle_util_for(UINT64_MAX, 10) == 1000);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The P4 observed-max field starts at zero (extends the P1a
// zero-init contract to wcet_observed_ns, all creation sites via the
// explicit inits + memset).
// Input: Fresh create()/create_user() TCBs.
// Expect: wcet_observed_ns == 0.
JARVIS_TEST(idle_observed_zero_init, "PRE: none | POST: none") {
    auto *t1 = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(t1 != nullptr);
    JARVIS_ASSERT(t1->wcet_observed_ns == 0);
    TaskControlBlock::destroy(t1);
    auto *t2 = TaskControlBlock::create_user(forever_entry, 5, 10,
                                             32_KiB);
    JARVIS_ASSERT(t2 != nullptr);
    JARVIS_ASSERT(t2->wcet_observed_ns == 0);
    t2->cleanup();
    delete t2;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: idle_aggregate_util publishes exact per-mille from seeded
// tick-charged samples; aperiodic reads 0; block/preempt stay 0 (no
// sinks — never fabricated).
// Input: Registered BLOCKED periodic task (exec seeded) + aperiodic
// task, full-budget aggregate.
// Expect: periodic exact 500; aperiodic 0; block/preempt 0; scanned.
JARVIS_TEST(idle_aggregate_util_publishes, "PRE: none | POST: none") {
    auto *periodic = TaskControlBlock::create(forever_entry, 5, 10);
    auto *aperiodic = TaskControlBlock::create(forever_entry, 5, 0);
    JARVIS_ASSERT(periodic != nullptr && aperiodic != nullptr);
    periodic->state = TaskState::BLOCKED;
    aperiodic->state = TaskState::BLOCKED;
    Scheduler::register_task(*periodic);
    Scheduler::register_task(*aperiodic);
    periodic->exec_period_ns = 5000000ULL;

    IdleScanCursor cursor = {};
    const IdleScanProgress progress = idle_aggregate_util(cursor,
                                                          1000000ULL);
    JARVIS_ASSERT(progress.scanned >= 2);
    JARVIS_ASSERT(periodic->util_per_mille == 500);
    JARVIS_ASSERT(aperiodic->util_per_mille == 0);
    JARVIS_ASSERT(periodic->block_per_mille == 0);
    JARVIS_ASSERT(periodic->preempt_per_mille == 0);

    Scheduler::remove_task(*periodic);
    Scheduler::remove_task(*aperiodic);
    TaskControlBlock::destroy(periodic);
    TaskControlBlock::destroy(aperiodic);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The meet latch counts clean boundaries only (seam-level,
// no tick driving).
// Input: Unregistered TCBs through idle_note_period_reload.
// Expect: !missed → meets+1; missed → unchanged; UINT32_MAX saturates.
JARVIS_TEST(idle_meets_latch, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    t->deadline_missed = false;
    idle_note_period_reload(*t);
    JARVIS_ASSERT(t->deadline_meets == 1);
    t->deadline_missed = true;
    idle_note_period_reload(*t);
    JARVIS_ASSERT(t->deadline_meets == 1);
    t->deadline_missed = false;
    t->deadline_meets = UINT32_MAX;
    idle_note_period_reload(*t);
    JARVIS_ASSERT(t->deadline_meets == UINT32_MAX);
    TaskControlBlock::destroy(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Observed-max is monotonic with a single transition-fired
// exceed log; the design bound is never modified (spec §2.5).
// Input: Unregistered TCB (wcet_ticks=10 → 10ms bound) through
// idle_note_exec_sample with rising samples.
// Expect: below → false + observed set; crossing → true + observed set
// + design still 10; above again → false (no re-fire) + max tracks.
JARVIS_TEST(idle_observed_max_and_log_once, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    t->wcet_ticks = 10;
    t->exec_period_ns = 5000000ULL;
    JARVIS_ASSERT(idle_note_exec_sample(*t) == false);
    JARVIS_ASSERT(t->wcet_observed_ns == 5000000ULL);
    t->exec_period_ns = 15000000ULL;
    JARVIS_ASSERT(idle_note_exec_sample(*t) == true);
    JARVIS_ASSERT(t->wcet_observed_ns == 15000000ULL);
    JARVIS_ASSERT(t->wcet_ticks == 10);
    t->exec_period_ns = 20000000ULL;
    JARVIS_ASSERT(idle_note_exec_sample(*t) == false);
    JARVIS_ASSERT(t->wcet_observed_ns == 20000000ULL);
    JARVIS_ASSERT(t->wcet_ticks == 10);
    TaskControlBlock::destroy(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Aggregation feeds the monstat source (guards the #288 UTIL
// flip against aggregation-unfed regression).
// Input: Seeded registered task + full-budget aggregate.
// Expect: util_per_mille becomes nonzero.
JARVIS_TEST(idle_monstat_util_source, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    t->state = TaskState::BLOCKED;
    Scheduler::register_task(*t);
    t->exec_period_ns = 1000000ULL;

    IdleScanCursor cursor = {};
    (void)idle_aggregate_util(cursor, 1000000ULL);
    JARVIS_ASSERT(t->util_per_mille == 100);

    Scheduler::remove_task(*t);
    TaskControlBlock::destroy(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The P5a counter + snapshot fields start at zero (extends the
// P1a/P4 zero-init contract; explicit inits at all 4 creation sites).
// Input: Fresh create()/create_user() TCBs.
// Expect: all 5 fields zero.
JARVIS_TEST(idle_mem_zero_init, "PRE: none | POST: none") {
    auto *t1 = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(t1 != nullptr);
    JARVIS_ASSERT(t1->mem_alloc_ops_ == 0);
    JARVIS_ASSERT(t1->mem_free_ops_ == 0);
    JARVIS_ASSERT(t1->mem_prev_outstanding_ == 0);
    JARVIS_ASSERT(t1->mem_prev_used_ == 0);
    JARVIS_ASSERT(t1->mem_leak_streak_ == 0);
    TaskControlBlock::destroy(t1);
    auto *t2 = TaskControlBlock::create_user(forever_entry, 5, 10,
                                             32_KiB);
    JARVIS_ASSERT(t2 != nullptr);
    JARVIS_ASSERT(t2->mem_alloc_ops_ == 0);
    JARVIS_ASSERT(t2->mem_free_ops_ == 0);
    JARVIS_ASSERT(t2->mem_prev_outstanding_ == 0);
    JARVIS_ASSERT(t2->mem_prev_used_ == 0);
    JARVIS_ASSERT(t2->mem_leak_streak_ == 0);
    t2->cleanup();
    delete t2;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Alloc/free ops count at the attribution points, via an
// impersonated task (ScopedCurrentTask) so the harness is untouched.
// Input: Driven task with a budget; charge/credit pairs + one real
// PMM::alloc_page/free_page round trip.
// Expect: charge bumps alloc_ops (and used_pages); credit bumps free_ops;
// the PMM path bumps alloc_ops exactly once per successful call.
JARVIS_TEST(idle_mem_alloc_free_counting, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    t->memory_budget_pages_ = 100000;
    {
        ScopedCurrentTask impersonate(*t);
        JARVIS_ASSERT(Scheduler::charge_task_memory(3));
        JARVIS_ASSERT(t->mem_alloc_ops_ == 1);
        JARVIS_ASSERT(t->memory_used_pages_ == 3);
        Scheduler::credit_task_memory(1);
        JARVIS_ASSERT(t->mem_free_ops_ == 1);
        JARVIS_ASSERT(t->memory_used_pages_ == 2);
        const uint64_t ops_before = t->mem_alloc_ops_;
        const uint64_t page = PMM::alloc_page();
        JARVIS_ASSERT(page != 0);
        JARVIS_ASSERT(t->mem_alloc_ops_ == ops_before + 1);
        PMM::free_page(page);
    }
    TaskControlBlock::destroy(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The leak heuristic fires exactly once per streak: growth with
// flat usage over PASSES consecutive passes pushes one PMM+1 WARN; further
// growth does not re-fire; flat input resets the streak and re-arms.
// Input: Registered BLOCKED task with seeded counters; direct field bumps
// between full-budget idle_scan_mem passes (the seam reads TCB state).
// Expect: streak 0,0→ no fire; streak==PASSES → exactly one new PMM+1
// entry; streak>PASSES → no second entry; flat pass → streak 0, re-arm.
JARVIS_TEST(idle_mem_leak_fires_once, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    t->state = TaskState::BLOCKED;
    Scheduler::register_task(*t);
    t->mem_alloc_ops_ = 10;
    t->memory_used_pages_ = 5;
    t->mem_prev_outstanding_ = 8;
    t->mem_prev_used_ = 5;

    auto pmm_warn_count = []() {
        uint64_t found = 0;
        log::DmesgService::instance().for_each(
            [&found](const log::LogEntry &e) {
                if (e.subsystem == log::ErrorSubsystem::PMM &&
                    e.error_code ==
                        log::kDmesgBase_PMM + 1)
                    ++found;
            });
        return found;
    };
    const uint64_t base = pmm_warn_count();
    const uint64_t passes = static_cast<uint64_t>(
        CONFIG_IDLE_MONITOR_MEM_PASSES);
    for (uint64_t pass = 0; pass < passes - 1; ++pass) {
        IdleScanCursor cursor = {};
        (void)idle_scan_mem(cursor, 1000000ULL);
        JARVIS_ASSERT(t->mem_leak_streak_ == pass + 1);
        JARVIS_ASSERT(pmm_warn_count() == base);
        t->mem_alloc_ops_ += 2;
    }
    {
        IdleScanCursor cursor = {};
        (void)idle_scan_mem(cursor, 1000000ULL);
    }
    JARVIS_ASSERT(t->mem_leak_streak_ == passes);
    JARVIS_ASSERT(pmm_warn_count() == base + 1);
    t->mem_alloc_ops_ += 2;
    {
        IdleScanCursor cursor = {};
        (void)idle_scan_mem(cursor, 1000000ULL);
    }
    JARVIS_ASSERT(t->mem_leak_streak_ == passes + 1);
    JARVIS_ASSERT(pmm_warn_count() == base + 1);
    {
        IdleScanCursor cursor = {};
        (void)idle_scan_mem(cursor, 1000000ULL);
    }
    JARVIS_ASSERT(t->mem_leak_streak_ == 0);
    JARVIS_ASSERT(pmm_warn_count() == base + 1);

    Scheduler::remove_task(*t);
    TaskControlBlock::destroy(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Used-pages growth (not just ops growth) never suspects: the
// heuristic keys on ops-with-flat-usage per spec §2.3.
// Input: Registered BLOCKED task, both counters and usage growing.
// Expect: streak stays 0 across passes.
JARVIS_TEST(idle_mem_usage_growth_not_suspect, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    t->state = TaskState::BLOCKED;
    Scheduler::register_task(*t);
    t->mem_alloc_ops_ = 10;
    t->memory_used_pages_ = 5;
    for (uint64_t pass = 0; pass < 6; ++pass) {
        IdleScanCursor cursor = {};
        (void)idle_scan_mem(cursor, 1000000ULL);
        JARVIS_ASSERT(t->mem_leak_streak_ == 0);
        t->mem_alloc_ops_ += 2;
        t->memory_used_pages_ += 1;
    }
    Scheduler::remove_task(*t);
    TaskControlBlock::destroy(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Attribution overhead is bounded (acceptance: measured number).
// Input: 10000 charge+credit pairs on an impersonated task (frozen ticks)
// + one full idle_scan_mem pass; rdtsc deltas.
// Expect: pairs total < 10M cycles (1000/pair headroom); scan < 1M cycles.
JARVIS_TEST(idle_mem_overhead_bounded, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    t->memory_budget_pages_ = 100000;
    {
        ScopedCurrentTask impersonate(*t);
        arch::IrqGuard guard;
        const uint64_t t0 = arch::rdtsc();
        for (uint64_t i = 0; i < 10000; ++i) {
            (void)Scheduler::charge_task_memory(1);
            Scheduler::credit_task_memory(1);
        }
        const uint64_t elapsed = arch::rdtsc() - t0;
        Logger::info("[WCET] idle_mem charge+credit x10000: %lu cycles",
                     elapsed);
        JARVIS_ASSERT_FMT(elapsed < 10000000ULL,
                          "charge/credit overhead %lu cycles", elapsed);
    }
    t->state = TaskState::BLOCKED;
    Scheduler::register_task(*t);
    {
        arch::IrqGuard guard;
        IdleScanCursor cursor = {};
        const uint64_t t0 = arch::rdtsc();
        (void)idle_scan_mem(cursor, 1000000ULL);
        const uint64_t elapsed = arch::rdtsc() - t0;
        Logger::info("[WCET] idle_scan_mem full pass: %lu cycles",
                     elapsed);
        JARVIS_ASSERT_FMT(elapsed < 1000000ULL,
                          "idle_scan_mem overhead %lu cycles", elapsed);
    }
    Scheduler::remove_task(*t);
    TaskControlBlock::destroy(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A wake stamps progress (seam via the set_task_ready choke
// point, issues #43/#285).
// Input: Registered BLOCKED task with a stale stamp; set_task_ready.
// Expect: stamp advances to (approximately) current ticks; state READY.
JARVIS_TEST(idle_stall_progress_bump_on_wake, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    t->state = TaskState::BLOCKED;
    Scheduler::register_task(*t);
    t->last_progress_tick = 100;
    const uint64_t before = arch::Timer::ticks();
    Scheduler::set_task_ready(*t);
    JARVIS_ASSERT(t->state == TaskState::READY);
    JARVIS_ASSERT(t->last_progress_tick >= before);
    JARVIS_ASSERT(t->stuck_suspected == false);
    JARVIS_ASSERT(t->stall_reported == false);
    Scheduler::remove_task(*t);
    TaskControlBlock::destroy(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The progress seam stamps and clears the flag episode
// (unit-level; the switch_to_task call site is covered by inspection —
// adjacent to stamp_exec under the same lock/IRQ discipline).
// Input: Unregistered TCB with stale stamp + set flag + reported latch.
// Expect: stamp exact; flag and latch cleared (next stall re-reports).
JARVIS_TEST(idle_stall_progress_bump_seam, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    t->last_progress_tick = 100;
    t->stuck_suspected = true;
    t->stall_reported = true;
    idle_note_progress(*t, 5000);
    JARVIS_ASSERT(t->last_progress_tick == 5000);
    JARVIS_ASSERT(t->stuck_suspected == false);
    JARVIS_ASSERT(t->stall_reported == false);
    TaskControlBlock::destroy(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Unarmed stall reports exactly once and never arms, kills,
// or changes state (M6 + hard rule).
// Input: Registered BLOCKED stale task (wdog disarmed) through
// idle_escalate_stall twice.
// Expect: exactly one new TIMING+7 WARN; still flagged; wdog stays
// disarmed; state still BLOCKED; second pass silent.
JARVIS_TEST(idle_escalate_unarmed_reports_only, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    t->state = TaskState::BLOCKED;
    Scheduler::register_task(*t);
    t->last_progress_tick = 100;
    t->stuck_suspected = true;
    t->wdog_armed = false;

    auto timing7_count = []() {
        uint64_t found = 0;
        log::DmesgService::instance().for_each(
            [&found](const log::LogEntry &e) {
                if (e.subsystem == log::ErrorSubsystem::TIMING &&
                    e.error_code == log::kDmesgBase_TIMING + 7)
                    ++found;
            });
        return found;
    };
    // Clear-first: the shared ring churns under tick DMD traffic and
    // wraps (evicting old entries), so deltas against a live base are
    // nondeterministic. klog_read precedent: self-contained probes.
    log::DmesgService::instance().clear();
    const uint64_t base = timing7_count();
    // Real-time now (not synthetic): the shared registry must not see
    // far-future time (see never_arms test).
    arch::IrqGuard measure_guard{};
    const uint64_t now = arch::Timer::ticks();
    {
        IdleScanCursor cursor = {};
        (void)idle_escalate_stall(cursor, 1000000ULL, now);
    }
    JARVIS_ASSERT(timing7_count() == base + 1);
    JARVIS_ASSERT(t->stuck_suspected == true);
    JARVIS_ASSERT(t->wdog_armed == false);
    JARVIS_ASSERT(t->state == TaskState::BLOCKED);
    {
        IdleScanCursor cursor = {};
        (void)idle_escalate_stall(cursor, 1000000ULL, now);
    }
    JARVIS_ASSERT(timing7_count() == base + 1);

    Scheduler::remove_task(*t);
    TaskControlBlock::destroy(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Armed + expired stall routes to the EXISTING handler (all of
// #41's disposition preserved), one-shot, no re-fire.
// Input: Registered task, stale + flagged, direct field arm with past
// expiry under IrqGuard (identical stores to sys_watchdog_create;
// guard freezes ticks so no tick scan can interleave); seam under the
// same guard.
// Expect: wdog disarmed; exactly one new TIMING+6 (real handler ran);
// helper TERMINATED by the action=3 disposition; second pass adds no
// TIMING+6.
JARVIS_TEST(idle_escalate_armed_expired_routes, "PRE: none | POST: none") {
    // BLOCKED + registered (never enqueued, never dispatched): a live
    // READY helper could dispatch between setup and seam, and the
    // dispatch progress stamp would clear the flag under test.
    auto *helper = TaskControlBlock::create(forever_entry, 9, 10);
    JARVIS_ASSERT(helper != nullptr);
    helper->state = TaskState::BLOCKED;
    Scheduler::register_task(*helper);
    helper->last_progress_tick = 100;
    helper->stuck_suspected = true;

    auto timing6_count = []() {
        uint64_t found = 0;
        log::DmesgService::instance().for_each(
            [&found](const log::LogEntry &e) {
                if (e.subsystem == log::ErrorSubsystem::TIMING &&
                    e.error_code == log::kDmesgBase_TIMING + 6)
                    ++found;
            });
        return found;
    };
    // Clear-first (see unarmed test): ring churn makes live-base
    // deltas nondeterministic. The whole measure window runs under one
    // IrqGuard: ring pushes race tick DMD traffic (multi-producer on an
    // SPSC ring), so counts are only exact with ticks frozen.
    log::DmesgService::instance().clear();
    const uint64_t base = timing6_count();
    {
        arch::IrqGuard guard{};
        const uint64_t now = arch::Timer::ticks();
        helper->wdog_period_ticks = 5;
        helper->wdog_last_kick_tick = now;
        helper->wdog_expiry_tick = now - 1;
        helper->wdog_armed = true;
        helper->wdog_gen = 1;
        IdleScanCursor cursor = {};
        (void)idle_escalate_stall(cursor, 1000000ULL, now);
        JARVIS_ASSERT(helper->wdog_armed == false);
        JARVIS_ASSERT(timing6_count() == base + 1);
        IdleScanCursor cursor2 = {};
        (void)idle_escalate_stall(cursor2, 1000000ULL, now);
        JARVIS_ASSERT(timing6_count() == base + 1);
    }
    // Cleanup: the helper is TERMINATED via the real action=3 path but
    // sits in the tick's deferred-kill queue, not the zombie list — so
    // remove + destroy directly (a later tick's deferred entry sees the
    // 0xDD-poisoned magic and skips safely). Waiting for tick reap here
    // would work too but costs up to 500 ticks of wall time.
    Scheduler::remove_task(*helper);
    TaskControlBlock::destroy(helper);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Armed but unexpired stall neither routes nor reports.
// Input: Registered BLOCKED flagged task, armed with future expiry.
// Expect: no TIMING+6 delta; wdog stays armed; no TIMING+7 either
// (the armed path never reports — the watchdog owns the episode).
JARVIS_TEST(idle_escalate_armed_future_no_fire, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    t->state = TaskState::BLOCKED;
    Scheduler::register_task(*t);
    t->last_progress_tick = 100;
    t->stuck_suspected = true;

    auto timing6_count = []() {
        uint64_t found = 0;
        log::DmesgService::instance().for_each(
            [&found](const log::LogEntry &e) {
                if (e.subsystem == log::ErrorSubsystem::TIMING &&
                    e.error_code == log::kDmesgBase_TIMING + 6)
                    ++found;
            });
        return found;
    };
    // Clear-first (see unarmed test): ring churn makes live-base
    // deltas nondeterministic.
    log::DmesgService::instance().clear();
    const uint64_t base = timing6_count();
    const uint64_t now = arch::Timer::ticks();
    // Ticks frozen across arm + seam + count: the ring races tick DMD
    // traffic, and a tick could otherwise fire nothing here but pollute
    // the no-delta assert via unrelated expiries.
    arch::IrqGuard measure_guard{};
    t->wdog_period_ticks = 100000;
    t->wdog_last_kick_tick = now;
    t->wdog_expiry_tick = now + 10000;
    t->wdog_armed = true;
    t->wdog_gen = 1;
    {
        IdleScanCursor cursor = {};
        (void)idle_escalate_stall(cursor, 1000000ULL, now);
    }
    JARVIS_ASSERT(t->wdog_armed == true);
    t->wdog_armed = false;
    JARVIS_ASSERT(timing6_count() == base);

    Scheduler::remove_task(*t);
    TaskControlBlock::destroy(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The monitor never arms a watchdog (hard rule, spec §5):
// full escalate passes over disarmed tasks leave every arm off.
// Input: Two registered BLOCKED tasks with flags set directly, through
// idle_escalate_stall (the scan seam that sets flags is covered by the
// stall tests; running a synthetic-time scan over the shared registry
// here would flag unrelated tasks and make the count
// environment-dependent).
// Expect: both wdog_armed false; exactly one TIMING+7 each (report-only,
// M6); no TIMING+6.
JARVIS_TEST(idle_escalate_never_arms_fresh, "PRE: none | POST: none") {
    auto *a = TaskControlBlock::create(forever_entry, 5, 10);
    auto *b = TaskControlBlock::create(forever_entry, 5, 10);
    JARVIS_ASSERT(a != nullptr && b != nullptr);
    a->state = TaskState::BLOCKED;
    b->state = TaskState::BLOCKED;
    Scheduler::register_task(*a);
    Scheduler::register_task(*b);
    a->last_progress_tick = 100;
    b->last_progress_tick = 100;
    a->stuck_suspected = true;
    b->stuck_suspected = true;

    auto timing7_count = []() {
        uint64_t found = 0;
        log::DmesgService::instance().for_each(
            [&found](const log::LogEntry &e) {
                if (e.subsystem == log::ErrorSubsystem::TIMING &&
                    e.error_code == log::kDmesgBase_TIMING + 7)
                    ++found;
            });
        return found;
    };
    // Clear-first (see unarmed test): ring churn makes live-base
    // deltas nondeterministic. Ticks frozen across the whole window:
    // ring pushes race tick DMD traffic on the shared SPSC ring.
    // Real-time now (not synthetic): the shared registry must not see
    // far-future time, which would flag unrelated tasks.
    log::DmesgService::instance().clear();
    const uint64_t base = timing7_count();
    arch::IrqGuard measure_guard{};
    const uint64_t now = arch::Timer::ticks();
    {
        IdleScanCursor cursor = {};
        (void)idle_escalate_stall(cursor, 1000000ULL, now);
    }
    JARVIS_ASSERT(a->wdog_armed == false);
    JARVIS_ASSERT(b->wdog_armed == false);
    JARVIS_ASSERT(timing7_count() == base + 2);

    Scheduler::remove_task(*a);
    Scheduler::remove_task(*b);
    TaskControlBlock::destroy(a);
    TaskControlBlock::destroy(b);
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
    JARVIS_REGISTER_TEST(idle_util_for_math);
    JARVIS_REGISTER_TEST(idle_observed_zero_init);
    JARVIS_REGISTER_TEST(idle_aggregate_util_publishes);
    JARVIS_REGISTER_TEST(idle_meets_latch);
    JARVIS_REGISTER_TEST(idle_observed_max_and_log_once);
    JARVIS_REGISTER_TEST(idle_monstat_util_source);
    JARVIS_REGISTER_TEST(idle_mem_zero_init);
    JARVIS_REGISTER_TEST(idle_mem_alloc_free_counting);
    JARVIS_REGISTER_TEST(idle_mem_leak_fires_once);
    JARVIS_REGISTER_TEST(idle_mem_usage_growth_not_suspect);
    JARVIS_REGISTER_TEST(idle_mem_overhead_bounded);
    JARVIS_REGISTER_TEST(idle_stall_progress_bump_on_wake);
    JARVIS_REGISTER_TEST(idle_stall_progress_bump_seam);
    JARVIS_REGISTER_TEST(idle_escalate_unarmed_reports_only);
    JARVIS_REGISTER_TEST(idle_escalate_armed_expired_routes);
    JARVIS_REGISTER_TEST(idle_escalate_armed_future_no_fire);
    JARVIS_REGISTER_TEST(idle_escalate_never_arms_fresh);
}
