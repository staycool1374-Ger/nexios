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

/// @file scheduler_snapshot.cpp
/// @brief Test-isolation capture/restore for Scheduler (issue #323).
///        Pure move out of scheduler.cpp: no logic changes.

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_snapshot_internal.hpp>
#include <kernel/task/scheduler_queue_internal.hpp>
#include <kernel/sync/spinlock_guard.hpp>

namespace kernel {

using sched_queue::edf_ready_;

// ---------------------------------------------------------------------------
// Test-isolation helpers
// ---------------------------------------------------------------------------

void Scheduler::capture_state(TaskControlBlock **tasks_out,
                              TaskControlBlock **id_table_out,
                              uint64_t &task_count_out,
                              uint64_t &current_idx_out, uint64_t &next_id_out,
                              TaskControlBlock *&idle_out, bool &preempt_out,
                              uint64_t *rq_bitmap_hi, uint64_t *rq_bitmap_lo,
                              uint64_t *sporadic_count_out) {
    all_tasks_.capture(tasks_out, MAX_TASKS);
    __builtin_memcpy(static_cast<void *>(id_table_out),
                     static_cast<const void *>(id_table_),
                     sizeof(TaskControlBlock *) * ID_TABLE_SIZE);
    task_count_out = all_tasks_.size();
    current_idx_out = current_index();
    next_id_out = __atomic_load_n(&next_task_id_, __ATOMIC_RELAXED);
    idle_out = idle_task_;
    preempt_out = preempt_enabled_;
    if (rq_bitmap_hi)
        *rq_bitmap_hi = rq_for(0).bitmap().raw_hi();
    if (rq_bitmap_lo)
        *rq_bitmap_lo = rq_for(0).bitmap().raw_lo();
    if (sporadic_count_out)
        *sporadic_count_out = sporadic_task_count_;
}

void Scheduler::capture_rqpod(ReadyQueuePOD &out, uint64_t cpu) noexcept {
    rq_for(cpu).capture_pod(out);
}

void Scheduler::restore_rqpod(const ReadyQueuePOD &src, uint64_t cpu) noexcept {
    rq_for(cpu).restore_pod(src);
}

void Scheduler::capture_percpu(SchedPerCpuPod &out) noexcept {
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c) {
        out.current[c] = kernel::cpu_ctx(c).current;
        out.save_rsp_to[c] =
            reinterpret_cast<uint64_t>(scheduler_save_rsp_to[c]);
        out.load_rsp_from[c] = scheduler_load_rsp_from[c];
        out.load_cr3_from[c] = scheduler_load_cr3_from[c];
        out.load_tls_from[c] = scheduler_load_tls_from[c];
        out.next_task_id[c] = scheduler_next_task_id[c];
        out.load_kstack_base[c] = scheduler_load_kstack_base[c];
        out.load_kstack_top[c] = scheduler_load_kstack_top[c];
        out.switch_generation[c] = scheduler_switch_generation[c];
        out.need_resched[c] = scheduler_need_resched[c] ? 1 : 0;
        SpinLockGuard<sync::SpinLock> guard(wake_mailbox_lock_[c]);
        out.mbox_count[c] = wake_mailbox_count_[c];
        for (uint64_t s = 0; s < MAILBOX_SLOTS; ++s) {
            out.mbox_task[c][s] = wake_mailbox_[c][s].task;
            out.mbox_id[c][s] = wake_mailbox_[c][s].id;
            out.mbox_generation[c][s] = wake_mailbox_[c][s].generation;
        }
    }
}

