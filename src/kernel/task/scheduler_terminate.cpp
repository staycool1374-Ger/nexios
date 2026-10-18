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

/// @file scheduler_terminate.cpp
/// @brief Task termination paths for Scheduler (issue #323).
///        Pure move out of scheduler.cpp: no logic changes.

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_switch_internal.hpp>
#include <kernel/task/scheduler_queue_internal.hpp>
#include <kernel/task/scheduler_integrity.hpp>   // issue #369 T1b validate_task_ptr
#include <assert.hpp>                              // issue #369 T1b ENSURE
#include <kernel/arch/timer.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <lib/logger.hpp>
#include <kernel/log/dmesg.hpp>

namespace kernel {

using sched_queue::edf_peek_valid_own;
using sched_verify::validate_task_ptr;

errors::SchedulerError
Scheduler::terminate_err(TaskControlBlock &task,
                         uint64_t exit_code) noexcept {
    ENSURE(validate_task_ptr(&task, "terminate_err"));
    arch::IrqGuard irq_guard{};
    // Issue #197: refuse to terminate a task that is current on ANOTHER
    // CPU (evaluated before taking scheduler_lock_ — leaf-only reads, no
    // new lock takes). Freeing an AP-live stack poisons the running task
    // (GPF in edf_remove / switch-corruption ENSURE under -smp 2).
    // Self-termination always succeeds (exempted below). No mutation on
    // refusal: no dequeue, no state write, no release, no reaper poke.
    if (is_current_on_any_cpu(&task) && &task != current_task()) {
        kernel::Logger::warn("sched: terminate id=%u refused (remote-current)",
                             static_cast<unsigned>(task.id));
        // Issue #234: fail-closed refusal enters the ring as WARN.
        log::dmesg_push_sev(log::ErrorSubsystem::SCHED,
                            log::kDmesgBase_SCHED + 14, log::LogSeverity::WARN,
                            task.name, task.id);
        return errors::SCHED_ERR_REMOTE_CURRENT;
    }
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    charge_exec(task); // issue #21: bill the final partial quantum (no loss)
#if defined(CONFIG_DEBUG_IPC_SCHED)
    if (task.id == 6) {
        auto *cur = current_task();
        kernel::Logger::raw_write("[H2-TERM6] cur=");
        kernel::Logger::print_dec(cur ? cur->id : 0u);
        kernel::Logger::raw_write(" st=");
        kernel::Logger::print_dec(static_cast<uint64_t>(task.state));
        kernel::Logger::raw_write(" inrq=");
        kernel::Logger::print_dec(task.in_ready_queue_ ? 1u : 0u);
        kernel::Logger::raw_write(" rq_prio=");
        kernel::Logger::print_dec(task.rq_priority_);
        kernel::Logger::raw_write(" tick=");
        kernel::Logger::print_dec(arch::Timer::ticks());
        kernel::Logger::raw_write("\n");
    }
#endif
    dequeue_ready(task);
    task.state = TaskState::TERMINATED;
    task.exit_code = exit_code;
    // If the parent is blocked in waitpid for this child, wake it now so it
    // does not deadlock waiting for a child that exited without sys_exit.
    wake_waiting_parent(task);

    // Release into the zombie list for deferred cleanup by the idle task.
    // For non-self termination this is the final step.
    // For self-termination we call it before the context switch: it removes
    // the task from scheduler tables (all_tasks_, deadline_list_, id_table_)
    // but does NOT free the kernel stack or TCB — those remain valid until
    // idle's cleanup_step() calls cleanup() + MemPool::free().  The context
    // switch only reads task.context for the save, so the table removal is
    // safe before switch_to_task.
    release_zombie(task);

    // Issue #155: prompt the PID 1 reaper after every zombie birth
    // (unconditional — spurious wakes are harmless under its
    // level-triggered drain loop).  Placed after the list push above
    // so wake-then-miss is impossible.  Same IrqGuard + scheduler_lock_
    // region (global→leaf order: notify takes only its leaf lock).
    // Deadlock-free by affinity clamping: set_affinity always clamps
    // masks to up-CPUs, so the reaper's target is either here
    // (lock-free enqueue_ready) or a booted AP (mailbox+IPI) — never
    // the down-CPU direct-enqueue branch that would retake this lock.
    if (task.id != 1) {
        if (auto *reaper = Scheduler::find_task(1))
            reaper->notify.notify(1);
    }

    // If the terminating task is the one currently on the CPU, arrange for a
    // context switch to a valid successor on the next ISR.  Otherwise
    // current_task() stays parked on a TERMINATED task and the running RSP
    // is later saved into its dead context, which can wedge the scheduler in
    // the idle loop (observed as the `all` suite hanging at the atomic
    // context-switch tests).
    if (&task == current_task()) {
    // peek the highest-priority ready task without dequeuing it.
    // next_task() would dequeue, but reschedule() never dispatches —
    // it only requests a deferred switch (INV-4).  Dequeuing here would
    // orphan the task until the next lazy rebuild, causing the calling
    // test's tight `while (state != TERMINATED) reschedule();` loop to
    // constantly dequeue → lazy-rebuild → dequeue (infinite livelock).
    // Issue #19: consult the EDF queue first — it is invisible to the
    // bitmap peek, and stranding an EDF successor wedges the same way.
    auto *next = edf_peek_valid_own();
    if (next == nullptr)
        next = rq_own().peek_highest();
    if (next && next != &task) {
        switch_to_task(&task, *next, nullptr);
    }
    }
    return errors::SCHED_ERR_OK;
}

void Scheduler::terminate(TaskControlBlock &task, uint64_t exit_code) noexcept {
    // Void legacy wrapper: refusal is fail-closed warn-only (all existing
    // callers are own-CPU paths where refusal is unreachable). Fallible
    // callers use terminate_err directly.
    const errors::SchedulerError err = terminate_err(task, exit_code);
    if (err != errors::SCHED_ERR_OK) {
        kernel::Logger::warn("sched: terminate id=%u refused (err=%u)",
                             static_cast<unsigned>(task.id),
                             static_cast<unsigned>(err));
        // Issue #234: refusal code enters the ring (canonical SCHED nr).
        log::dmesg_push_sev(
            log::ErrorSubsystem::SCHED,
            log::kDmesgBase_SCHED + static_cast<uint64_t>(err),
            log::LogSeverity::WARN, task.name, task.id);
    }
}

}  // namespace kernel
