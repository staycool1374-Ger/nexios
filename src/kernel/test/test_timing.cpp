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

/// @file test_timing.cpp
/// @brief Timing measurement tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): accounting, alarm, reaper and
/// deadline-miss behaviour are verified through REAL kernel tasks (prio ≥ 11)
/// that genuinely run on real timer ticks.  The tests never call
/// Scheduler::on_tick()/scan_deadlines() to fake time, and never mutate
/// task->executed_ticks/remaining_ticks/alarm_ticks/deadline_ticks — the
/// state under test is reached through real execution.

#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <kernel/daemon/daemon_mgr.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/syscall/syscall.hpp>
#include <signal.hpp>

using namespace kernel;

// ---------------------------------------------------------------------------
// Shared driven helpers
// ---------------------------------------------------------------------------

/// @brief  Create a real kernel task (prio ≥ 11) whose lambda runs
///         `busy_ticks` of real time, then return.  Waits for genuine
///         dispatch + termination.  Returns the TCB for cleanup.
static TaskControlBlock *run_real_task(void (*entry)(), uint64_t prio = 11,
                                       uint64_t period = 10) {
    auto *t = TaskControlBlock::create(entry, prio, period);
    if (t == nullptr)
        return nullptr;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    return t;
}

/// @brief  Release a completed task TCB (mirrors test_ipc_blocking.cpp).
static void release_task(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    kernel::test::terminate_if_live(t);
}

