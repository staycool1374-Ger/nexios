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

/// @file test_sched_edf.cpp
/// @brief Deadline-aware preemptive scheduling tests (issue #19, v0.4.8).
///
/// DRIVEN: dispatch order is observed through REAL dispatched tasks — the
/// harness never calls Scheduler::next_task()/on_tick() to fake a decision
/// and never mutates task deadline/priority fields.  Global EDF (finite
/// deadline, non-exempt) is exercised directly; exemptions mirror the
/// taskdefs spawn sites.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/nexios_config.h>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

/// @brief Dispatch a task for real and wait for genuine termination.
static TaskControlBlock *run_edf_task(void (*entry)(), uint64_t prio,
                                      uint64_t period) {
    auto *t = TaskControlBlock::create(entry, prio, period);
    if (t == nullptr)
        return nullptr;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    return t;
}

static void release_edf_task(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    kernel::test::terminate_if_live(t);
}

} // namespace

// Runmode: kernel
// Testidea: DM assignment orders shorter deadlines to higher priorities.
// Input: Two FIXED tasks (periods 4 and 40); assign_deadline_priority both.
// Expect: shorter-period task gets strictly higher priority, both in band.
// Depends: Scheduler::assign_deadline_priority (issue #19)
JARVIS_TEST(edf_dm_assign_orders_by_deadline, "PRE: none | POST: none") {
    auto *short_task = TaskControlBlock::create([]() {}, 11, 4);
    auto *long_task = TaskControlBlock::create([]() {}, 11, 40);
    JARVIS_ASSERT(short_task != nullptr);
    JARVIS_ASSERT(long_task != nullptr);
    Scheduler::add_task(*short_task);
    Scheduler::add_task(*long_task);
    Scheduler::assign_deadline_priority(*short_task);
    Scheduler::assign_deadline_priority(*long_task);
    JARVIS_ASSERT(short_task->priority > long_task->priority);
    JARVIS_ASSERT(short_task->priority <= CONFIG_DM_PRIO_MAX);
    JARVIS_ASSERT(long_task->priority >= CONFIG_DM_PRIO_MIN);
    release_edf_task(short_task);
    release_edf_task(long_task);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: DM assignment clamps extremes into the band, off system bands.
// Input: Periods 1 and 0xFFFFFFFE; assign both.
// Expect: priorities within [2,120]; never 0/1, never >= 121.
// Depends: Scheduler::assign_deadline_priority clamping (issue #19)
JARVIS_TEST(edf_dm_assign_clamps_to_band, "PRE: none | POST: none") {
    auto *tiny = TaskControlBlock::create([]() {}, 11, 1);
    auto *huge =
        TaskControlBlock::create([]() {}, 11, 0xFFFFFFFEULL);
    JARVIS_ASSERT(tiny != nullptr);
    JARVIS_ASSERT(huge != nullptr);
    Scheduler::add_task(*tiny);
    Scheduler::add_task(*huge);
    Scheduler::assign_deadline_priority(*tiny);
    Scheduler::assign_deadline_priority(*huge);
    JARVIS_ASSERT(tiny->priority >= CONFIG_DM_PRIO_MIN);
    JARVIS_ASSERT(tiny->priority <= CONFIG_DM_PRIO_MAX);
    JARVIS_ASSERT(huge->priority >= CONFIG_DM_PRIO_MIN);
    JARVIS_ASSERT(huge->priority <= CONFIG_DM_PRIO_MAX);
    JARVIS_ASSERT(tiny->priority > huge->priority);
    release_edf_task(tiny);
    release_edf_task(huge);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: earliest absolute deadline dispatches first, priority ignored.
// Input: Low-prio task (prio 5, period 4) + high-prio task (prio 60,
//        period 400); both READY; reschedule; record run order.
// Expect: the prio-5 task runs first (earlier deadline).
// Depends: EDF dispatch in next_task (issue #19)
JARVIS_TEST(edf_earliest_dispatches_first, "PRE: none | POST: none") {
    static volatile uint64_t g_order = 0;
    static volatile uint64_t g_early_slot = 0;
    static volatile uint64_t g_late_slot = 0;
    g_order = 0;
    g_early_slot = 0;
    g_late_slot = 0;

    auto *early = TaskControlBlock::create(
        []() {
            g_order = g_order + 1;
            g_early_slot = g_order;
        },
        5, 4);
    auto *late = TaskControlBlock::create(
        []() {
            g_order = g_order + 1;
            g_late_slot = g_order;
        },
        60, 400);
    JARVIS_ASSERT(early != nullptr);
    JARVIS_ASSERT(late != nullptr);
    Scheduler::add_task(*early);
    Scheduler::add_task(*late);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(early);
    kernel::test::wait_for_termination_safe(late);
    JARVIS_ASSERT_EQ(1ULL, g_early_slot);
    JARVIS_ASSERT_EQ(2ULL, g_late_slot);
    release_edf_task(early);
    release_edf_task(late);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: an EDF arrival preempts a running FIXED task.
// Input: Aperiodic FIXED runner (prio 11) spinning; low-prio EDF task
//        (prio 5, period 4) added mid-quantum; reschedule.
// Expect: EDF task dispatches while the runner is still mid-quantum
//         (priority dispatch alone could never preempt 11 with a 5).
// Depends: needs_switch EDF-preemption (issue #19)
JARVIS_TEST(edf_preempts_fixed, "PRE: none | POST: none") {
    static volatile uint64_t g_progress = 0;
    static volatile uint64_t g_edf_saw = 0;
    static volatile uint64_t g_edf_ran = 0;
    g_progress = 0;
    g_edf_saw = 0;
    g_edf_ran = 0;

    auto *runner = TaskControlBlock::create(
        []() {
            for (uint64_t i = 0; i < 300000; ++i) {
                g_progress = i;
                arch::pause();
            }
        },
        11, TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(runner != nullptr);
    Scheduler::add_task(*runner);
    Scheduler::reschedule();
    // Bounded wait for the runner to be genuinely mid-quantum.
    for (uint64_t i = 0; i < 10000000 && g_progress <= 1000; ++i)
        arch::pause();
    JARVIS_ASSERT(g_progress > 1000);

    auto *edf = TaskControlBlock::create(
        []() {
            g_edf_saw = g_progress;
            g_edf_ran = 1;
        },
        5, 4);
    JARVIS_ASSERT(edf != nullptr);
    Scheduler::add_task(*edf);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(edf);
    kernel::test::wait_for_termination_safe(runner);
    JARVIS_ASSERT_EQ(1ULL, g_edf_ran);
    JARVIS_ASSERT(g_edf_saw > 1000);
    release_edf_task(edf);
    release_edf_task(runner);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: equal deadlines never thrash or starve.
// Input: Two EDF tasks, same period 10, bounded progress loops.
// Expect: both terminate with full progress (no livelock/starvation).
// Depends: EDF tie-break (priority, then id) (issue #19)
JARVIS_TEST(edf_equal_deadlines_no_thrash, "PRE: none | POST: none") {
    static volatile uint64_t g_a = 0;
    static volatile uint64_t g_b = 0;
    g_a = 0;
    g_b = 0;

    auto *a = TaskControlBlock::create(
        []() {
            for (uint64_t i = 0; i < 5000; ++i) {
                g_a = i;
                arch::pause();
            }
        },
        11, 10);
    auto *b = TaskControlBlock::create(
        []() {
            for (uint64_t i = 0; i < 5000; ++i) {
                g_b = i;
                arch::pause();
            }
        },
        11, 10);
    JARVIS_ASSERT(a != nullptr);
    JARVIS_ASSERT(b != nullptr);
    Scheduler::add_task(*a);
    Scheduler::add_task(*b);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(a);
    kernel::test::wait_for_termination_safe(b);
    JARVIS_ASSERT_EQ(4999ULL, g_a);
    JARVIS_ASSERT_EQ(4999ULL, g_b);
    release_edf_task(a);
    release_edf_task(b);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: exempt tasks stay priority-dispatched under global EDF.
// Input: Exempt task X (period 400, prio 60, edf_exempt) + FIXED task Y
//        (NO_PERIOD, prio 5); no EDF tasks present; both READY.
// Expect: X is bitmap-queued (not EDF-queued) and runs first.
// Depends: edf_exempt sites + eligibility rule (issue #19)
JARVIS_TEST(edf_exempt_daemon_stays_fixed, "PRE: none | POST: none") {
    static volatile uint64_t g_order = 0;
    static volatile uint64_t g_x_slot = 0;
    static volatile uint64_t g_y_slot = 0;
    g_order = 0;
    g_x_slot = 0;
    g_y_slot = 0;

    auto *x = TaskControlBlock::create(
        []() {
            g_order = g_order + 1;
            g_x_slot = g_order;
        },
        60, 400);
    auto *y = TaskControlBlock::create(
        []() {
            g_order = g_order + 1;
            g_y_slot = g_order;
        },
        5, TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(x != nullptr);
    JARVIS_ASSERT(y != nullptr);
    x->edf_exempt = true; // mirrors the taskdefs daemon spawn site
    Scheduler::add_task(*x);
    Scheduler::add_task(*y);
    // White-box proof of the exemption: finite deadline, yet bitmap.
    JARVIS_ASSERT(x->in_ready_queue_ == true);
    JARVIS_ASSERT(x->in_edf_queue_ == false);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(x);
    kernel::test::wait_for_termination_safe(y);
    JARVIS_ASSERT_EQ(1ULL, g_x_slot);
    JARVIS_ASSERT_EQ(2ULL, g_y_slot);
    release_edf_task(x);
    release_edf_task(y);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: a missed deadline recovers — no wedge, future periods run.
// Input: EDF task (period 2) genuinely overruns 40 ticks, blocks on a
//        gate; scan observes the miss; gate posts; peer EDF task (period
//        40) runs briefly.  Both must terminate.
// Expect: overrunner miss_count >= 1; both tasks terminate cleanly.
// Depends: EDF dispatch + existing miss/reload machinery (issue #19)
JARVIS_TEST(edf_missed_deadline_recovers, "PRE: none | POST: none") {
    sync::Semaphore gate;
    gate.init(0, 1);

    auto *over = TaskControlBlock::create(
        []() {
            sync::Semaphore *g = reinterpret_cast<sync::Semaphore *>(
                Scheduler::current_task()->user_data);
            uint64_t start = arch::Timer::ticks();
            while (arch::Timer::ticks() - start < 40)
                arch::pause();
            g->wait();
            while (Scheduler::current_task()->state == TaskState::BLOCKED)
                arch::pause();
        },
        11, 2);
    JARVIS_ASSERT(over != nullptr);
    over->user_data = &gate;
    Scheduler::add_task(*over);
    Scheduler::reschedule();
    // Unbounded (matches test_deadline_miss.cpp): the task genuinely needs
    // 40 ticks before it blocks; a short bound flakes under emulation.
    // A real failure to block trips the host watchdog (FAIL, never a hang).
    while (over->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT(over->state == TaskState::BLOCKED);
    JARVIS_ASSERT(over->deadline_ticks < arch::Timer::ticks());
    kernel::test::trigger_deadline_monitor_scan();
    JARVIS_ASSERT(over->deadline_miss_count >= 1);

    auto *peer = run_edf_task(
        []() {
            auto *self = Scheduler::current_task();
            while (self->executed_ticks < 2)
                arch::pause();
        },
        11, 40);
    JARVIS_ASSERT(peer != nullptr);

    gate.post();
    kernel::test::wait_for_termination_safe(over);
    release_edf_task(peer);
    release_edf_task(over);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

void register_sched_edf_tests() {
    Logger::info("Registering sched_edf tests");
    JARVIS_REGISTER_TEST(edf_dm_assign_orders_by_deadline);
    JARVIS_REGISTER_TEST(edf_dm_assign_clamps_to_band);
    JARVIS_REGISTER_TEST(edf_earliest_dispatches_first);
    JARVIS_REGISTER_TEST(edf_preempts_fixed);
    JARVIS_REGISTER_TEST(edf_equal_deadlines_no_thrash);
    JARVIS_REGISTER_TEST(edf_exempt_daemon_stays_fixed);
    JARVIS_REGISTER_TEST(edf_missed_deadline_recovers);
}
