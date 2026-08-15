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

/// @file test_preemption.cpp
/// @brief Kernel preemption tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): the needs_switch() predicate is
/// exercised with REAL tasks at real priorities (relative to the harness at
/// prio 10) and the BLOCKED state is reached via a real blocking operation —
/// never via direct `task->state` or `cur->priority` writes.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/arch/timer.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {
struct PreemptionWaitContext {
    uint64_t child_id_;
};

// Parent entry: blocks in the real WAITPID syscall.  The handler dequeues the
// task and sets BLOCKED without holding any lock across the deferred switch
// (cookbook: the clean blocking path, unlike Semaphore::wait()).
void preemption_wait_parent_entry() {
    auto *self = Scheduler::current_task();
    auto *ctx = reinterpret_cast<PreemptionWaitContext *>(self->user_data);
    uint64_t status = 0;
    Syscall::handle(static_cast<uint64_t>(SyscallNumber::WAITPID),
                    ctx->child_id_,
                    reinterpret_cast<uint64_t>(&status), 0, 0, nullptr);
    while (self->state == TaskState::BLOCKED)
        arch::hlt();
}

// Child entry: stays live (READY at prio 5 < harness 10) so the parent's
// WAITPID never resolves; the child never dispatches ahead of the harness.
void preemption_forever_child_entry() {
    for (;;) {
        arch::hlt();
    }
}
} // namespace

// Runmode: kernel
// Testidea: needs_switch() returns true when a higher-priority READY task
// (prio 11 > harness 10) exists.
// Input: Real task (prio 11) added to the ready queue.
// Expect: Scheduler::needs_switch() returns true.
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(preemption_needs_switch_higher_priority, "PRE: none | POST: none") {
    auto *high = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(high != nullptr);
    // IRQs off across add_task + assert: the empty-lambda task must stay READY
    // in the queue.  A timer ISR dispatching it mid-window would run the empty
    // body and self-terminate it, making needs_switch() return false (flake —
    // widened by add_task's serial Logger::info inside the lock).
    bool result;
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*high);
        result = Scheduler::needs_switch();
    }
    JARVIS_ASSERT(result == true);

    kernel::test::terminate_and_drain(*high);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: needs_switch() returns false when the only READY task has the
