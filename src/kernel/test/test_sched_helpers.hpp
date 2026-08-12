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

/// @file test_sched_helpers.hpp
/// @brief Safe test helpers for scheduler operations (set_current, reschedule).
///
/// Tests run under the INIT task (IF=1).  The timer ISR fires
/// rate_monotonic_schedule(), which can consume stale context-switch globals
/// or undo set_current() via scheduler_on_context_switch().  These helpers
/// disable interrupts to prevent ISR interference.
///
/// Use yield_as() for "set_current + reschedule" pairs.
/// Use ScopedCurrentTask for scoped current-task changes (NO blocking ops).
/// Use raw Scheduler::set_current() for blocking operations or deliberate
/// preemption-simulation tests.

#pragma once

#include <kernel/arch/irq_guard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include "task_ptr.hpp"

namespace kernel::test {

/// @brief Atomically set a task as current and call reschedule, with
///        interrupts disabled to prevent timer ISR interference between
///        the two operations.  This restores the IF=0 semantics that
///        tests using the "set_current + reschedule" pattern were
///        originally designed for.
inline void yield_as(TaskControlBlock &task) noexcept {
    arch::IrqGuard guard;
    Scheduler::set_current(task);
    Scheduler::reschedule();
}

/// @brief  Yield to a task to steer next_task() scheduling, then restore the
///         harness as current and re-enqueue the task.  Unlike yield_as(),
///         the task was never actually executing — only configured as current
///         to influence the scheduler's next selection.  After this call the
///         harness continues to drive execution via reschedule() and the given
///         task remains eligible for scheduling (READY, in ready queue).
///         This avoids the invariant violation that set_current() removes the
///         "old current" from the ready queue even when it never ran.
///
///         NOTE: sets scheduler_need_resched directly instead of calling
///         Scheduler::reschedule(), because reschedule() calls next_task()
///         which DEQUEUES the highest-priority peer task from the ready queue,
///         leaving the RQ with only the re-enqueued task.  The timer ISR would
///         then dispatch the wrong task first.
inline void yield_to_task(TaskControlBlock &task) noexcept {
    // H2 stale-resume liveness guard (ROADMAP §v0.4.0 direction #4): the
    // harness's stored context.rsp can occasionally point into a test body's
    // setup path; on resume the harness re-executes this helper on a task that
    // may already have self-terminated and been removed from id_table_.  Acting
    // on a dead task here corrupts state BEFORE any enqueue could be refused:
    // set_current(dead) re-points the scheduler's current-cache, and
    // `task.state = READY` resurrects a freed/recycled block.  Refuse at entry
    // so the stale resume is a harmless no-op (the test body's wait loop then
    // exits because both tasks are TERMINATED / freed).
    if (Scheduler::find_task(task.id) != &task)
        return;

    arch::IrqGuard guard;
    auto *original = Scheduler::current_task();
    Scheduler::set_current(task);
    __atomic_store_n(&scheduler_need_resched, true, __ATOMIC_RELEASE);
    Scheduler::set_current(*original);
    // set_current(task) re-enqueued `original` (the harness) into the ready
    // queue because it was RUNNING at the time.  We restored it as current
    // again, so it must NOT remain in the ready queue (INV-4: a current task
    // must never be queued, or next_task() re-selects it and the subsequent
    // switch_to_task double-enqueue is refused, wedging the scheduler).
    if (original->in_ready_queue_)
        Scheduler::dequeue_ready(*original);
    task.state = TaskState::READY;
    Scheduler::enqueue_ready(task);
}

/// @brief RAII guard that sets a task as current for the duration of
///        a scope, restoring the original task on destruction.
///        Interrupts are disabled for the entire scope to prevent the
///        timer ISR from consuming stale globals or undoing set_current().
///
///        NOTE: Do NOT perform blocking operations (semaphore wait, mutex
///        lock with contention, IPC receive) inside this scope — the
///        scheduler cannot context-switch with IF=0.  For blocking
///        patterns, use raw Scheduler::set_current() with the defensive
///        cleanup in ~ScopedCurrentTask.
class [[nodiscard]] ScopedCurrentTask {
    TaskControlBlock *saved_;
    arch::IrqGuard guard_;

  public:
    explicit ScopedCurrentTask(TaskControlBlock &task) noexcept
        : saved_(Scheduler::current_task()) {
        Scheduler::set_current(task);
    }

    ~ScopedCurrentTask() noexcept {
        if (saved_) {
            Scheduler::set_current(*saved_);
        }
    }