// Runmode: kernel
// Testidea: The real timer ISR (on_tick) increments the RUNNING task's
// executed_ticks by 1 per real tick.  A dispatched task observes its own
// executed_ticks growing through real execution.
// Input: Kernel task (prio 11) busy-waits until its own executed_ticks
//        reaches >= 2 (real on_tick accounting), then records the value.
// Expect: recorded executed_ticks >= 2.
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(timer_tick_accounting, "PRE: none | POST: none") {
    static volatile uint64_t g_ticks_seen = 0;

    auto *t = run_real_task([]() {
        auto *self = Scheduler::current_task();
        while (self->executed_ticks < 2)
            arch::pause();
        g_ticks_seen = self->executed_ticks;
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(g_ticks_seen >= 2);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: When remaining_ticks reaches 0, the real on_tick reloads it to
// period_ticks.  A periodic task genuinely runs across a period boundary and
// observes its remaining_ticks wrap back up (reload).
// Input: Kernel task (prio 11, period 5) busy-waits ~10 real ticks while
//        polling its own remaining_ticks for the reload event.
// Expect: g_period_reloaded == true (remaining_ticks jumped up after 0).
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(timer_period_reload, "PRE: none | POST: none") {
    static volatile bool g_period_reloaded = false;

    auto *t = run_real_task([]() {
        auto *self = Scheduler::current_task();
        uint64_t prev = self->remaining_ticks;
        // Wait ~2.5 real periods (period=5 ticks) so remaining_ticks reaches 0
        // and on_tick reloads it to period_ticks — a genuine observed reload.
        uint64_t start = arch::Timer::ticks();
        while (arch::Timer::ticks() - start < 13 && !g_period_reloaded) {
            uint64_t cur = self->remaining_ticks;
            if (cur > prev)
                g_period_reloaded = true; // reloaded from 0 back to period
            prev = cur;
            arch::pause();
        }
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(g_period_reloaded);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A real alarm armed via the syscall fires after the requested
// real tick count: the real on_tick decrements alarm_ticks and raises
// SIGALRM.  A kernel task arms an alarm (3 ticks) and busy-waits until the
// signal is pending.
// Input: Kernel task (prio 11) calls Syscall::handle(ALARM, 0, 3000) then
//        polls its own pending_signals for SIGALRM.
// Expect: alarm arrives (pending SIGALRM); alarm_armed cleared.
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock,
// kernel::signal
JARVIS_TEST(timer_alarm_delivery, "PRE: none | POST: none") {
    static volatile bool g_alarm_fired = false;

    auto *t = run_real_task([]() {
        auto *self = Scheduler::current_task();
        uint64_t ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::ALARM), 0, 3000, 0, 0,
            nullptr);
        if (ret != 0)
            return;
        while (!(self->pending_signals &
                 (1ULL << static_cast<uint64_t>(Signal::SIGALRM))))
            arch::pause();
        g_alarm_fired = true;
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(g_alarm_fired);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Before the alarm expires the signal is NOT pending and the alarm
// stays armed.  A kernel task arms an alarm (100 ticks) and polls for only a
// few real ticks (well short of the deadline).
// Input: Kernel task (prio 11) calls Syscall::handle(ALARM, 0, 100000) then
//        busy-waits ~5 real ticks; asserts alarm still armed, no SIGALRM.
// Expect: alarm_armed stays true; pending SIGALRM stays clear.
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(timer_alarm_not_expired, "PRE: none | POST: none") {
    static volatile uint64_t g_still_armed = 0;
    static volatile uint64_t g_alarm_pending = 0;

    auto *t = run_real_task([]() {
        auto *self = Scheduler::current_task();
        uint64_t ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::ALARM), 0, 100000, 0, 0,
            nullptr);
        if (ret != 0)
            return;
        uint64_t start = arch::Timer::ticks();
        while (arch::Timer::ticks() - start < 5)
            arch::pause();
        g_still_armed = self->alarm_armed ? 1 : 0;
        g_alarm_pending =
            (self->pending_signals &
             (1ULL << static_cast<uint64_t>(Signal::SIGALRM)))
                ? 1
                : 0;
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(1ULL, g_still_armed);
    JARVIS_ASSERT_EQ(0ULL, g_alarm_pending);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The real timer ISR's rate_monotonic_schedule dispatches a
// higher-priority task when one is READY.  A prio-11 task genuinely runs on a
// real tick and records its execution.
// Input: Kernel task (prio 11) sets a global flag; harness dispatches it.
// Expect: g_high_ran == true (the higher-priority task was dispatched by the
// real RMS path).
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(timer_rate_monotonic_schedule_indirect, "PRE: none | POST: none") {
    static volatile bool g_high_ran = false;

    auto *t = run_real_task([]() { g_high_ran = true; });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(g_high_ran);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: An orphaned TERMINATED child is collected by the real reaper
// path (drain_zombie_list) after genuine self-termination.
// Input: Kernel task (prio 11) whose lambda exits immediately — the
//        trampoline genuinely terminates it; the harness drains zombies.
// Expect: find_task(id) == nullptr after the real reap; no leak.
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(timer_reap_orphans_periodic, "PRE: none | POST: none") {
    auto *child = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(child != nullptr);
    uint64_t child_id = child->id;
    Scheduler::add_task(*child);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(child);
    // Real reaper path: drain the zombie list (what on_tick's periodic
    // reap_orphans does; it is suppressed during tests, so we invoke the
    // same API the reaper uses).
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(Scheduler::find_task(child_id) == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Real ticks with only the idle task eligible must not corrupt
// scheduler state.  A real task runs to completion; the scheduler's
// corruption counter must not advance.
// Input: Kernel task (prio 11) runs a short busy-wait then terminates.
// Expect: scheduler_corruption_count unchanged; current task valid.
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(timer_no_side_effects_on_idle, "PRE: none | POST: none") {
    uint64_t corruption_before = kernel::scheduler_corruption_count;

    auto *t = run_real_task([]() {
        for (uint64_t i = 0; i < 1000; ++i)
            arch::pause();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(Scheduler::current_task() != nullptr);
    JARVIS_ASSERT_EQ(corruption_before, kernel::scheduler_corruption_count);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A live daemon task (vfsd/iocd, RUNNING or BLOCKED in its normal
// loop) is NOT restarted by real ticks passing.
// Input: Find a live non-idle daemon; let a few real ticks elapse.
// Expect: The daemon task still exists and is not TERMINATED.
// Depends: kernel::task::Scheduler, kernel::daemon::DaemonMgr
JARVIS_TEST(timer_daemon_restart_not_triggered_on_active,
            "PRE: none | POST: none") {
    TaskControlBlock *daemon_task = nullptr;
    for (uint64_t i = 0; i < daemon::MAX_DAEMONS; ++i) {
        const auto &entry = daemon::get_entry(i);
        if (entry.pid == 0)
            continue;
        auto *dt = Scheduler::find_task(entry.pid);
        if (dt && dt->magic == TaskControlBlock::TCB_MAGIC &&
            dt->state != TaskState::TERMINATED) {
            daemon_task = dt;
            break;
        }
    }
    JARVIS_ASSERT(daemon_task != nullptr);
    uint64_t pid = daemon_task->id;

    // Let real ticks elapse — the daemon must survive, not be restarted.
    uint64_t start = arch::Timer::ticks();
    while (arch::Timer::ticks() - start < 20)
        arch::pause();

    auto *still = Scheduler::find_task(pid);
    JARVIS_ASSERT(still != nullptr);
    JARVIS_ASSERT(still->state != TaskState::TERMINATED);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A real task that genuinely overruns its deadline (busy-waits
// past its 2-tick period in real time) is detected by the deadline scan.
// Input: Kernel task (prio 11, period 2) dispatched for real; harness waits
//        for genuine block on a semaphore, then runs scan_deadlines().
// Expect: deadline_miss_count >= 1.
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock,
// CONFIG_DEADLINE_MISS_DETECTION
JARVIS_TEST(timer_deadline_miss_detection_fires, "PRE: none | POST: none") {
#if !CONFIG_DEADLINE_MISS_DETECTION
    JARVIS_TEST_PASS();
    return;
#endif
    sync::Semaphore gate;
    gate.init(0, 1);

    auto *helper = TaskControlBlock::create(
        []() {
            sync::Semaphore *g = reinterpret_cast<sync::Semaphore *>(
                Scheduler::current_task()->user_data);
            uint64_t start = arch::Timer::ticks();
            while (arch::Timer::ticks() - start < 40)
                arch::pause();
            g->wait();
            // INV-4: Semaphore::wait() sets BLOCKED then returns immediately
            // (the switch is deferred).  Spin on our own BLOCKED state so the
            // harness can observe it before we would self-terminate; the
            // harness's gate.post() wakes us (state != BLOCKED) and we return.
            while (Scheduler::current_task()->state == TaskState::BLOCKED)
                arch::pause();
        },
        11, 2);
    JARVIS_ASSERT(helper != nullptr);
    helper->user_data = &gate;
    Scheduler::add_task(*helper);
    Scheduler::reschedule();
    while (helper->state != TaskState::BLOCKED)
        arch::pause();

    // Genuine overrun: the real deadline (now+2 at create) is long past.
    JARVIS_ASSERT(helper->deadline_ticks < arch::Timer::ticks());
    kernel::test::trigger_deadline_monitor_scan();
    // deadline_missed is reset to false by re-arm; the persistent count is
    // the stable check.
    JARVIS_ASSERT(helper->deadline_miss_count >= 1);

    gate.post();
    kernel::test::wait_for_termination_safe(helper);
    release_task(helper);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A task whose deadline is genuinely far in the future is NOT
// detected as missed.  A kernel task (prio 11, period 10000) blocks early
// (deadline far future) and the scan runs.
// Input: Kernel task (prio 11, period 10000) blocks on a real semaphore.
//        scan_deadlines() runs while its deadline is far in the future.
// Expect: deadline_miss_count == 0.
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock,
// CONFIG_DEADLINE_MISS_DETECTION
JARVIS_TEST(timer_deadline_miss_skips_future, "PRE: none | POST: none") {
#if !CONFIG_DEADLINE_MISS_DETECTION
    JARVIS_TEST_PASS();
    return;
#endif
    sync::Semaphore gate;
    gate.init(0, 1);

    auto *helper = TaskControlBlock::create(
        []() {
            sync::Semaphore *g = reinterpret_cast<sync::Semaphore *>(
                Scheduler::current_task()->user_data);
            g->wait();
            // INV-4: Semaphore::wait() sets BLOCKED then returns immediately
            // (deferred switch).  Spin on BLOCKED so the harness observes it
            // before we self-terminate; gate.post() wakes us and we return.
            while (Scheduler::current_task()->state == TaskState::BLOCKED)
                arch::pause();
        },
        11, 10000);
    JARVIS_ASSERT(helper != nullptr);
    helper->user_data = &gate;
    Scheduler::add_task(*helper);
    Scheduler::reschedule();
    while (helper->state != TaskState::BLOCKED)
        arch::pause();

    // Real future deadline: 10000 ticks from creation — not yet passed.
    JARVIS_ASSERT(helper->deadline_ticks > arch::Timer::ticks());
    kernel::test::trigger_deadline_monitor_scan();

    JARVIS_ASSERT(helper->deadline_miss_count == 0);

    gate.post();
    kernel::test::wait_for_termination_safe(helper);
    release_task(helper);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The deadline scan fires only once per deadline period: after a
// genuine overrun is detected and the deadline re-armed (advanced by
// period_ticks), a second scan does not re-fire it.
// Input: Kernel task (prio 11, period 100) blocks immediately; its real
//        deadline (create+100) passes in real time.  scan_deadlines() runs
//        twice — the re-arm (deadline += 100) puts the deadline in the future
//        before the second scan.
// Expect: deadline_miss_count == 1 (only the first scan fires).
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock,
// CONFIG_DEADLINE_MISS_DETECTION
JARVIS_TEST(timer_deadline_miss_only_once, "PRE: none | POST: none") {
#if !CONFIG_DEADLINE_MISS_DETECTION
    JARVIS_TEST_PASS();
    return;
#endif
    sync::Semaphore gate;
    gate.init(0, 1);

    auto *helper = TaskControlBlock::create(
        []() {
            sync::Semaphore *g = reinterpret_cast<sync::Semaphore *>(
                Scheduler::current_task()->user_data);
            g->wait();
            // INV-4: Semaphore::wait() sets BLOCKED then returns immediately
            // (deferred switch).  Spin on BLOCKED so the harness observes it
            // before we self-terminate; gate.post() wakes us and we return.
            while (Scheduler::current_task()->state == TaskState::BLOCKED)
                arch::pause();
        },
        11, 100);
    JARVIS_ASSERT(helper != nullptr);
    helper->user_data = &gate;
    Scheduler::add_task(*helper);
    Scheduler::reschedule();
    while (helper->state != TaskState::BLOCKED)
        arch::pause();

    // Genuinely wait until the real deadline (create+100) has passed.
    while (helper->deadline_ticks >= arch::Timer::ticks())
        arch::pause();

    kernel::test::trigger_deadline_monitor_scan();
    JARVIS_ASSERT(helper->deadline_miss_count == 1);

    // Second scan: the re-arm (deadline += 100) put the deadline in the
    // future — no second event for the same overrun window.
    kernel::test::trigger_deadline_monitor_scan();
    JARVIS_ASSERT(helper->deadline_miss_count == 1);

    gate.post();
    kernel::test::wait_for_termination_safe(helper);
    release_task(helper);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A task with period_ticks == 0 (aperiodic, no deadline tracking)
// is skipped by the deadline scan — no miss is reported.
// Input: Kernel task (prio 11, period 0) dispatched for real; its deadline
//        is never tracked.  scan_deadlines() runs.
// Expect: deadline_miss_count stays 0.
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock,
// CONFIG_DEADLINE_MISS_DETECTION
JARVIS_TEST(timer_deadline_miss_skips_zero, "PRE: none | POST: none") {
#if !CONFIG_DEADLINE_MISS_DETECTION
    JARVIS_TEST_PASS();
    return;
#endif
    sync::Semaphore gate;
    gate.init(0, 1);

    auto *helper = TaskControlBlock::create(
        []() {
            sync::Semaphore *g = reinterpret_cast<sync::Semaphore *>(
                Scheduler::current_task()->user_data);
            uint64_t start = arch::Timer::ticks();
            while (arch::Timer::ticks() - start < 20)
                arch::pause();
            g->wait();
            // INV-4: Semaphore::wait() sets BLOCKED then returns immediately
            // (deferred switch).  Spin on BLOCKED so the harness observes it
            // before we self-terminate; gate.post() wakes us and we return.
            while (Scheduler::current_task()->state == TaskState::BLOCKED)
                arch::pause();
        },
        11, 0);
    JARVIS_ASSERT(helper != nullptr);
    helper->user_data = &gate;
    Scheduler::add_task(*helper);
    Scheduler::reschedule();
    while (helper->state != TaskState::BLOCKED)
        arch::pause();

    kernel::test::trigger_deadline_monitor_scan();

    // period == 0 ⇒ the scan's "not a periodic task" guard skips it.
    JARVIS_ASSERT(helper->deadline_miss_count == 0);

    gate.post();
    kernel::test::wait_for_termination_safe(helper);
    release_task(helper);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

/// @brief Append a freshly created, add_task'd (parked + BLOCKED) task with
///        the given absolute deadline to a DeadlineList. Returns the task (or
///        nullptr on alloc/add failure) so the caller can clean it up. The
///        task is parked out of the ready queue and forced BLOCKED so the
///        scheduler never dispatches it (mirrors the existing timing tests'
///        lifecycle). period_ticks is forced to 0 so the task is NOT inserted
///        into the scheduler's own deadline_list_ (keeping the monitor from
///        scanning it) while still exercising the full create/add/cleanup
///        lifecycle for a balanced ResourceTracker. IRQs are disabled for the
///        whole setup so no timer tick can dispatch the task in the window
///        between add_task and the BLOCKED transition.
static TaskControlBlock *dl_make(DeadlineList &dl, uint64_t deadline) {
    arch::IrqGuard guard;
    if (Scheduler::task_count() >= 58)
        return nullptr; // headroom below MAX_TASKS
    auto *t = TaskControlBlock::create([]() {}, 10, 10);
    if (t == nullptr)
        return nullptr;
    t->base_priority = 10;
    t->priority = 10;
    t->period_ticks = 0; // keep out of scheduler's deadline_list_
    t->deadline_ticks = deadline;
    Scheduler::add_task(*t);
    Scheduler::dequeue_ready(*t); // park so it is not dispatched
    {
        kernel::test::ScopedCurrentTask scope(*t);
        t->state = TaskState::BLOCKED;
    }
    dl.insert(*t);
    return t;
}

/// @brief Teardown for a dl_make task — mirrors the existing timing tests
///        (cleanup() unregisters from the scheduler; no set_task_ready, so the
///        BLOCKED task is never dispatched out from under this teardown).
static void dl_free(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    t->cleanup();
    delete t;
}

// Runmode: kernel
// Testidea: Insert tasks with shuffled future deadlines; the list must stay
// sorted ascending and expose the earliest via peek_earliest().
// Expect: size == 5, peek_earliest() == the minimum-deadline task.
JARVIS_TEST(deadline_list_sorted_insert, "PRE: none | POST: none") {
    DeadlineList dl;
    uint64_t base = arch::Timer::ticks();
    TaskControlBlock *tasks[5];
    tasks[0] = dl_make(dl, base + 50);
    tasks[1] = dl_make(dl, base + 10);
    tasks[2] = dl_make(dl, base + 30);
    tasks[3] = dl_make(dl, base + 20);
    tasks[4] = dl_make(dl, base + 40);
    JARVIS_ASSERT(tasks[0] && tasks[1] && tasks[2] && tasks[3] && tasks[4]);
    JARVIS_ASSERT(dl.size() == 5);
    JARVIS_ASSERT(dl.peek_earliest() == tasks[1]); // base + 10 is earliest
    for (auto *t : tasks)
        dl_free(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: pop_earliest_if_expired only returns a task whose deadline is
// strictly in the past; future tasks yield nullptr. IRQs are disabled so the
// clock cannot advance between arming and popping.
// Expect: expired task popped (size 0); future-only list pops nullptr.
JARVIS_TEST(deadline_list_pop_expired, "PRE: none | POST: none") {
    DeadlineList dl;
    {
        arch::IrqGuard guard;
        uint64_t now = arch::Timer::ticks();
        auto *expired = dl_make(dl, now - 1);
        JARVIS_ASSERT(expired != nullptr);
        JARVIS_ASSERT(dl.pop_earliest_if_expired() == expired);
        JARVIS_ASSERT(dl.empty());
        auto *future = dl_make(dl, now + 1000);
        JARVIS_ASSERT(future != nullptr);
        JARVIS_ASSERT(dl.pop_earliest_if_expired() == nullptr);
        JARVIS_ASSERT(!dl.empty());
        dl_free(future);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: remove() deletes an arbitrary (middle) node preserving order.
// Expect: after removing the middle task, size drops by 1 and the new
//         earliest is the next-smallest.
JARVIS_TEST(deadline_list_remove_mid, "PRE: none | POST: none") {
    DeadlineList dl;
    uint64_t base = arch::Timer::ticks();
    auto *a = dl_make(dl, base + 10);
    auto *b = dl_make(dl, base + 20);
    auto *c = dl_make(dl, base + 30);
    JARVIS_ASSERT(a && b && c);
    dl.remove(*b); // remove middle
    JARVIS_ASSERT(dl.size() == 2);
    JARVIS_ASSERT(dl.peek_earliest() == a);
    dl.remove(*a);
    dl.remove(*c);
    JARVIS_ASSERT(dl.empty());
    dl_free(a);
    dl_free(b);
    dl_free(c);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: remove() on a non-member is a no-op.
// Expect: size unchanged, list intact.
JARVIS_TEST(deadline_list_remove_absent, "PRE: none | POST: none") {
    DeadlineList dl;
    uint64_t base = arch::Timer::ticks();
    auto *a = dl_make(dl, base + 10);
    auto *b = dl_make(dl, base + 20);
    JARVIS_ASSERT(a && b);

    // A task that is NEVER inserted into the list (created + parked exactly
    // like dl_make, minus the dl.insert()).  Removing it must be a no-op.
    arch::IrqGuard guard;
    auto *ghost = TaskControlBlock::create([]() {}, 10, 10);
    JARVIS_ASSERT(ghost != nullptr);
    ghost->base_priority = 10;
    ghost->priority = 10;
    ghost->period_ticks = 0;
    ghost->deadline_ticks = base + 999;
    Scheduler::add_task(*ghost);
    Scheduler::dequeue_ready(*ghost);
    {
        kernel::test::ScopedCurrentTask scope(*ghost);
        ghost->state = TaskState::BLOCKED;
    }

    dl.remove(*a); // remove real member
    dl.remove(*ghost); // remove non-member -> no-op
    JARVIS_ASSERT(dl.size() == 1);
    JARVIS_ASSERT(dl.peek_earliest() == b);
    dl_free(a);
    dl_free(b);
    dl_free(ghost);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: clear() empties the list.
// Expect: after clear, empty() and size()==0.
JARVIS_TEST(deadline_list_empty_and_clear, "PRE: none | POST: none") {
    DeadlineList dl;
    uint64_t base = arch::Timer::ticks();
    auto *a = dl_make(dl, base + 10);
    auto *b = dl_make(dl, base + 20);
    JARVIS_ASSERT(a && b);
    JARVIS_ASSERT(!dl.empty());
    dl.clear();
    JARVIS_ASSERT(dl.empty());
    JARVIS_ASSERT(dl.size() == 0);
    dl_free(a);
    dl_free(b);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The list accepts many tasks (bounded by headroom below
// MAX_TASKS) without corrupting order or size. Insertion order is reversed
// relative to deadline order.
// Expect: size equals the number of tasks actually created; peek_earliest is
//         the minimum-deadline task actually inserted; removing all leaves it
//         empty. Teardown runs via ScopeGuard so a failed assert cannot leak.
JARVIS_TEST(deadline_list_capacity, "PRE: none | POST: none") {
    DeadlineList dl;
    uint64_t base = arch::Timer::ticks();
    TaskControlBlock *made[64];
    uint64_t count = 0;
    uint64_t min_dl = ~0ULL;
    auto cleanup = ScopeGuard([&]() {
        for (uint64_t i = 0; i < count; ++i)
            if (made[i]) {
                dl.remove(*made[i]);
                dl_free(made[i]);
            }
    });
    // Insert deadlines in reverse so insertion order != sorted order.
    for (uint64_t i = 0; i < 32; ++i) {
        uint64_t const dl_t = base + (32 - i) * 100; // i=0 largest .. i=31 smallest
        auto *t = dl_make(dl, dl_t);
        if (t == nullptr)
            break; // scheduler near capacity — stop, still valid
        made[count++] = t;
        if (dl_t < min_dl)
            min_dl = dl_t;
    }
    JARVIS_ASSERT(dl.size() == count);
    JARVIS_ASSERT(dl.size() > 0);
    JARVIS_ASSERT(dl.peek_earliest()->deadline_ticks == min_dl);
    JARVIS_ASSERT(dl.empty() == false);
    JARVIS_TEST_PASS();
}
void register_timing_tests() {
    Logger::raw_write("[TIMING] register_timing_tests called!\n");
    Logger::info("Registering timing tests");
    JARVIS_REGISTER_TEST(timer_tick_accounting);
    JARVIS_REGISTER_TEST(timer_period_reload);
    JARVIS_REGISTER_TEST(timer_alarm_delivery);
    JARVIS_REGISTER_TEST(timer_alarm_not_expired);
    JARVIS_REGISTER_TEST(timer_rate_monotonic_schedule_indirect);
    JARVIS_REGISTER_TEST(timer_reap_orphans_periodic);
    JARVIS_REGISTER_TEST(timer_no_side_effects_on_idle);
    JARVIS_REGISTER_TEST(timer_daemon_restart_not_triggered_on_active);
    JARVIS_REGISTER_TEST(timer_deadline_miss_detection_fires);
    JARVIS_REGISTER_TEST(timer_deadline_miss_skips_future);
    JARVIS_REGISTER_TEST(timer_deadline_miss_only_once);
    JARVIS_REGISTER_TEST(timer_deadline_miss_skips_zero);
    // Phase 7 (P7a): DeadlineList (sorted deadline queue) unit coverage.
    JARVIS_REGISTER_TEST(deadline_list_sorted_insert);
    JARVIS_REGISTER_TEST(deadline_list_pop_expired);
    JARVIS_REGISTER_TEST(deadline_list_remove_mid);
    JARVIS_REGISTER_TEST(deadline_list_remove_absent);
    JARVIS_REGISTER_TEST(deadline_list_empty_and_clear);
    JARVIS_REGISTER_TEST(deadline_list_capacity);
}