// same priority as the current harness (round-robin handled by tick).
// Input: Real task (prio 10, equal to harness) added.
// Expect: needs_switch() returns false.
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(preemption_needs_switch_equal_priority, "PRE: none | POST: none") {
    auto *equal = TaskControlBlock::create([]() {}, 10, 10);
    JARVIS_ASSERT(equal != nullptr);
    Scheduler::add_task(*equal);

    bool result = Scheduler::needs_switch();

    kernel::test::terminate_and_drain(*equal);
    JARVIS_ASSERT(result == false);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: needs_switch() returns false when a higher-priority task exists
// but is BLOCKED (not runnable) — reached through the real WAITPID block.
// Input: A real parent (prio 11) blocks in WAITPID on a live lower-priority
//        child (prio 5).  Both are registered together under an IRQ guard.
// Expect: needs_switch() returns false; both tasks are reclaimed cleanly.
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(preemption_needs_switch_blocked_higher, "PRE: none | POST: none") {
    PreemptionWaitContext ctx{};
    auto *parent =
        TaskControlBlock::create(preemption_wait_parent_entry, 11, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->user_data = &ctx;

    auto *child = TaskControlBlock::create(preemption_forever_child_entry, 5, 10);
    JARVIS_ASSERT(child != nullptr);
    parent->add_child(child);
    ctx.child_id_ = child->id;

    {
        arch::IrqGuard guard;
        Scheduler::add_task(*parent);
        Scheduler::add_task(*child);
    }

    Scheduler::reschedule();
    while (parent->state != TaskState::BLOCKED &&
           parent->state != TaskState::TERMINATED)
        arch::pause();
    JARVIS_ASSERT(parent->state == TaskState::BLOCKED);

    bool result = Scheduler::needs_switch();
    JARVIS_ASSERT(result == false);

    // Cleanup BEFORE asserting (cookbook Rule 5): the child is a live READY
    // task, the parent is BLOCKED in WAITPID.  Reclaim both without touching
    // a zombie.
    kernel::test::terminate_and_drain(*child);
    Scheduler::terminate(*parent, 0);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: needs_switch() is NOT gated by the preemptible flag: a
// higher-priority READY task still forces a switch while preemption is
// disabled.  (preempt_enabled_ is a setter/getter pair with no scheduling
// effect; the flag is not consulted by needs_switch().)
// Input: Real task (prio 11) added; preemption disabled.
// Expect: needs_switch() returns true while preemption is off.
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(preemption_needs_switch_ignores_preemptible_flag,
            "PRE: none | POST: none") {
    Scheduler::set_preemptible(false);

    auto *high = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(high != nullptr);
    Scheduler::add_task(*high);

    bool result = Scheduler::needs_switch();

    // Cleanup BEFORE asserting (cookbook Rule 5): the task is live READY.
    kernel::test::terminate_and_drain(*high);
    Scheduler::set_preemptible(true);

    JARVIS_ASSERT(result == true);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies toggling preemption on/off repeatedly does not crash and
// state remains consistent.
// Input: Loop 100 times: set_preemptible(true), set_preemptible(false).
// Expect: No crash; is_preemptible() reflects each toggle correctly.
// Depends: kernel::task::Scheduler
JARVIS_TEST(preemption_interrupt_enable_disable_cycle,
            "PRE: none | POST: none") {
    for (uint64_t i = 0; i < 100; ++i) {
        Scheduler::set_preemptible(true);
        JARVIS_ASSERT(Scheduler::is_preemptible() == true);

        Scheduler::set_preemptible(false);
        JARVIS_ASSERT(Scheduler::is_preemptible() == false);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A REAL periodic task (prio 11, period 5) that runs past a full
// period observes its remaining_ticks reload from 0 back to period — proving
// the real on_tick quantum-accounting reload path.
// Input: Real task busy-waits ~10 real ticks while polling remaining_ticks.
// Expect: remaining_ticks reloads (jumps up after reaching 0).
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(preemption_quantum_exhaustion, "PRE: none | POST: none") {
    static volatile bool g_reloaded = false;

    auto *t = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            uint64_t prev = self->remaining_ticks;
            // Wait on REAL ticks (period 5) so the reload path is guaranteed
            // to fire instead of racing a fixed iteration budget.
            uint64_t start = arch::Timer::ticks();
            while (arch::Timer::ticks() - start < 20) {
                uint64_t cur = self->remaining_ticks;
                if (cur > prev) {
                    g_reloaded = true; // reloaded from 0 back to period
                    break;
                }
                prev = cur;
                arch::pause();
            }
        },
        11, 5);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    // Cleanup BEFORE asserting (cookbook Rule 5): the task self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(g_reloaded);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the real RMS dispatch picks a higher-priority READY task
// and does not dispatch the harness to itself.
// Input: Real task (prio 11) added; the harness calls reschedule() and the
//        timer ISR dispatches the higher-priority task.
// Expect: The higher-priority task genuinely runs (completes).
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(preemption_task_switch_does_not_switch_to_self,
            "PRE: none | POST: none") {
    static uint64_t g_ran = 0;
    auto *other = TaskControlBlock::create(
        []() { g_ran = 1; }, 11, 10);
    JARVIS_ASSERT(other != nullptr);
    Scheduler::add_task(*other);

    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(other);
    JARVIS_ASSERT_EQ(1ULL, g_ran);

    // The task self-terminated and is owned by the zombie list.
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

void register_preemption_tests() {
    Logger::info("Registering preemption tests");
    JARVIS_REGISTER_TEST(preemption_needs_switch_higher_priority);
    JARVIS_REGISTER_TEST(preemption_needs_switch_equal_priority);
    JARVIS_REGISTER_TEST(preemption_needs_switch_blocked_higher);
    JARVIS_REGISTER_TEST(preemption_needs_switch_ignores_preemptible_flag);
    JARVIS_REGISTER_TEST(preemption_interrupt_enable_disable_cycle);
    JARVIS_REGISTER_TEST(preemption_quantum_exhaustion);
    JARVIS_REGISTER_TEST(preemption_task_switch_does_not_switch_to_self);
}