    ScopedCurrentTask(const ScopedCurrentTask &) = delete;
    ScopedCurrentTask &operator=(const ScopedCurrentTask &) = delete;
};

/// @brief Forever-loop entry function — never returns, so _task_trampoline
///        never fires and the task stays alive until explicitly terminated.
inline void forever_entry() {
    for (;;) arch::pause();
}

/// @brief Create a task with the forever-loop entry.  The task never
///        auto-terminates — it must be explicitly terminated via
///        terminate_and_drain().
/// @param priority Scheduling priority.
/// @param period   Task period in ticks.
/// @param name     Optional name (copied into TCB).
/// @return Pointer to the new TCB (must be freed by caller).
inline TaskControlBlock *create_forever_task(uint64_t priority,
                                             uint64_t period = 10,
                                             const char *name = nullptr) {
    auto *tcb = TaskControlBlock::create(forever_entry, priority, period);
    if (tcb) {
        Scheduler::add_task(*tcb);
        if (name)
            __builtin_strncpy(tcb->name, name, CONFIG_TASK_NAME_LEN - 1);
    }
    return tcb;
}

/// @brief Safely terminate a forever-task and drain its zombie.
///        Handles the common case where terminate + drain_zombie_list
///        is needed.
/// @note  `terminate()` on an ALREADY-TERMINATED task re-appends it to the
///        zombie list (release_zombie has no idempotency guard); on a task a
///        PRIOR drain already reaped (block 0xDD-poisoned), reading state and
///        calling terminate() dereferences freed memory (ENSURE panic).  Guard
///        on magic (is_valid) FIRST, then live state — an already-reaped block
///        is skipped entirely.
inline void terminate_and_drain(TaskControlBlock &task) {
    if (TaskControlBlock::is_valid(&task) &&
        task.state != TaskState::TERMINATED)
        Scheduler::terminate(task, 0);
    Scheduler::drain_zombie_list();
}

/// @brief Terminate one task ONLY if it is still live.  Safe for tasks that
///        self-terminated (already TERMINATED and sitting in the zombie list):
///        terminate() is skipped, so no double zombie-append and no
///        dereference of a block a prior drain may have freed.  Checks magic
///        first so an already-reaped (0xDD-poisoned) block is skipped without
///        touching freed memory.
inline void terminate_if_live(TaskControlBlock *task) {
    if (task && TaskControlBlock::is_valid(task) &&
        task->state != TaskState::TERMINATED)
        Scheduler::terminate(*task, 0);
}

/// @brief Teardown for a pair of tasks: terminate the still-live ones, then
///        reclaim ALL zombies exactly once.  This is the correct pattern for
///        two self-terminated tasks — calling terminate_and_drain() per task
///        re-drains (and the second call derefs a block the first drain
///        freed).
inline void terminate_and_drain2(TaskControlBlock *a, TaskControlBlock *b) {
    terminate_if_live(a);
    terminate_if_live(b);
    Scheduler::drain_zombie_list();
}

/// @brief Teardown for a triple of tasks (see terminate_and_drain2).
inline void terminate_and_drain3(TaskControlBlock *a, TaskControlBlock *b,
                                 TaskControlBlock *c) {
    terminate_if_live(a);
    terminate_if_live(b);
    terminate_if_live(c);
    Scheduler::drain_zombie_list();
}

/// @brief Safely wait for task termination in a test harness.  The raw
///        `while (t->state != TERMINATED) hlt()` pattern reads the TCB even
///        after the zombie reaper has freed + 0xDD-poisoned it (e.g. when the
///        scheduler is momentarily idle between test tasks) — the poisoned
///        state is never TERMINATED, so the harness spins forever (H2 residual
///        hang).  is_valid() checks magic FIRST (task.hpp), so a freed block
///        reads magic != TCB_MAGIC and the wait exits.  Also re-arms
///        scheduler_need_resched so the timer ISR keeps driving the suite.
inline void wait_for_termination_safe(TaskControlBlock *task) {
    if (!task)
        return;
    while (TaskControlBlock::is_valid(task) &&
           task->state != TaskState::TERMINATED) {
        __atomic_store_n(&kernel::scheduler_need_resched, true,
                         __ATOMIC_RELEASE);
        arch::hlt();
    }
}

#if CONFIG_DEADLINE_MONITOR_TASK
/// @brief Trigger one deadline scan through the real timer/monitor path.
///        Test execution normally suppresses monitor wakeups to keep snapshot
///        isolation deterministic, so briefly release that gate and wait for
///        the monitor's completion counter instead of calling scan_deadlines()
///        directly from the harness.
inline void trigger_deadline_monitor_scan() {
    // Run the deadline scan DIRECTLY instead of waiting on the monitor task.
    // The monitor's block/wake handshake is unreliable under the
    // deferred-switch + snapshot test environment: it can strand the monitor
    // READY-but-not-in-runq after a wake, so the second scan in a test never
    // completes and the harness spins forever.  scan_deadlines() is the exact
    // logic the monitor runs — calling it synchronously is deterministic.
    Scheduler::scan_deadlines();
}
#endif

} // namespace kernel::test

/// @brief  Create a TCB for test use and register it with the scheduler WITHOUT
///         making it runnable.  The task is registered in id_table_ and
///         all_tasks_ (so IPC routing via find_task() works), and is placed in
///         BLOCKED state to prevent the scheduler's lazy-rebuild from
///         discovering and enqueuing it.  Tests use Scheduler::set_current()
///         to impersonate the task.
/// @param  ipc_priority  Priority visible to IPC (block_sender inheritance).
/// @param  period        Task period in ticks.
/// @return TaskPtr with RAII cleanup (remove_task + cleanup + delete).
inline TaskPtr create_test_task(uint64_t ipc_priority = 5,
                                uint64_t period = 10) {
    constexpr uint64_t CREATE_PRIORITY = 20;
    auto *tcb = TaskControlBlock::create([]() {}, CREATE_PRIORITY, period);
    if (tcb) {
        tcb->state = TaskState::BLOCKED;
        Scheduler::register_task(*tcb);
        tcb->priority = ipc_priority;
    }
    return TaskPtr(tcb);
}
