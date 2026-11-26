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
#include <kernel/task/scheduler_path.hpp>            // issue #324 Tick 13 teardown trace
#include <kernel/task/scheduler_switch_internal.hpp>
#include <kernel/task/scheduler_queue_internal.hpp>
#include <kernel/task/scheduler_integrity.hpp>   // issue #369 T1b validate_task_ptr
#include <kernel/task/scheduler_lock_guard.hpp>  // scheduler_lock_depth_own
#include <kernel/daemon/daemon_mgr.hpp>
#include <kernel/ipc/death_notify.hpp>           // M1/F6 death latch
#include <kernel/ipc/ipc.hpp>                    // M1/F7 waiter drains
#include <kernel/proc/proc_tcb_caps.hpp>         // F11 proc-mgr hold funnel release
#include <assert.hpp>                              // issue #369 T1b ENSURE
#include <kernel/arch/timer.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <lib/logger.hpp>
#include <kernel/log/dmesg.hpp>

namespace kernel {

using sched_queue::edf_peek_valid_own;
using sched_verify::validate_task_ptr;

/// @brief First-writer-wins death-cause store (issue #516).
///        Single CAS on the zero sentinel; recycled TCBs start at 0
///        (all five memset-zero birth sites + the init-list zero).
void Scheduler::note_death_cause(TaskControlBlock &dead,
                                 ipc::DeathCause cause) noexcept {
    uint64_t expected = 0;
    const uint64_t wanted = static_cast<uint64_t>(cause);
    __atomic_compare_exchange_n(&dead.death_cause, &expected, wanted, false,
                                __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

/// @brief Latch a death record + wake the dead task's IPC waiters
///        (M1/F6/F7, issues #559/#560, INV-MK-6/INV-MK-11).
void Scheduler::publish_task_death(TaskControlBlock &dead,
                                   ipc::DeathCause cause) noexcept {
    ENSURE(validate_task_ptr(&dead, "publish_task_death"));
    // Wake path takes scheduler_lock_ on the not-own-CPU branch
    // (set_task_ready): publishing under the global lock would panic a
    // supervisor affine off-BSP.  All terminate funnels release the lock
    // before calling here; the deferred complete_teardown backstop never
    // holds it either.
    ENSURE(scheduler_lock_depth_own() == 0);
    // M1/F6 (#516): clear the supervisor PID cell BEFORE the IRQ-off
    // window.  Idempotent (pid==0 after the first clear is a silent
    // no-op); bounded serial emit; needs no lock and no atomicity with
    // the latch below.
    Scheduler::note_death_cause(dead, cause);
    kernel::daemon::notify_death(dead.id);
    arch::IrqGuard irq_guard{};
    // Exactly-once by construction: on_task_death only transitions
    // ACTIVE→PENDING, so a repeat call finds no slot and emits no poke;
    // the reply drain only READY-marks (the waiter owns its flag clear).
    ipc::DeathNotify::on_task_death(dead);
    // M1/F7: wake reply waiters whose dest tag names the dead task.
    // Queue-full senders are deliberately NOT woken here: their queue is
    // still intact at funnel time, and the unconditional teardown drain
    // (MessageQueue::~MessageQueue) wakes them with fast-fail when the
    // queue is destroyed.  Waking them here collapses the BLOCKED window
    // the handshake depends on (test IpcBlockedSenderOnReceiverCleanup:
    // a woken sender retries, fast-fails and terminates before the
    // harness ever observes BLOCKED) — convicted by bisection, full
    // class green without the early sender wake.
    IPC::wake_reply_waiters_of(dead);
    // F11 (#564, INV-PROC-5): drop every proc-mgr TCB-cap hold naming the
    // dead task (target leg) or held by it (holder leg, index-only — the
    // dying holder's CNode teardown owns its wrappers/edges). IrqGuard is
    // held above with scheduler_lock_ NOT held (ENSUREd at entry);
    // release_on_death adds only leaf takes, is idempotent across
    // double-publish (generation-mismatch no-op), and performs no logging.
    proc::proc_tcb_caps::release_on_death(dead.id, dead.generation);
    // Wake-only: no reschedule(), no switch_to_task (single-writer arm
    // rule, H2).  The poked supervisor dispatches on the next tick.
}

errors::SchedulerError
Scheduler::terminate_err(TaskControlBlock &task,
                         uint64_t exit_code,
                         ipc::DeathCause cause) noexcept {

    ENSURE(validate_task_ptr(&task, "terminate_err"));
    arch::IrqGuard irq_guard{};
    // Issue #197: refuse to terminate a task that is current on ANOTHER
    // CPU (evaluated before taking scheduler_lock_ — leaf-only reads, no
    // new lock takes). Freeing an AP-live stack poisons the running task
    // (GPF in edf_remove / switch-corruption ENSURE under -smp 2).
    // Self-termination always succeeds (exempted below). No mutation on
    // refusal: no dequeue, no state write, no release, no reaper poke.
    // Issue #324 Tick 14: refusal leg is cold (fail-closed exception).
    if (is_current_on_any_cpu(&task) && &task != current_task()) [[unlikely]] {
        kernel::Logger::warn("sched: terminate id=%u refused (remote-current)",
                             static_cast<unsigned>(task.id));
        // Issue #234: fail-closed refusal enters the ring as WARN.
        log::dmesg_push_sev(log::ErrorSubsystem::SCHED,
                            log::kDmesgBase_SCHED + 14, log::LogSeverity::WARN,
                            task.name, task.id);
        return errors::SCHED_ERR_REMOTE_CURRENT;
    }
    ENSURE(scheduler_lock_depth_own() == 0); // issue #369 T2b: no reentrant take (non-recursive)
    // M1 scope A: termination mutations under the lock (unchanged order).
    {
        SchedulerLockGuard guard(scheduler_lock_);
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
    // F4 INV-ADM-5 (issue #557): death is terminal for the admin
    // latch — clear every field so a recycled TCB slot never inherits
    // a stale park.  The deadline node is dropped when it was parked
    // (remove is membership-guarded, hence a safe no-op otherwise).
    __atomic_store_n(&task.admin_parked, false, __ATOMIC_RELEASE);
    __atomic_store_n(&task.admin_wake_pending, false, __ATOMIC_RELEASE);
    __atomic_store_n(&task.admin_stop_requested, false, __ATOMIC_RELEASE);
    __atomic_store_n(&task.admin_wdog_expiry_tick, 0, __ATOMIC_RELEASE);
    deadline_list_.remove(task);
    // Issue #53: join-rendezvous handoff for terminating threads (covers
    // kill/fault/watchdog paths funnelled here, mirroring sys_exit's
    // inline handoff).  No-ops for non-threads (checked inside).
    TaskControlBlock::thread_join_note_termination(task);
    // If the parent is blocked in waitpid for this child, wake it now so it
    // does not deadlock waiting for a child that exited without sys_exit.
    wake_waiting_parent(task);
    // Issue #324 Tick 13: teardown trace — termination mutation complete
    // (dequeue + TERMINATED mark + parent wake); release_zombie steps next.
    teardown_step(SchedTeardownSteps::Terminate);

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
    } // M1 scope A ends: lock released before the death publish.

    // M1/F6/F7: latch the death record + drain IPC waiters with NO
    // scheduler lock held (publish_task_death ENSUREs this — the wake
    // path retakes the lock on the not-own-CPU branch).
    publish_task_death(task, cause);

    // M1 scope B: successor selection + deferred-switch arm under a fresh
    // lock (peek helpers read scheduler state).  The peek runs after the
    // publish, so newly-readied waiters/supervisors are visible to it.
    {
        SchedulerLockGuard guard(scheduler_lock_);
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
    // Issue #513: empty-runq self-termination must not park current on
    // the corpse.  When nothing else is runnable (peek null, or only the
    // exiting task itself), fall back to the own CPU's idle — mirroring
    // switch_away_from_terminating — so the deferred switch always has a
    // live target and the trap epilogue never srets back into TERMINATED
    // code (the trampoline hlt would otherwise park the hart with
    // whatever SIE the exiting task happened to hold).
    if (next == nullptr || next == &task)
        next = own_idle();
    if (next && next != &task) {
        switch_to_task(&task, *next, nullptr);
    }
    }
    } // M1 scope B ends.
    return errors::SCHED_ERR_OK;
}

/// @brief Trap-safe termination prefix (issue #523): everything
///        terminate_err does EXCEPT the self-current switch_to_task.
///        Fault handlers (aarch64 EL0, riscv64 U-mode) run with traps masked
///        and must leave via the deferred-switch arm published by
///        switch_away_from_terminating — a direct switch_to_task from trap
///        context would publish the trap frame as the resume point.
///        The prefix orders release_zombie BEFORE wake_waiting_parent
///        (issue #593, INV-593-2): the corpse leaves the live tables before
///        any parent is readied.  Callers are the arch fault kills and the
///        sys_kill self-fatal leg (issue #593); the cause defaults to Fault
///        so the arch callers stay byte-identical.
///        The pre-existing fault-kill shape (TERMINATED + wake + switch_away)
///        skipped release_zombie, so fault-killed tasks were never
///        zombie-listed and drain_zombie_list reaped nothing (PMM/Tasks/MQ
///        leak, e.g. the pager probes).  This helper restores the full
///        prefix: dequeue, TERMINATED mark, join-note, release_zombie,
///        parent wake, reaper poke.
///        Caller contract (mirrors switch_away_from_terminating): hold no
///        scheduler lock; the task must be live (not already TERMINATED —
///        release_zombie has no idempotency guard); call
///        switch_away_from_terminating[_discard] AFTER this returns, so the
///        zombie-before-arm ordering holds (release_zombie's
///        invalidate_pending_switch_to runs before the arm is published).
errors::SchedulerError Scheduler::terminate_err_from_trap(
    TaskControlBlock &task, uint64_t exit_code,
    ipc::DeathCause cause) noexcept {
    ENSURE(validate_task_ptr(&task, "terminate_err_from_trap"));
    arch::IrqGuard irq_guard{};
    // Issue #197: same fail-closed remote-current refusal as terminate_err.
    if (is_current_on_any_cpu(&task) && &task != current_task()) [[unlikely]] {
        kernel::Logger::warn("sched: trap-terminate id=%u refused "
                             "(remote-current)",
                             static_cast<unsigned>(task.id));
        return errors::SCHED_ERR_REMOTE_CURRENT;
    }
    ENSURE(scheduler_lock_depth_own() == 0); // issue #369 T2b: no reentrant take
    // M1 scope A: termination mutations under the lock (unchanged order).
    {
        SchedulerLockGuard guard(scheduler_lock_);
        charge_exec(task); // issue #21: bill the final partial quantum
        dequeue_ready(task);
    task.state = TaskState::TERMINATED;
    task.exit_code = exit_code;
    // F4 INV-ADM-5 (issue #557): death is terminal for the admin
    // latch — clear every field (see terminate_err).  The trap funnel
    // shares the same clearing so a fault-killed parked task cannot
    // strand a stale park across slot reuse.
    __atomic_store_n(&task.admin_parked, false, __ATOMIC_RELEASE);
    __atomic_store_n(&task.admin_wake_pending, false, __ATOMIC_RELEASE);
    __atomic_store_n(&task.admin_stop_requested, false, __ATOMIC_RELEASE);
    __atomic_store_n(&task.admin_wdog_expiry_tick, 0, __ATOMIC_RELEASE);
    deadline_list_.remove(task);
    // Issue #53: join-rendezvous handoff (mirrors terminate_err).
    TaskControlBlock::thread_join_note_termination(task);
    teardown_step(SchedTeardownSteps::Terminate);
    // Zombie-list the corpse for the deferred drain (idle cleanup_step or
    // the harness's drain_zombie_list) — the step the trap kill skipped.
    // Issue #593 (INV-593-2): the release precedes the parent wake, so a
    // woken parent can never observe the corpse in all_tasks_ and free a
    // live stack via sys_waitpid's in-table branch.
    release_zombie(task);
    // Issue #217: wake a waitpid-blocked parent before switching away.
    // Plan-590 step 12 applies without a defer branch: this funnel already
    // owns the residence, so PARENT_COLLECTING parents are spared by
    // INV-WAIT-5 until their collect_zombie_child, and non-collecting
    // outcomes fall through to the ordinary zombie drain.
    const WakeResult wake = Scheduler::wake_waiting_parent(task);
    (void)wake;
    // Issue #155: prompt the PID 1 reaper (same placement as terminate_err:
    // after the list push so wake-then-miss is impossible).
    if (task.id != 1) {
        if (auto *reaper = Scheduler::find_task(1))
            reaper->notify.notify(1);
    }
    } // M1 scope A ends: lock released before the death publish.

    // M1/F6/F7: latch + drain with NO scheduler lock held.  The caller
    // (switch_away_from_terminating) arms the deferred switch AFTER this
    // returns — publish never arms, so the zombie-before-arm ordering the
    // caller contract requires is preserved.
    publish_task_death(task, cause);
    return errors::SCHED_ERR_OK;
}

void Scheduler::terminate(TaskControlBlock &task, uint64_t exit_code,
                          ipc::DeathCause cause) noexcept {
    // Void legacy wrapper: refusal is fail-closed warn-only (all existing
    // callers are own-CPU paths where refusal is unreachable). Fallible
    // callers use terminate_err directly.
    const errors::SchedulerError err = terminate_err(task, exit_code, cause);
    // Issue #324 Tick 14: refusal leg is cold (own-CPU callers never refuse).
    if (err != errors::SCHED_ERR_OK) [[unlikely]] {
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
