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

/// @file scheduler_snapshot_fields.cpp
/// @brief Task-field snapshot/restore for Scheduler (issue #323).
///        Pure move out of scheduler.cpp: no logic changes.

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_snapshot_fields_internal.hpp>
#include <kernel/task/scheduler_switch_internal.hpp> // issue #323 current_sp (charge_exec is kernel::)
#include <kernel/task/scheduler_queue_internal.hpp>  // issue #323 edf_insert
#include <kernel/task/scheduler_integrity.hpp>       // issue #323 task_stack_ptr
#include <kernel/task/tcb_write_log.hpp>             // issue #323 TCB_WRITE
#include <kernel/daemon/daemon_mgr.hpp>              // issue #323 daemon::is_managed
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <assert.hpp>

namespace kernel {

using sched_switch::current_sp;
using sched_queue::edf_insert;
using sched_verify::task_stack_ptr;
using sched_verify::validate_task_ptr;

void Scheduler::read_times(TaskControlBlock &task, TaskTimes &out) noexcept {

    ENSURE(scheduler_lock_.holder() == nullptr); // issue #369 T2b: no reentrant take (non-recursive)
    ENSURE(validate_task_ptr(&task, "read_times"));
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    if (&task == current_task())
        charge_exec(task); // charge-before-read: sample includes time to now
    out.exec_ns_total = task.exec_ns_total;
    out.exec_period_ns = task.exec_period_ns;
    out.executed_ticks = task.executed_ticks;
    out.wcet_ticks = task.wcet_ticks;
}

void Scheduler::restore_task_fields(const TaskFields *saved) {
    // Direction 1 (H2 §4.6): the harness (PID 1, the physically-running test
    // runner) must keep its ACTIVE context.rsp.  restore_task_fields otherwise
    // overwrites every task's context (incl. context.rsp) with the snapshot-time
    // value — for the harness that frame is the stale daemon-wait arch_hlt, so a
    // later deferred-switch resume of the harness iretq's the stale frame and
    // freezes the suite (the residual H2 hang).  Capture the harness's live RSP
    // (only meaningful when the harness is the current task, i.e. snapshot_restore
    // is running on it) and re-apply it after the field restore.
    TaskControlBlock *harness = Scheduler::get_harness_task();
    uint64_t harness_live_rsp = 0;
    if (harness && harness->magic == TaskControlBlock::TCB_MAGIC &&
        harness == Scheduler::current_task()) {
        harness_live_rsp = current_sp();
    }

    uint64_t t_idx = 0;
    for (auto *t = all_tasks_.first_ptr(); t; t = all_tasks_.next_ptr(t)) {
        // NOTE: t->magic is NOT checked here.  If a TCB's block was freed
        // during the test (MemPool::free fills with 0xDD under CONFIG_DEBUG),
        // and the MemPool bitmap is restored by snapshot_restore (making the
        // block allocated again), the TCB's fields are stale.  Fall back to
        // position-based match when both magic and id are corrupted.
        for (uint64_t j = 0; j < MAX_TASKS; ++j) {
            if (saved[j].magic != TaskControlBlock::TCB_MAGIC)
                continue;
            if (saved[j].id != t->id && j != t_idx)
                continue;
            if (saved[j].id != t->id && t->magic == TaskControlBlock::TCB_MAGIC)
                continue; // valid TCB with non-matching ID — keep searching
            TCB_WRITE(t, magic, saved[j].magic);
            TCB_WRITE(t, id, saved[j].id);
            t->parent_id = saved[j].parent_id;
            TCB_WRITE(t, state, saved[j].state);
            t->priority = saved[j].priority;
            t->base_priority = saved[j].base_priority;
            t->period_ticks = saved[j].period_ticks;
            t->deadline_ticks = saved[j].deadline_ticks;
            t->deadline_missed = saved[j].deadline_missed;
            t->deadline_miss_count = saved[j].deadline_miss_count;
            t->executed_ticks = saved[j].executed_ticks;
            t->remaining_ticks = saved[j].remaining_ticks;
            t->exec_window_start_tick = saved[j].exec_window_start_tick;
            t->exit_code = saved[j].exit_code;
            t->context = saved[j].context;
            // Direction 1: keep the harness's ACTIVE context.rsp (see above).
            // Per audits/deep-analysis-h2-ssdeadline-v0.3.9.md §4.6/§5: the
            // harness IS a legitimate deferred-switch resume target (it is
            // switched away to dispatch test tasks and must be switched back),
            // and its context.rsp must stay a proper ISR-style frame — attempts
            // to re-point or invalidate it (live-RSP binding, zeroing) were
            // TESTED AND REVERTED as they regressed the race.  The live-RSP
            // re-apply below is the only kept hardening.
            if (t == harness && harness_live_rsp != 0) {
                task_stack_ptr(t) = harness_live_rsp;
            }
            TCB_WRITE(t, kernel_stack,
                      reinterpret_cast<uint8_t *>(saved[j].kernel_stack));
            t->kernel_stack_top = saved[j].kernel_stack_top;
            t->pending_signals = saved[j].pending_signals;
            t->alarm_ticks = saved[j].alarm_ticks;
            t->alarm_armed = saved[j].alarm_armed;
            t->recv_timeout_armed = saved[j].recv_timeout_armed;
            t->recv_timed_out = saved[j].recv_timed_out;
            t->sleep_armed = saved[j].sleep_armed;
            t->sleep_expired = saved[j].sleep_expired;
            t->wdog_armed = saved[j].wdog_armed;
            t->wdog_period_ticks = saved[j].wdog_period_ticks;
            t->wdog_last_kick_tick = saved[j].wdog_last_kick_tick;
            t->wdog_expiry_tick = saved[j].wdog_expiry_tick;
            t->wdog_gen = saved[j].wdog_gen;
            // Snapshot does not capture SporadicServer state nor PMM page-table
            // pools — clear the pointer and the intrusive object list so stale
            // UAF (0xDD-poisoned block) from a restored MemPool free-list cannot
            // crash effective_priority().  detach_all_objects() only unlinks
            // (bounded, never releases): the pool restore has already rewound
            // test-allocated blocks, so calling release() here would double-free.
            // Daemon ensure_running() recreates the SporadicServer if needed.
            // Issue #275: daemon CSpaces are persistent system state
            // (pre-snapshot grants: ramdisk frames, watchdog supervision)
            // — preserve the pointer, otherwise every restore orphans
            // live grants and the next ensure mints an empty replacement.
            // Issue #360: the harness (PID 1 test runner) is persistent
            // suite state too — detaching its CNode makes every harness
            // ensure_cspace() mint a +1 CNode per test.  Kept pointers
            // stay safe: the pool rewind resets block bytes to the
            // baseline (slots empty), so no test-installed slot survives
            // into the next cycle.
            bool keep_cspace =
                daemon::is_managed(t->id) || t == harness;
            t->detach_all_objects(keep_cspace);
            t->runq_next_ = saved[j].runq_next;
            t->runq_prev_ = saved[j].runq_prev;
            t->in_ready_queue_ = saved[j].in_ready_queue;
            t->rq_priority_ = saved[j].rq_priority;
            t->edf_next_ = saved[j].edf_next;
            t->edf_prev_ = saved[j].edf_prev;
            t->in_edf_queue_ = saved[j].in_edf_queue;
            t->ready_deferred_ = saved[j].ready_deferred;
            t->sched_policy = saved[j].sched_policy;
            t->edf_exempt = saved[j].edf_exempt;
            t->cpu_affinity = saved[j].cpu_affinity;
            t->iopb_slot_ = saved[j].iopb_slot;
            t->tls_base_ = saved[j].tls_base;
            break;
        }
        ++t_idx;
    }
    // Issue #19: rebuild EDF heads from the restored membership flags.
    // Order is re-derived by ordered insert (deterministic function of the
    // keys); bitmap heads come from restore_pod(), EDF has no POD — the
    // flags are authoritative.
    for (auto *t = all_tasks_.first_ptr(); t;
         t = all_tasks_.next_ptr(t)) {
        if (t->magic != TaskControlBlock::TCB_MAGIC || !t->in_edf_queue_)
            continue;
        t->in_edf_queue_ = false; // edf_insert refuses flagged nodes
        t->edf_next_ = nullptr;
        t->edf_prev_ = nullptr;
        edf_insert(*t, queue_target(*t));
    }
}

void Scheduler::capture_task_fields(TaskFields *out) {
    uint64_t idx = 0;
    for (auto *t = all_tasks_.first_ptr(); t; t = all_tasks_.next_ptr(t)) {
        if (t->magic != TaskControlBlock::TCB_MAGIC) {
            if (idx < MAX_TASKS)
                out[idx].magic = 0;
            ++idx;
            continue;
        }
        if (idx >= MAX_TASKS)
            break;
        out[idx].magic = t->magic;
        out[idx].id = t->id;
        out[idx].parent_id = t->parent_id;
        out[idx].state = t->state;
        out[idx].priority = t->priority;
        out[idx].base_priority = t->base_priority;
        out[idx].period_ticks = t->period_ticks;
        out[idx].deadline_ticks = t->deadline_ticks;
        out[idx].deadline_missed = t->deadline_missed;
        out[idx].deadline_miss_count = t->deadline_miss_count;
        out[idx].executed_ticks = t->executed_ticks;
        out[idx].remaining_ticks = t->remaining_ticks;
        out[idx].exec_window_start_tick = t->exec_window_start_tick;
        out[idx].exit_code = t->exit_code;
        out[idx].context = t->context;
        out[idx].kernel_stack = reinterpret_cast<uint64_t>(t->kernel_stack);
        out[idx].kernel_stack_top = t->kernel_stack_top;
        out[idx].waiting_child_pid = t->waiting_child_pid;
        out[idx].waiting_child_status =
            reinterpret_cast<uint64_t>(t->waiting_child_status);
        out[idx].pending_signals = t->pending_signals;
        out[idx].alarm_ticks = t->alarm_ticks;
        out[idx].alarm_armed = t->alarm_armed;
        out[idx].recv_timeout_armed = t->recv_timeout_armed;
        out[idx].recv_timed_out = t->recv_timed_out;
        out[idx].sleep_armed = t->sleep_armed;
        out[idx].sleep_expired = t->sleep_expired;
        out[idx].wdog_armed = t->wdog_armed;
        out[idx].wdog_period_ticks = t->wdog_period_ticks;
        out[idx].wdog_last_kick_tick = t->wdog_last_kick_tick;
        out[idx].wdog_expiry_tick = t->wdog_expiry_tick;
        out[idx].wdog_gen = t->wdog_gen;
        out[idx].runq_next = t->runq_next_;
        out[idx].runq_prev = t->runq_prev_;
        out[idx].in_ready_queue = t->in_ready_queue_;
        out[idx].rq_priority = t->rq_priority_;
        out[idx].edf_next = t->edf_next_;
        out[idx].edf_prev = t->edf_prev_;
        out[idx].in_edf_queue = t->in_edf_queue_;
        out[idx].ready_deferred = t->ready_deferred_;
        out[idx].sched_policy = t->sched_policy;
        out[idx].edf_exempt = t->edf_exempt;
        out[idx].cpu_affinity = t->cpu_affinity;
        out[idx].iopb_slot = t->iopb_slot_;
        out[idx].tls_base = t->tls_base_;
        ++idx;
    }
    while (idx < MAX_TASKS)
        out[idx++].magic = 0;
}
}  // namespace kernel