void Scheduler::restore_percpu(const SchedPerCpuPod &src) noexcept {
    // BSP current ([0]) is owned by the RSP-match re-identification
    // (restore_state) — never overwrite it here.
    for (uint64_t c = 1; c < CONFIG_MAX_CPUS; ++c) {
        TaskControlBlock *t = src.current[c];
        if (t && t->magic == TaskControlBlock::TCB_MAGIC &&
            id_table_find(t->id) == t) {
            kernel::cpu_ctx(c).current = t;
        } else if (idle_tasks_[c] &&
                   idle_tasks_[c]->magic == TaskControlBlock::TCB_MAGIC) {
            kernel::cpu_ctx(c).current = idle_tasks_[c];
        }
        scheduler_load_rsp_from[c] = src.load_rsp_from[c];
        scheduler_load_cr3_from[c] = src.load_cr3_from[c];
        scheduler_load_tls_from[c] = src.load_tls_from[c];
        scheduler_next_task_id[c] = src.next_task_id[c];
        scheduler_load_kstack_base[c] = src.load_kstack_base[c];
        scheduler_load_kstack_top[c] = src.load_kstack_top[c];
        scheduler_switch_generation[c] = src.switch_generation[c];
        scheduler_need_resched[c] = src.need_resched[c] != 0;
        uint64_t *save = reinterpret_cast<uint64_t *>(src.save_rsp_to[c]);
        scheduler_save_rsp_to[c] = save;
        // Mailbox is CLEARED (spec §3.4.5): pending cross-CPU wakes do
        // not cross test boundaries (a test publishing then snapshotting
        // is a test bug — the wake would target a rewound world).
        SpinLockGuard<sync::SpinLock> guard(wake_mailbox_lock_[c]);
        wake_mailbox_count_[c] = 0;
        for (uint64_t s = 0; s < MAILBOX_SLOTS; ++s)
            wake_mailbox_[c][s].task = nullptr;
    }
    // BSP atoms are cleared by restore_state's existing clear-all loop.
}


void Scheduler::restore_state(TaskControlBlock *const *tasks_in,
                              TaskControlBlock *const *id_table_in,
                              uint64_t task_count_in, uint64_t current_idx_in,
                              uint64_t next_id_in, TaskControlBlock *idle_in,
                              bool preempt_in, uint64_t rq_bitmap_hi,
                              uint64_t rq_bitmap_lo,
                              uint64_t sporadic_count_in) {
    all_tasks_.restore(tasks_in, task_count_in);
    __builtin_memcpy(static_cast<void *>(id_table_),
                     static_cast<const void *>(id_table_in),
                     sizeof(TaskControlBlock *) * ID_TABLE_SIZE);
    __atomic_store_n(&next_task_id_, next_id_in, __ATOMIC_RELAXED);
    idle_task_ = idle_in;
    preempt_enabled_ = preempt_in;
    sporadic_task_count_ = sporadic_count_in;

    // Restore current_task() from index
    set_current_index(current_idx_in);

    // Ready-queue state is restored separately via restore_pod()
    (void)rq_bitmap_hi;
    (void)rq_bitmap_lo;
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c)
        rq_for(c).reset();
    // Issue #19: EDF heads are rebuilt from membership flags at the end
    // of restore_task_fields — clear here so no stale head survives.
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c)
        edf_ready_[c].head_ = nullptr;

    {
#if defined(CONFIG_ARCH_X86_64)
        uint32_t _a, _b, _c, _d;
        asm volatile("cpuid" : "=a"(_a), "=b"(_b), "=c"(_c), "=d"(_d) : "a"(0));
#endif
    }
    // Issue #25 C1: clear the BSP switch atoms (AP CPUs are handled by
    // restore_percpu, which restores their captured state; a test may
    // have armed any CPU before the snapshot).
    __atomic_store_n(&scheduler_load_rsp_from[0], (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_load_cr3_from[0], (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_load_tls_from[0], (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_next_task_id[0], UINT64_MAX,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_save_rsp_to[0], (uint64_t *)nullptr,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_need_resched[0], false, __ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_switch_generation[0], (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&isr_nesting_own(), (uint64_t)0, __ATOMIC_RELEASE);
    // Issue #242: breaker observation is policy-runtime — a restored test
    // must not inherit the previous test's watch window.
    reset_starvation_breaker();
}

void Scheduler::clear_switch_globals() noexcept {
    // Issue #25 C1: clear ALL CPUs (see above).
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c) {
        __atomic_store_n(&scheduler_load_rsp_from[c], (uint64_t)0,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&scheduler_load_cr3_from[c], (uint64_t)0,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&scheduler_load_tls_from[c], (uint64_t)0,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&scheduler_save_rsp_to[c], (uint64_t *)nullptr,
                         __ATOMIC_RELEASE);
    }
}

void Scheduler::rebuild_all_tasks() noexcept {
    all_tasks_.rebuild();
}

}  // namespace kernel
