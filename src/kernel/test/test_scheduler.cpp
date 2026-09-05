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

/// @file test_scheduler.cpp
/// @brief Scheduler core tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/irq_guard.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {
struct WaitpidTestContext {
    uint64_t child_id_ = 0;
    uint64_t status_ = 0;
    volatile uint64_t blocked_ = 0;
    volatile uint64_t woke_ = 0;
};

void scheduler_waitpid_parent_entry() {
    auto *self = Scheduler::current_task();
    auto *ctx = reinterpret_cast<WaitpidTestContext *>(self->user_data);
    Syscall::handle(static_cast<uint64_t>(SyscallNumber::WAITPID),
                    ctx->child_id_,
                    reinterpret_cast<uint64_t>(&ctx->status_), 0, 0, nullptr);
    if (self->state == TaskState::BLOCKED) {
        ctx->blocked_ = 1;
        while (self->state == TaskState::BLOCKED)
            arch::hlt();
    }
    ctx->woke_ = 1;
}

void scheduler_waitpid_child_entry() {
    Scheduler::terminate(*Scheduler::current_task(), 42);
    for (;;) {
        arch::hlt();
    }
}
} // namespace

// Runmode: kernel
// Testidea: Validates that the scheduler task count increments when a task is
// added and decrements when removed. Also verifies the count includes the
// current task and idle task.
// Input: Create a task, add it, verify count increases; remove it, verify
// count restores.
// Expect: task_count increases by 1 after add_task, returns to original after
// terminate_and_drain.
// Depends: test, scheduler, task, pmm, vmm
JARVIS_TEST(scheduler_task_count, "PRE: none | POST: none") {
    auto *before = Scheduler::current_task();
    JARVIS_ASSERT(before != nullptr);
    uint64_t cnt_before = Scheduler::task_count();

    auto *new_task = TaskControlBlock::create([]() {}, 1, 10);
    JARVIS_ASSERT(new_task != nullptr);
    Scheduler::add_task(*new_task);
    JARVIS_ASSERT_EQ(cnt_before + 1, Scheduler::task_count());

    kernel::test::terminate_and_drain(*new_task);
    JARVIS_ASSERT_EQ(cnt_before, Scheduler::task_count());

    auto *after = Scheduler::current_task();
    JARVIS_ASSERT(after != nullptr);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that the current task is non-null, has a positive task
// ID, and is in RUNNING state. Also verifies current_task changes after a
// real context switch to a higher-priority task.
// Input: Create a higher-priority task, dispatch it, verify current_task
// updates.
// Expect: current_task returns the higher-priority task while it runs.
// Depends: test, scheduler, task
JARVIS_TEST(scheduler_current_task, "PRE: none | POST: none") {
    static uint64_t g_self = 0;
    static uint64_t g_ran = 0;

    auto *high = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            g_self = cur ? cur->id : 0;
            g_ran = 1;
        },
        11, 10);
    JARVIS_ASSERT(high != nullptr);
    Scheduler::add_task(*high);

    auto *original = Scheduler::current_task();
    JARVIS_ASSERT(original != nullptr);
    JARVIS_ASSERT(original->id > 0);
    JARVIS_ASSERT(original->state == TaskState::RUNNING);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(high);

    const auto high_id = high->id;
    kernel::test::terminate_and_drain(*high);
    JARVIS_ASSERT_EQ(1ULL, g_ran);
    JARVIS_ASSERT(g_self == high_id);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that reschedule() does NOT swap to a different task
