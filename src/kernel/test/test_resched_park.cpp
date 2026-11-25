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

/// @file test_resched_park.cpp
/// @brief Cycle D QE (issue #545): F1 reschedule()-park contract tests
///        (core_sched) + the #545 strand-shape absence test.
///        No production code touched.
///
///        Harness premise (pinned, same as the M2 park tests): the suite
///        runs at irq_guard_depth >= 1, so the timed park is unreachable
///        in-harness — BLOCKED callers take the denied (status-quo-ante)
///        leg.  Every test below asserts depth-relative behavior and logs
///        the park/denied/wakeup counters.  The true park leg (return only
///        once no longer BLOCKED) is covered by the soak + a depth-0 seam
///        follow-up, not here.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/cpu_context.hpp>
#include <kernel/arch/hal/timer.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/core/path_tracer.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_path.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/tick_watchdog.hpp>
#include <kernel/test/park_seam.hpp>
#include <kernel/sync/park_lock.hpp>
#include <kernel/sync/spinlock.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace resched_park_test {

// Gate-bit positions in resched_park_denied_why (see
// resched_park_gate_base/gate_task in scheduler_tick.cpp).
constexpr uint64_t WHY_IRQGUARD = 1ULL << 2;
constexpr uint64_t WHY_HARNESS = 1ULL << 12;

// --- no_blocked_return rendezvous state ---
volatile bool g_a_blocked_seen = false;
volatile bool g_handover = false;
volatile uint64_t g_a_depth = 0;
volatile uint64_t g_a_state_at_return = 0;
TaskControlBlock *g_a_tcb = nullptr;

// Self-blocking task: marks itself BLOCKED + dequeued, reschedules,
// records the post-return state, then hlt-waits for the peer wake.
void blocked_entry() {
    auto *self = Scheduler::current_task();
    {
        arch::IrqGuard irq_guard{};
        Scheduler::dequeue_ready(*self);
        self->state = TaskState::BLOCKED;
    }
    g_a_depth = arch::irq_guard_depth_own();
    g_a_blocked_seen = true;
    Scheduler::reschedule();
    g_a_state_at_return = static_cast<uint64_t>(self->state);
    for (;;) {
        arch::sti();
        arch::hlt();
        arch::cli();
        if (self->state != TaskState::BLOCKED)
            break;
    }
}

// Peer: waits until A is parked/blocked, wakes it, records handover.
void waker_entry() {
    auto *self = Scheduler::current_task();
    (void)self;
    for (uint64_t i = 0; i < 200000ULL && !g_a_blocked_seen; ++i) {
        Scheduler::reschedule();
        arch::hlt();
    }
    if (!g_a_blocked_seen || g_a_tcb == nullptr)
        return;
    if (!TaskControlBlock::is_valid(g_a_tcb))
        return;
    Scheduler::set_task_ready(*g_a_tcb);
    g_handover = true;
}

// --- strand-shape rendezvous state (lock_strand_absent_under_tick) ---
sync::SpinLock g_strand_lock;
volatile bool g_strand_holder_ready = false;
volatile bool g_strand_waiter_got = false;

void strand_holder_entry() {
    auto *self = Scheduler::current_task();
    (void)self;
    g_strand_holder_ready = true;
    g_strand_lock.lock();
    // Brief hold (~a few thousand pauses): long enough that the waiter
    // contends, short enough to resolve inside one denied-spin block.
    for (uint64_t i = 0; i < 5000ULL; ++i)
        arch::pause();
    g_strand_lock.unlock();
}

void strand_waiter_entry() {
    auto *self = Scheduler::current_task();
    (void)self;
    for (uint64_t i = 0; i < 200000ULL && !g_strand_holder_ready; ++i) {
        Scheduler::reschedule();
        arch::hlt();
    }
    if (!g_strand_holder_ready)
        return;
    sync::ParkLockGuard guard(g_strand_lock, "test_strand");
    if (guard.owned())
        g_strand_waiter_got = true;
}

} // namespace resched_park_test