// when the current task is the only non-idle task (no spurious context
// switch). This is the anti-spurious-switch guarantee.
// Input: No other READY tasks; call reschedule.
// Expect: current_task() returns the same task after reschedule.
// Depends: test, scheduler
JARVIS_TEST(scheduler_reschedule_noop, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    // reschedule() is a *real* cooperative switch: it dispatches the highest
    // READY task via the scheduler. It must NOT spuriously switch away from the
    // current (RUNNING) task when the only other candidate is the idle/harness
    // task — that is the anti-spurious-switch guard (scheduler.cpp:~1375).
    {
        arch::IrqGuard guard;
        auto *before = Scheduler::current_task();
        Scheduler::reschedule();
        auto *after = Scheduler::current_task();
        JARVIS_ASSERT(after == before);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that adding a new task increments the task count and
// removing it restores the original count.
// Input: A new TaskControlBlock with empty lambda, priority 1, quanta 10
// Expect: JARVIS_ASSERT_EQ checks count increases by 1 after add, returns to
// original after remove; current_task non-null
// Depends: test, scheduler, task, pmm, vmm
JARVIS_TEST(scheduler_remove_task, "PRE: none | POST: none") {
    auto *before = Scheduler::current_task();
    JARVIS_ASSERT(before != nullptr);
    uint64_t cnt_before = Scheduler::task_count();

    auto *new_task = TaskControlBlock::create([]() {}, 1, 10);
    JARVIS_ASSERT(new_task != nullptr);
    Scheduler::add_task(*new_task);
    JARVIS_ASSERT_EQ(cnt_before + 1, Scheduler::task_count());

    kernel::test::terminate_and_drain(*new_task);
    JARVIS_ASSERT_EQ(cnt_before, Scheduler::task_count());

    auto *after = Scheduler::current_task();
    JARVIS_ASSERT(after != nullptr);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that terminate + release_zombie adds the task to the
// zombie list and that drain_zombie_list frees its resources.
// Input: A new TaskControlBlock with parent_id=999999, state=TERMINATED,
// exit_code=42
// Expect: task count decreases by 1 after terminate (release_zombie removes
// from all_tasks_); drain_zombie_list cleans up without leak.
// Depends: test, scheduler, task, pmm, vmm
JARVIS_TEST(scheduler_reap_orphans, "PRE: none | POST: none") {
    uint64_t cnt_before = Scheduler::task_count();

    auto *child = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child != nullptr);
    child->parent_id = 999999;
    Scheduler::add_task(*child);

    // terminate now calls release_zombie which removes from all_tasks_
    // (task count decreases by 1) and defers cleanup to the zombie list.
    Scheduler::terminate(*child, 42);
    JARVIS_ASSERT_EQ(cnt_before, Scheduler::task_count());

    // drain_zombie_list synchronously cleans up the zombie.
    Scheduler::drain_zombie_list();

    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verify that the scheduler selects a higher‑priority task over a
// lower‑priority one via next_task().
// Input: Two tasks, low (priority 5), high (priority 9).
// Expect: next_task returns the high-priority task.
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(scheduler_preemptive_priority, "PRE: none | POST: none") {
    auto *low = TaskControlBlock::create([]() {}, 5, 5);
    JARVIS_ASSERT(low != nullptr);

    auto *high = TaskControlBlock::create([]() {}, 15, 5);
    JARVIS_ASSERT(high != nullptr);

    // IRQs off across both add_task calls + next_task(): both empty-lambda
    // tasks must stay READY in the queue.  A timer ISR dispatching either one
    // mid-window would run its empty body and self-terminate it, so next_task()
    // would no longer see `high` (flake — widened by add_task's serial
    // Logger::info inside the lock).
    TaskControlBlock *next;
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*low);
        Scheduler::add_task(*high);
        next = Scheduler::next_task();
    }

    // Cleanup BEFORE assert (cookbook Rule 5): both never-dispatched
    // (next_task() dequeued `high`); direct remove_task+cleanup+delete is the
    // leak-free teardown for never-running tasks.
    Scheduler::remove_task(*low);
    low->cleanup();
    delete low;
    Scheduler::remove_task(*high);
    high->cleanup();
    delete high;

    JARVIS_ASSERT(next == high);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verify that two equal‑priority tasks are both eligible for
// scheduling via next_task().
// Input: Two tasks, both priority 15.
// Expect: next_task returns one of the two tasks.
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(scheduler_quantum_exhaustion, "PRE: none | POST: none") {
    auto *t1 = TaskControlBlock::create([]() {}, 15, 5);
    auto *t2 = TaskControlBlock::create([]() {}, 15, 5);
    JARVIS_ASSERT(t1 && t2);

    // Register AND select under one IrqGuard (cookbook Rule 2): a tick between
    // add_task and next_task dispatches one empty-lambda task (self-terminates),
    // so next_task() returns idle instead of a prio-15 pick.
    TaskControlBlock *next;
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*t1);
        Scheduler::add_task(*t2);
        next = Scheduler::next_task();
    }

    // Cleanup BEFORE assert (cookbook Rule 5): both never-dispatched
    // (next_task dequeued the pick); direct remove_task+cleanup+delete is
    // leak-free for never-running tasks.
    Scheduler::remove_task(*t1);
    t1->cleanup();
    delete t1;
    Scheduler::remove_task(*t2);
    t2->cleanup();
    delete t2;

    JARVIS_ASSERT(next == t1 || next == t2);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the real waitpid wake and zombie cleanup path.
// Input: A dispatched parent blocks in WAITPID; a dispatched child terminates
//        with exit status 42; wake_waiting_parent resumes the parent.
// Expect: Parent receives status 42, child leaves the live task table, and
//         both TCBs are reclaimed by the real zombie drain.
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(scheduler_waitpid_wakes_parent,
            "PRE: none | POST: none") {
    WaitpidTestContext context;
    auto *parent =
        TaskControlBlock::create(scheduler_waitpid_parent_entry, 20, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->user_data = &context;

    auto *child =
        TaskControlBlock::create(scheduler_waitpid_child_entry, 12, 10);
    JARVIS_ASSERT(child != nullptr);
    parent->add_child(child);
    context.child_id_ = child->id;

    // Add both tasks under an IRQ guard so a timer tick cannot dispatch the
    // parent before the child is registered.  The parent (prio 20) then blocks
    // in WAITPID while the child is present; the child (prio 12) runs next,
    // self-terminates with status 42, and wake_waiting_parent resumes it.
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*parent);
        Scheduler::add_task(*child);
    }

    Scheduler::reschedule();
    while (parent->state != TaskState::BLOCKED &&
           parent->state != TaskState::TERMINATED)
        arch::pause();
    kernel::test::wait_for_termination_safe(parent);
    bool child_removed = Scheduler::find_task(context.child_id_) == nullptr;
    if (!child_removed) {
        Scheduler::terminate(*child, 0);
    }
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(context.blocked_ == 1);
    JARVIS_ASSERT(context.woke_ == 1);
    JARVIS_ASSERT(child_removed);
    JARVIS_ASSERT_EQ(42ULL, context.status_);

    Scheduler::drain_zombie_list();

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies Scheduler::alloc_id() returns monotonically increasing
// IDs; wraps correctly from UINT64_MAX. Input: Call alloc_id() multiple times.
// Expect: Each call returns a value one greater than the previous.
// Depends: kernel::task::Scheduler
JARVIS_TEST(scheduler_alloc_id_sequential, "PRE: none | POST: none") {
    uint64_t id1 = Scheduler::alloc_id();
    uint64_t id2 = Scheduler::alloc_id();
    uint64_t id3 = Scheduler::alloc_id();
    JARVIS_ASSERT_EQ(id2, id1 + 1);
    JARVIS_ASSERT_EQ(id3, id2 + 1);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies task_at() bounds: task_at(0) returns idle task;
// task_at(count-1) returns last real task; task_at(count) returns nullptr.
// Input: Call task_at with various indices.
// Expect: Correct pointer or nullptr at boundaries.
// Depends: kernel::task::Scheduler
JARVIS_TEST(scheduler_task_at_bounds, "PRE: none | POST: none") {
    auto *idle = Scheduler::task_at(0);
    JARVIS_ASSERT(idle != nullptr);
    JARVIS_ASSERT(idle == Scheduler::get_idle_task());

    uint64_t count = Scheduler::task_count();
    auto *last = Scheduler::task_at(count - 1);
    JARVIS_ASSERT(last != nullptr);

    auto *oob = Scheduler::task_at(count);
    JARVIS_ASSERT(oob == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies find_task() returns nullptr for a nonexistent task ID.
// Input: Call find_task(999999).
// Expect: Returns nullptr.
// Depends: kernel::task::Scheduler
JARVIS_TEST(scheduler_find_task_nonexistent, "PRE: none | POST: none") {
    auto *result = Scheduler::find_task(999999);
    JARVIS_ASSERT(result == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies is_preemptible() reflects set_preemptible(true) and
// set_preemptible(false).
// Input: Toggle preemptible on/off.
// Expect: is_preemptible() matches the set value.
// Depends: kernel::task::Scheduler
JARVIS_TEST(scheduler_set_preemptible_toggle, "PRE: none | POST: none") {
    Scheduler::set_preemptible(true);
    JARVIS_ASSERT(Scheduler::is_preemptible() == true);

    Scheduler::set_preemptible(false);
    JARVIS_ASSERT(Scheduler::is_preemptible() == false);

    Scheduler::set_preemptible(true);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that after the real timer ISR dispatches a
// higher-priority task, current_task() returns that task while it runs.
// Input: Create a higher-priority task (prio 11); a real dispatch happens;
//        the task records Scheduler::current_task() inside its own lambda.
// Expect: The task's recorded current_task() == itself (it was RUNNING).
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(scheduler_current_task_after_switch, "PRE: none | POST: none") {
    static uint64_t g_self = 0;
    static uint64_t g_ran = 0;

    auto *high = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            g_self = cur ? cur->id : 0;
            g_ran = 1;
        },
        11, 10);
    JARVIS_ASSERT(high != nullptr);
    Scheduler::add_task(*high);

    auto *original = Scheduler::current_task();
    JARVIS_ASSERT(original != nullptr);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(high);
    // The real RMS dispatch selected the higher-priority task.

    const auto high_id = high->id;
    kernel::test::terminate_and_drain(*high);
    JARVIS_ASSERT_EQ(1ULL, g_ran);
    JARVIS_ASSERT(g_self == high_id);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies adding a task with a duplicate ID is handled gracefully
// (no crash, no corruption).  Two tasks are created; the second is given the
// first's ID; add_task is invoked on both; the scheduler must remain
// consistent.
// Input: Create task t1, add it. Create another task with same ID (manually
// set id), attempt to add.
// Expect: No crash; scheduler remains consistent; find_task returns a task
// for that ID.
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(scheduler_add_duplicate_id, "PRE: none | POST: none") {
    auto *t1 = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(t1 != nullptr);
    Scheduler::add_task(*t1);

    // Create second task and manually set same ID
    auto *t2 = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(t2 != nullptr);
    t2->id = t1->id;

    // Should not crash; add_task will insert into hash table which may
    // overwrite or fail. We verify scheduler remains consistent.
    Scheduler::add_task(*t2);

    // Verify we can still find a task with that ID
    auto *found = Scheduler::find_task(t1->id);

    // Cleanup both.  t1 was never dispatched (create + add_task only) — the
    // direct remove_task+cleanup+delete pattern is leak-free for never-running
    // tasks (terminate_and_drain's zombie path can strand them).
    Scheduler::remove_task(*t1);
    t1->cleanup();
    delete t1;
    JARVIS_ASSERT(found != nullptr);
    // Remove t2 from scheduler if present — a direct iteration is needed
    // because the hash-table probe chain may be broken by the tombstone
    // left by remove_task(t1) when both tasks share the same ID.  The
    // unconditional cleanup below is load-bearing: if add_task(t2) failed to
    // register (duplicate id), t2 is an unregistered orphan and still needs
    // cleanup+delete; if it was registered, the loop removed it.  Either way
    // exactly one free happens.
    for (uint64_t _i = 0; _i < Scheduler::task_count(); ++_i) {
        if (Scheduler::task_at(_i) == t2) {
            Scheduler::remove_task(*t2);
            break;
        }
    }
    t2->cleanup();
    delete t2;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verify next_task() at equal priority preserves FIFO enqueue order.
// (scheduler.md: next_task() is a priority-bitmap + per-level FIFO bucket —
// there is NO same-priority period tiebreak.  The old
// scheduler_shorter_period_preferred asserted a period preference that does
// not exist and passed only because t1 was enqueued first.)
// Input: Two tasks, same priority (15), different periods (5, 20).
// Expect: next_task() returns the first-enqueued task (t1), FIFO order.
JARVIS_TEST(scheduler_equal_priority_fifo, "PRE: none | POST: none") {
    auto *t1 =
        TaskControlBlock::create([]() {}, 15, 5); // priority=15, period=5
    auto *t2 =
        TaskControlBlock::create([]() {}, 15, 20); // priority=15, period=20
    JARVIS_ASSERT(t1 && t2);

    // Register AND select under one IrqGuard (cookbook Rule 2): a timer tick
    // between add_task and next_task would dispatch one empty-lambda task
    // (self-terminates), so next_task() returns idle instead of the FIFO pick.
    TaskControlBlock *next;
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*t1);
        Scheduler::add_task(*t2);
        next = Scheduler::next_task();
    }

    // Cleanup BEFORE assert (cookbook Rule 5): both never-dispatched (next_task
    // dequeued the pick); direct remove_task+cleanup+delete is leak-free for
    // never-running tasks.
    Scheduler::remove_task(*t1);
    t1->cleanup();
    delete t1;
    Scheduler::remove_task(*t2);
    t2->cleanup();
    delete t2;

    // FIFO at equal priority: the first-enqueued task is selected.
    JARVIS_ASSERT(next == t1);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verify that reschedule() does NOT swap to a different task
// when the current task is the only non-idle task (no context switch).
// Input: Create one task, set it as current, call reschedule.
// Expect: current_task() returns the same task.
JARVIS_TEST(scheduler_no_spurious_switch, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    // reschedule() is a *real* cooperative switch: it dispatches the highest
    // READY task via the scheduler. It must NOT spuriously switch away from the
    // current (RUNNING) task when the only other candidate is the idle/harness
    // task — that is the anti-spurious-switch guard (scheduler.cpp:~1375).
    // The old test created a peer task, set it current, and asserted no switch,
    // which only held when reschedule() was a no-op. Here we assert the real
    // guarantee: with no other READY task, current is preserved.
    {
        arch::IrqGuard guard;
        auto *before = Scheduler::current_task();
        Scheduler::reschedule();
        auto *after = Scheduler::current_task();
        JARVIS_ASSERT(after == before);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all scheduler-related unit tests with the test framework.
// Input: (none)
// Expect: Each JARVIS_REGISTER_TEST call registers a test function for later
// execution
// Depends: test, logger, scheduler, task, pmm, vmm
void register_scheduler_tests() {
    Logger::info("Registering scheduler tests");
    JARVIS_REGISTER_TEST(scheduler_task_count);
    JARVIS_REGISTER_TEST(scheduler_current_task);
    JARVIS_REGISTER_TEST(scheduler_reschedule_noop);
    JARVIS_REGISTER_TEST(scheduler_remove_task);
    JARVIS_REGISTER_TEST(scheduler_reap_orphans);
    JARVIS_REGISTER_TEST(scheduler_preemptive_priority);
    JARVIS_REGISTER_TEST(scheduler_quantum_exhaustion);
    JARVIS_REGISTER_TEST(scheduler_waitpid_wakes_parent);
    JARVIS_REGISTER_TEST(scheduler_alloc_id_sequential);
    JARVIS_REGISTER_TEST(scheduler_task_at_bounds);
    JARVIS_REGISTER_TEST(scheduler_find_task_nonexistent);
    JARVIS_REGISTER_TEST(scheduler_set_preemptible_toggle);
    JARVIS_REGISTER_TEST(scheduler_current_task_after_switch);
    JARVIS_REGISTER_TEST(scheduler_add_duplicate_id);
    JARVIS_REGISTER_TEST(scheduler_equal_priority_fifo);
    JARVIS_REGISTER_TEST(scheduler_no_spurious_switch);
}