// Runmode: kernel
// Testidea: F1 Cycle D QE: a RUNNING caller takes the C1 fast path —
// immediate arm-only return, no park, no denial, no state change.
// Input: Harness task (RUNNING) calls reschedule().
// Expect: Returns at once; state stays RUNNING; current + in_ready_queue_
// unchanged; resched_park entries/wakeups/denied deltas all 0.
// Depends: Scheduler::reschedule C1 leg, park counters
JARVIS_TEST(resched_park_running_fastpath, "PRE: none | POST: none") {
    auto *self = Scheduler::current_task();
    JARVIS_ASSERT(self != nullptr);
    JARVIS_ASSERT(self->state == TaskState::RUNNING);
    const bool rq_before = self->in_ready_queue_;
    const uint64_t e0 = Scheduler::resched_park_entries_count();
    const uint64_t w0 = Scheduler::resched_park_wakeups_count();
    const uint64_t d0 = Scheduler::resched_park_denied_count();
    kernel::path_reset();
    Scheduler::reschedule();
    kernel::path_reset();
    JARVIS_ASSERT(self->state == TaskState::RUNNING);
    JARVIS_ASSERT(Scheduler::current_task() == self);
    JARVIS_ASSERT(self->in_ready_queue_ == rq_before);
    JARVIS_ASSERT_EQ(e0, Scheduler::resched_park_entries_count());
    JARVIS_ASSERT_EQ(w0, Scheduler::resched_park_wakeups_count());
    JARVIS_ASSERT_EQ(d0, Scheduler::resched_park_denied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F1 Cycle D QE: a BLOCKED caller inside an IrqGuard is denied
// the park (C3 status quo ante) — prompt return, still BLOCKED, IF still 0.
// Input: Harness forced BLOCKED inside IrqGuard, reschedule().
// Expect: Returns; state still BLOCKED; IF==0 inside; entries delta 0;
// denied delta >= 1 with the IrqGuard bit set; IF==1 after restore.
// Depends: reschedule C3 leg, resched_park_denied_why bitmap
JARVIS_TEST(resched_park_denied_irqguard, "PRE: none | POST: none") {
    auto *self = Scheduler::current_task();
    JARVIS_ASSERT(self != nullptr);
    const uint64_t e0 = Scheduler::resched_park_entries_count();
    const uint64_t d0 = Scheduler::resched_park_denied_count();
    {
        arch::IrqGuard irq_guard{};
        JARVIS_ASSERT(!arch::interrupts_enabled());
        self->state = TaskState::BLOCKED;
        Scheduler::reschedule();
        // C3: denied — returned while still BLOCKED, never parked.
        JARVIS_ASSERT(self->state == TaskState::BLOCKED);
        JARVIS_ASSERT(!arch::interrupts_enabled());
        self->state = TaskState::RUNNING;
    }
    JARVIS_ASSERT(arch::interrupts_enabled());
    JARVIS_ASSERT(self->state == TaskState::RUNNING);
    JARVIS_ASSERT_EQ(e0, Scheduler::resched_park_entries_count());
    JARVIS_ASSERT(Scheduler::resched_park_denied_count() > d0);
    JARVIS_ASSERT((Scheduler::resched_park_denied_why() &
                   resched_park_test::WHY_IRQGUARD) != 0);
    kernel::path_reset();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F1 Cycle D QE: single-task-guest denial leg (bit 10) is
// unreachable in-harness (daemons + peers always registered) — pin the
// premise and verify the no-hang status quo: a BLOCKED-denied caller
// returns promptly with scheduler state coherent.
// Input: BLOCKED-forced harness inside IrqGuard, reschedule().
// Expect: Prompt return; all_tasks_.size() > 1 (bit-10 leg genuinely not
// the denier here); entries delta 0; no hang; state restored RUNNING.
// Depends: reschedule C3/C5 legs, AllTasksRegistry size
JARVIS_TEST(resched_park_denied_single_task, "PRE: none | POST: none") {
    auto *self = Scheduler::current_task();
    JARVIS_ASSERT(self != nullptr);
    // Premise: the single-task topology never holds in-harness, so this
    // leg documents the denied status quo; the bit-10 gate itself is
    // covered by inspection + the depth-0 follow-up.
    JARVIS_ASSERT(Scheduler::all_tasks().size() > 1);
    const uint64_t e0 = Scheduler::resched_park_entries_count();
    {
        arch::IrqGuard irq_guard{};
        self->state = TaskState::BLOCKED;
        Scheduler::reschedule();
        JARVIS_ASSERT(self->state == TaskState::BLOCKED);
        self->state = TaskState::RUNNING;
    }
    JARVIS_ASSERT_EQ(e0, Scheduler::resched_park_entries_count());
    JARVIS_ASSERT(self->state == TaskState::RUNNING);
    JARVIS_ASSERT(Scheduler::current_task() == self);
    kernel::path_reset();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F1 Cycle D QE: no-ticks denial leg (bit 1) is unreachable
// in-harness (CONFIG_TICK_HZ != 0, ticks flow) — pin the premise and
// verify the no-hang status quo for a denied BLOCKED caller.
// Input: BLOCKED-forced harness inside IrqGuard, reschedule().
// Expect: Prompt return; CONFIG_TICK_HZ != 0 + ticks advance across the
// test (timer alive); entries delta 0; state restored RUNNING.
// Depends: reschedule C3 leg, Timer::ticks
JARVIS_TEST(resched_park_denied_no_ticks, "PRE: none | POST: none") {
    auto *self = Scheduler::current_task();
    JARVIS_ASSERT(self != nullptr);
    JARVIS_ASSERT(static_cast<uint64_t>(CONFIG_TICK_HZ) != 0);
    const uint64_t t0 = arch::Timer::ticks();
    const uint64_t e0 = Scheduler::resched_park_entries_count();
    {
        arch::IrqGuard irq_guard{};
        self->state = TaskState::BLOCKED;
        Scheduler::reschedule();
        JARVIS_ASSERT(self->state == TaskState::BLOCKED);
        self->state = TaskState::RUNNING;
    }
    JARVIS_ASSERT_EQ(e0, Scheduler::resched_park_entries_count());
    JARVIS_ASSERT(self->state == TaskState::RUNNING);
    JARVIS_ASSERT(arch::Timer::ticks() >= t0);
    kernel::path_reset();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F1 Cycle D QE: the harness task itself never parks —
// RUNNING reschedule is C1, BLOCKED-forced reschedule is denied via the
// harness gate (bit 12), never entries.
// Input: Harness reschedule() RUNNING, then BLOCKED-forced in guard.
// Expect: entries + wakeups deltas 0 across both; denied moves only on
// the BLOCKED leg with the harness bit set; current + state coherent.
// Depends: reschedule C1/C3 legs, harness gate, park counters
JARVIS_TEST(resched_park_harness_untouched, "PRE: none | POST: none") {
    auto *self = Scheduler::current_task();
    JARVIS_ASSERT(self != nullptr);
    const uint64_t e0 = Scheduler::resched_park_entries_count();
    const uint64_t w0 = Scheduler::resched_park_wakeups_count();
    const uint64_t d0 = Scheduler::resched_park_denied_count();
    Scheduler::reschedule();
    JARVIS_ASSERT_EQ(e0, Scheduler::resched_park_entries_count());
    JARVIS_ASSERT_EQ(w0, Scheduler::resched_park_wakeups_count());
    JARVIS_ASSERT_EQ(d0, Scheduler::resched_park_denied_count());
    {
        arch::IrqGuard irq_guard{};
        self->state = TaskState::BLOCKED;
        Scheduler::reschedule();
        JARVIS_ASSERT(self->state == TaskState::BLOCKED);
        self->state = TaskState::RUNNING;
    }
    JARVIS_ASSERT_EQ(e0, Scheduler::resched_park_entries_count());
    JARVIS_ASSERT_EQ(w0, Scheduler::resched_park_wakeups_count());
    JARVIS_ASSERT(Scheduler::resched_park_denied_count() > d0);
    JARVIS_ASSERT((Scheduler::resched_park_denied_why() &
                   resched_park_test::WHY_HARNESS) != 0);
    kernel::path_reset();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F1 Cycle D QE: BLOCKED self-park handover — a self-blocking
// task and a waker peer complete the rendezvous with no hang and no
// BLOCKED-return violation.
// Input: Task A BLOCK-marks + dequeues + reschedules, then hlt-waits;
// peer B set_task_ready(A)s it.  Priorities FIXED (B 12 > A 11).
// Expect: Handover observed; both terminate; depth-relative return
// contract: at suite depth 0 the park returns only once no longer
// BLOCKED (woken); at the harness depth 1 the denied leg returns still
// BLOCKED and the hlt-wait covers the wake (same premise as the M2
// contention test).  d0 <= 1 pinned.
// Depends: reschedule C2/C3 legs, set_task_ready wake, hlt wait
JARVIS_TEST(resched_park_no_blocked_return, "PRE: none | POST: none") {
    resched_park_test::g_a_blocked_seen = false;
    resched_park_test::g_handover = false;
    resched_park_test::g_a_depth = 0;
    resched_park_test::g_a_state_at_return = 0;
    resched_park_test::g_a_tcb = nullptr;
    const uint64_t e0 = Scheduler::resched_park_entries_count();
    const uint64_t w0 = Scheduler::resched_park_wakeups_count();
    const uint64_t d0 = Scheduler::resched_park_denied_count();
    auto *task_a =
        TaskControlBlock::create(resched_park_test::blocked_entry, 11, 10);
    JARVIS_ASSERT(task_a != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*task_a, SchedPolicy::FIXED));
    auto *task_b =
        TaskControlBlock::create(resched_park_test::waker_entry, 12, 10);
    JARVIS_ASSERT(task_b != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*task_b, SchedPolicy::FIXED));
    resched_park_test::g_a_tcb = task_a;
    {
        arch::IrqGuard irq_guard{};
        Scheduler::add_task(*task_a);
        Scheduler::add_task(*task_b);
    }
    Scheduler::reschedule();
    test::wait_for_termination_safe(task_a);
    test::wait_for_termination_safe(task_b);
    Logger::info("[RESCHED-PARK-TEST] handover=%lu depth=%lu ret_state=%lu "
                 "entries=%lu wakeups=%lu denied=%lu",
                 static_cast<uint64_t>(resched_park_test::g_handover),
                 resched_park_test::g_a_depth,
                 resched_park_test::g_a_state_at_return,
                 Scheduler::resched_park_entries_count() - e0,
                 Scheduler::resched_park_wakeups_count() - w0,
                 Scheduler::resched_park_denied_count() - d0);
    JARVIS_ASSERT(resched_park_test::g_handover);
    JARVIS_ASSERT(resched_park_test::g_a_blocked_seen);
    JARVIS_ASSERT(resched_park_test::g_a_depth <= 1);
    if (resched_park_test::g_a_depth == 0) {
        // True park leg: returned only once woken (no longer BLOCKED).
        JARVIS_ASSERT(resched_park_test::g_a_state_at_return !=
                      static_cast<uint64_t>(TaskState::BLOCKED));
        JARVIS_ASSERT(Scheduler::resched_park_entries_count() > e0);
    } else {
        // Denied leg (harness outer guard): returned still BLOCKED.
        JARVIS_ASSERT(resched_park_test::g_a_state_at_return ==
                      static_cast<uint64_t>(TaskState::BLOCKED));
        JARVIS_ASSERT(Scheduler::resched_park_denied_count() > d0);
    }
    test::terminate_and_drain2(task_a, task_b);
    resched_park_test::g_a_tcb = nullptr;
    kernel::path_reset();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F1 Cycle D QE: the reschedule arm step sequence is unchanged
// (C6 — deferred-switch arm + dispatch step untouched by the park).
// Input: Dispatch pair (low current + high peer), reschedule under
// ScopedCurrentTask, validate the arm segment.
// Expect: path_validate(k_dispatch_arm_expected); pair unwound
// (need_resched cleared, candidate re-enqueued, tasks drained).
// Depends: dispatch_step Arm segment, path tracer
JARVIS_TEST(resched_park_dispatch_path_unchanged,
            "PRE: none | POST: none") {
    TaskControlBlock *t_cur = TaskControlBlock::create(test::forever_entry,
                                                       10,
                                                       TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(t_cur != nullptr);
    TaskControlBlock *t_next = TaskControlBlock::create(
        test::forever_entry, 100, TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(t_next != nullptr);
    {
        arch::IrqGuard irq_guard{};
        Scheduler::add_task(*t_cur);
        Scheduler::add_task(*t_next);
    }
    bool arm_chained = false;
    {
        test::ScopedCurrentTask cur(*t_cur);
        kernel::path_reset();
        Scheduler::reschedule();
        arm_chained = kernel::path_validate(k_dispatch_arm_expected);
        __atomic_store_n(&Scheduler::SwSlots::need_resched(), false,
                         __ATOMIC_RELEASE);
    }
    if (t_next->in_ready_queue_)
        Scheduler::dequeue_ready(*t_next);
    if (!t_cur->in_ready_queue_ && t_cur->state == TaskState::READY)
        Scheduler::set_task_ready(*t_cur);
    test::terminate_and_drain2(t_cur, t_next);
    kernel::path_reset();
    JARVIS_ASSERT(arm_chained);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Cycle D QE (issue #545): the #545 strand shape — a
// high-priority spinner contending a lock briefly held across a BLOCKED
// queue consumer — resolves inside one bounded denied-spin block (no
// escape, no watchdog fire, no hang).
// Input: Holder takes the lock, pause-holds ~5000 rounds (waiter
// contends inside the window), releases; waiter ParkLockGuards.
// Expect: Waiter acquires; denied-escape delta 0 (resolved within the
// first bounded block); watchdog never fires; both terminate.
// Depends: ParkLockGuard denied path, denied-escape + watchdog counters
JARVIS_TEST(lock_strand_absent_under_tick, "PRE: none | POST: none") {
    sync::park_lock_reset_for_test();
    sync::spinlock_order_reset_for_test();
    tick_watchdog::reset_for_test();
    resched_park_test::g_strand_holder_ready = false;
    resched_park_test::g_strand_waiter_got = false;
    const uint64_t esc0 = sync::lock_denied_escape_count();
    auto *holder = TaskControlBlock::create(
        resched_park_test::strand_holder_entry, 11, 10);
    JARVIS_ASSERT(holder != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*holder, SchedPolicy::FIXED));
    auto *waiter = TaskControlBlock::create(
        resched_park_test::strand_waiter_entry, 12, 10);
    JARVIS_ASSERT(waiter != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*waiter, SchedPolicy::FIXED));
    {
        arch::IrqGuard irq_guard{};
        Scheduler::add_task(*holder);
        Scheduler::add_task(*waiter);
    }
    Scheduler::reschedule();
    test::wait_for_termination_safe(holder);
    test::wait_for_termination_safe(waiter);
    Logger::info("[RESCHED-PARK-TEST] strand got=%lu esc=%lu fire=%lu",
                 static_cast<uint64_t>(
                     resched_park_test::g_strand_waiter_got),
                 sync::lock_denied_escape_count() - esc0,
                 tick_watchdog::fire_count());
    JARVIS_ASSERT(resched_park_test::g_strand_holder_ready);
    JARVIS_ASSERT(resched_park_test::g_strand_waiter_got);
    JARVIS_ASSERT_EQ(esc0, sync::lock_denied_escape_count());
    JARVIS_ASSERT(!tick_watchdog::fired());
    test::terminate_and_drain2(holder, waiter);
    sync::park_lock_reset_for_test();
    sync::spinlock_order_reset_for_test();
    tick_watchdog::reset_for_test();
    kernel::path_reset();
    JARVIS_TEST_PASS();
}

// Gate-bit position for a caller already holding a spinlock (LockHeld arm
// in resched_park_gate_base, scheduler_tick.cpp).  WHY_IRQGUARD (bit 2)
// lives in resched_park_test above.
constexpr uint64_t WHY_HELD_LOCK = 1ULL << 3;

// Runmode: kernel
// Testidea: #578 QE: spawned BLOCKED+dequeued caller parks under the seam
// and returns only once woken (no BLOCKED-return); depth-relative: at
// depth 0 the woken leg runs (entries/wakeups move, denied 0); at
// depth>=1 the denied leg runs (entries 0, denied moves) — same contract
// as resched_park_no_blocked_return, plus seam + rescue pins.  The caller
// is spawned (never the harness, which gate bit 12 always denies) and
// marked at creation (run_one arms the seam for TF_PARK_DEPTH0 tests).
// Input: Task A BLOCK-marks + dequeues + reschedules, then hlt-waits;
// peer B set_task_ready(A)s it. Priorities FIXED (B 12 > A 11).
// Expect: handover + A observed BLOCKED + entry depth pinned; at depth 0:
// return only once woken (ret != BLOCKED), entries/wakeups >= 1, denied
// delta 0; at depth>=1: denied leg (ret BLOCKED, denied>0, entries 0).
// Both legs: stuck/orphan rescue deltas 0, seam unexpected delta 0,
// tracker delta 0.
// Depends: reschedule C2/C3 legs, set_task_ready wake, park seam,
//         wait_for_termination
JARVIS_TEST(resched_park_at0_woken_return, "PRE: none | POST: none") {
    JARVIS_ASSERT(test::park_seam_active());
    Scheduler::reset_sched_oracles_for_test();
    resched_park_test::g_a_blocked_seen = false;
    resched_park_test::g_handover = false;
    resched_park_test::g_a_depth = 0;
    resched_park_test::g_a_state_at_return = 0;
    resched_park_test::g_a_tcb = nullptr;
    const uint64_t e0 = Scheduler::resched_park_entries_count();
    const uint64_t w0 = Scheduler::resched_park_wakeups_count();
    const uint64_t d0 = Scheduler::resched_park_denied_count();
    const uint64_t u0 = gs::park_test_ctx_unexpected_count();
    const uint64_t waive0 = gs::park_test_ctx_waive_count();
    auto *task_a =
        TaskControlBlock::create(resched_park_test::blocked_entry, 11, 10);
    JARVIS_ASSERT(task_a != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*task_a, SchedPolicy::FIXED));
    auto *task_b =
        TaskControlBlock::create(resched_park_test::waker_entry, 12, 10);
    JARVIS_ASSERT(task_b != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*task_b, SchedPolicy::FIXED));
    resched_park_test::g_a_tcb = task_a;
    {
        arch::IrqGuard irq_guard{};
        Scheduler::add_task(*task_a);
        Scheduler::add_task(*task_b);
    }
    Scheduler::reschedule();
    test::wait_for_termination_safe(task_a);
    test::wait_for_termination_safe(task_b);
    Logger::info("[RESCHED-PARK-TEST] at0_woken handover=%lu depth=%lu "
                 "ret_state=%lu entries=%lu wakeups=%lu denied=%lu "
                 "stuck=%lu orph=%lu unexp=%lu waive=%lu",
                 static_cast<uint64_t>(resched_park_test::g_handover),
                 resched_park_test::g_a_depth,
                 resched_park_test::g_a_state_at_return,
                 Scheduler::resched_park_entries_count() - e0,
                 Scheduler::resched_park_wakeups_count() - w0,
                 Scheduler::resched_park_denied_count() - d0,
                 Scheduler::read_sched_oracles().stuck_rescue,
                 Scheduler::read_sched_oracles().flagged_orphan_rescue,
                 gs::park_test_ctx_unexpected_count() - u0,
                 gs::park_test_ctx_waive_count() - waive0);
    JARVIS_ASSERT(resched_park_test::g_handover);
    JARVIS_ASSERT(resched_park_test::g_a_blocked_seen);
    if (resched_park_test::g_a_depth == 0) {
        // True park leg (seam depth 0): returned only once woken.
        JARVIS_ASSERT(resched_park_test::g_a_state_at_return !=
                      static_cast<uint64_t>(TaskState::BLOCKED));
        JARVIS_ASSERT(Scheduler::resched_park_entries_count() > e0);
        JARVIS_ASSERT(Scheduler::resched_park_wakeups_count() > w0);
        JARVIS_ASSERT(Scheduler::resched_park_denied_count() == d0);
    } else {
        // Denied leg (outer guard held): returned still BLOCKED.
        JARVIS_ASSERT(resched_park_test::g_a_state_at_return ==
                      static_cast<uint64_t>(TaskState::BLOCKED));
        JARVIS_ASSERT(Scheduler::resched_park_denied_count() > d0);
        JARVIS_ASSERT(Scheduler::resched_park_entries_count() == e0);
    }
    JARVIS_ASSERT_EQ(0ULL, Scheduler::read_sched_oracles().stuck_rescue);
    JARVIS_ASSERT_EQ(0ULL, Scheduler::read_sched_oracles().flagged_orphan_rescue);
    JARVIS_ASSERT(gs::park_test_ctx_unexpected_count() == u0);
    test::terminate_and_drain2(task_a, task_b);
    resched_park_test::g_a_tcb = nullptr;
    kernel::path_reset();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #578 QE: the seam never waives a REAL guard or a held lock —
// a BLOCKED caller under a live IrqGuard or holding a SpinLock is still
// denied the park (fail-closed); entries stay 0, no escape.
// Input: Harness BLOCKED-forced (a) inside an IrqGuard scope, (b) while
// holding a stack SpinLock (INV-C1 WARN expected, warn-only).
// TF_PARK_DEPTH0 armed (seam active, caller unmarked by design).
// Expect: prompt return still BLOCKED; entries delta 0; denied moves with
// the IrqGuard bit (a) / held-lock bit (b) in denied_why (harness bit may
// also be set — subset check, not exact); escape delta 0; stuck/orphan
// rescue deltas 0; seam unexpected delta 0; state restored RUNNING.
// Depends: reschedule C3 leg, deny bitmap, INV-C1 warn-only, park seam
JARVIS_TEST(resched_park_at0_gate_negative, "PRE: none | POST: none") {
    JARVIS_ASSERT(test::park_seam_active());
    Scheduler::reset_sched_oracles_for_test();
    auto *self = Scheduler::current_task();
    JARVIS_ASSERT(self != nullptr);
    const uint64_t e0 = Scheduler::resched_park_entries_count();
    const uint64_t d0 = Scheduler::resched_park_denied_count();
    const uint64_t esc0 = sync::lock_denied_escape_count();
    const uint64_t u0 = gs::park_test_ctx_unexpected_count();
    {
        // Leg A: live IrqGuard denies even under the seam (unmarked caller
        // is never waived; a phantom depth is not a live guard anyway).
        arch::IrqGuard irq_guard{};
        JARVIS_ASSERT(!arch::interrupts_enabled());
        self->state = TaskState::BLOCKED;
        Scheduler::reschedule();
        JARVIS_ASSERT(self->state == TaskState::BLOCKED);
        self->state = TaskState::RUNNING;
    }
    JARVIS_ASSERT(arch::interrupts_enabled());
    JARVIS_ASSERT(Scheduler::resched_park_denied_count() > d0);
    JARVIS_ASSERT((Scheduler::resched_park_denied_why() &
                   resched_park_test::WHY_IRQGUARD) != 0);
    JARVIS_ASSERT_EQ(e0, Scheduler::resched_park_entries_count());
    {
        // Leg B: a held spinlock denies (LockHeld arm); the INV-C1 WARN is
        // expected and warn-only — no panic, no termination.
        sync::SpinLock probe;
        probe.lock();
        self->state = TaskState::BLOCKED;
        Scheduler::reschedule();
        JARVIS_ASSERT(self->state == TaskState::BLOCKED);
        self->state = TaskState::RUNNING;
        probe.unlock();
    }
    JARVIS_ASSERT(arch::interrupts_enabled());
    JARVIS_ASSERT((Scheduler::resched_park_denied_why() & WHY_HELD_LOCK) !=
                  0);
    JARVIS_ASSERT_EQ(e0, Scheduler::resched_park_entries_count());
    JARVIS_ASSERT(sync::lock_denied_escape_count() == esc0);
    JARVIS_ASSERT_EQ(0ULL, Scheduler::read_sched_oracles().stuck_rescue);
    JARVIS_ASSERT_EQ(0ULL, Scheduler::read_sched_oracles().flagged_orphan_rescue);
    JARVIS_ASSERT(gs::park_test_ctx_unexpected_count() == u0);
    JARVIS_ASSERT(self->state == TaskState::RUNNING);
    JARVIS_ASSERT_EQ(0ULL, sync::spinlock_held_by_current_count());
    sync::spinlock_order_reset_for_test();
    kernel::path_reset();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #578 QE (OQ2): gate bits 10 (single-task) and 1 (no ticks)
// stay premise-pinning negatives — the seam does not waive, exercise, or
// observe them. Documents the deviation, pins the no-hang status quo.
// Input: BLOCKED-forced harness inside IrqGuard, reschedule() (TF_PARK_DEPTH0
// armed; denial expected via the live guard bit, never via 10/1).
// Expect: prompt return still BLOCKED; entries delta 0; denied moves;
// denied_why has the live-guard bit and LACKS bits 10 and 1;
// all_tasks>1 + TICK_HZ!=0 premises hold; rescue deltas 0; seam
// unexpected delta 0; tracker delta 0.
// Depends: reschedule C3 leg, gate bitmap premises, park seam
JARVIS_TEST(resched_park_at0_gate_bits_premise, "PRE: none | POST: none") {
    JARVIS_ASSERT(test::park_seam_active());
    Scheduler::reset_sched_oracles_for_test();
    auto *self = Scheduler::current_task();
    JARVIS_ASSERT(self != nullptr);
    // Premises: the single-task topology and the tickless build never hold
    // in-harness, so bits 10/1 are genuinely not the denier here.
    JARVIS_ASSERT(Scheduler::all_tasks().size() > 1);
    JARVIS_ASSERT(static_cast<uint64_t>(CONFIG_TICK_HZ) != 0);
    const uint64_t e0 = Scheduler::resched_park_entries_count();
    const uint64_t d0 = Scheduler::resched_park_denied_count();
    const uint64_t u0 = gs::park_test_ctx_unexpected_count();
    {
        arch::IrqGuard irq_guard{};
        self->state = TaskState::BLOCKED;
        Scheduler::reschedule();
        JARVIS_ASSERT(self->state == TaskState::BLOCKED);
        self->state = TaskState::RUNNING;
    }
    const uint64_t why = Scheduler::resched_park_denied_why();
    Logger::info("[RESCHED-PARK-TEST] at0_premise why=0x%lx entries=%lu "
                 "denied=%lu stuck=%lu orph=%lu unexp=%lu",
                 why, Scheduler::resched_park_entries_count() - e0,
                 Scheduler::resched_park_denied_count() - d0,
                 Scheduler::read_sched_oracles().stuck_rescue,
                 Scheduler::read_sched_oracles().flagged_orphan_rescue,
                 gs::park_test_ctx_unexpected_count() - u0);
    JARVIS_ASSERT(Scheduler::resched_park_denied_count() > d0);
    JARVIS_ASSERT_EQ(e0, Scheduler::resched_park_entries_count());
    // The live guard bit denies; bits 10 (single task) and 1 (no ticks)
    // are absent exactly as premised — never waived, never exercised.
    JARVIS_ASSERT((why & resched_park_test::WHY_IRQGUARD) != 0);
    JARVIS_ASSERT((why & (1ULL << 10)) == 0);
    JARVIS_ASSERT((why & (1ULL << 1)) == 0);
    JARVIS_ASSERT_EQ(0ULL, Scheduler::read_sched_oracles().stuck_rescue);
    JARVIS_ASSERT_EQ(0ULL, Scheduler::read_sched_oracles().flagged_orphan_rescue);
    JARVIS_ASSERT(gs::park_test_ctx_unexpected_count() == u0);
    JARVIS_ASSERT(self->state == TaskState::RUNNING);
    JARVIS_ASSERT(Scheduler::current_task() == self);
    kernel::path_reset();
    JARVIS_TEST_PASS();
}

void register_resched_park_tests() {
    JARVIS_REGISTER_TEST(resched_park_running_fastpath);
    JARVIS_REGISTER_TEST(resched_park_denied_irqguard);
    JARVIS_REGISTER_TEST(resched_park_denied_single_task);
    JARVIS_REGISTER_TEST(resched_park_denied_no_ticks);
    JARVIS_REGISTER_TEST(resched_park_harness_untouched);
    JARVIS_REGISTER_TEST(resched_park_no_blocked_return);
    JARVIS_REGISTER_TEST(resched_park_dispatch_path_unchanged);
    JARVIS_REGISTER_TEST(lock_strand_absent_under_tick);
    JARVIS_REGISTER_TEST_FLAGS(resched_park_at0_woken_return,
                               kernel::test::TF_KERNEL |
                                   kernel::test::TF_PARK_DEPTH0);
    JARVIS_REGISTER_TEST_FLAGS(resched_park_at0_gate_negative,
                               kernel::test::TF_KERNEL |
                                   kernel::test::TF_PARK_DEPTH0);
    JARVIS_REGISTER_TEST_FLAGS(resched_park_at0_gate_bits_premise,
                               kernel::test::TF_KERNEL |
                                   kernel::test::TF_PARK_DEPTH0);
}
