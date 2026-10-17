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

/// @file scheduler_smp.cpp
/// @brief Cross-CPU machinery for Scheduler (issue #323): deferred-switch arm
///        cancellation, AP quiesce, the wake mailbox + SCHED-IPI drain, and
///        the AP dispatch tick.  Pure move out of scheduler.cpp.

#include <kernel/task/scheduler.hpp>
#include <kernel/kernel.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/arch/apic.hpp>
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/percpu.hpp>
#endif
#include <assert.hpp>

namespace kernel {

void Scheduler::cancel_pending_switch_cpu(uint64_t cpu) noexcept {
    cpu %= CONFIG_MAX_CPUS;
    __atomic_store_n(&scheduler_load_rsp_from[cpu], (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_load_cr3_from[cpu], (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_load_tls_from[cpu], (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_next_task_id[cpu], UINT64_MAX,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_save_rsp_to[cpu], (uint64_t *)nullptr,
                     __ATOMIC_RELEASE);
    // Issue #221 (audit): the one-shot force flag belongs to the arm.
    // Clearing the arm without clearing the flag lets a later trap epilogue
    // bypass the depth/source gate for an unrelated fresh arm.
    __atomic_store_n(&scheduler_force_apply[cpu], (uint64_t)0,
                     __ATOMIC_RELEASE);
    // Bump the generation LAST (release): an ISR epilogue that captured
    // the pre-cancel generation fails its re-check and skips the apply.
    uint64_t gen = __atomic_load_n(&scheduler_switch_generation[cpu],
                                   __ATOMIC_RELAXED);
    __atomic_store_n(&scheduler_switch_generation[cpu], gen + 1,
                     __ATOMIC_RELEASE);
}

void Scheduler::quiesce_enter() noexcept {
    // Issue #197: windows nest (an outer test/harness window brackets paths
    // like set_affinity_err that quiesce internally), so count the depth
    // and clear the flag only at the outermost exit — a bool would unpark
    // the AP mid-window.
    __atomic_fetch_add(&sched_quiesce_depth_, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&sched_quiesced_, true, __ATOMIC_RELEASE);
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c)
        cancel_pending_switch_cpu(c);
#if defined(CONFIG_ARCH_X86_64)
    // Issue #26: a quiesced world must not resume masked — re-assert the
    // calling CPU's class to ACCEPT_ALL (own-CPU only, INV-TPR1).
    arch::per_cpu_current()->tpr_shadow = arch::APIC::TPR_CLASS_ACCEPT_ALL;
    arch::APIC::set_tpr_class(arch::APIC::TPR_CLASS_ACCEPT_ALL);
#endif
}

void Scheduler::quiesce_exit() noexcept {
    uint64_t depth =
        __atomic_load_n(&sched_quiesce_depth_, __ATOMIC_RELAXED);
    ENSURE(depth > 0 && "quiesce_exit without matching enter");
    if (__atomic_sub_fetch(&sched_quiesce_depth_, 1, __ATOMIC_RELEASE) == 0)
        __atomic_store_n(&sched_quiesced_, false, __ATOMIC_RELEASE);
#if defined(CONFIG_ARCH_X86_64)
    // Same re-assert on the way out: teardown paths must not leak a
    // raised class into the resumed world.
    arch::per_cpu_current()->tpr_shadow = arch::APIC::TPR_CLASS_ACCEPT_ALL;
    arch::APIC::set_tpr_class(arch::APIC::TPR_CLASS_ACCEPT_ALL);
#endif
}

void Scheduler::mailbox_publish(uint64_t cpu, TaskControlBlock &task) noexcept {
    cpu %= CONFIG_MAX_CPUS;
    // IF=0 across the publish (internal guard): a same-CPU ISR can never
    // preempt mid-publish, so the blocking leaf take below can only
    // contend with the OTHER CPU (which always progresses) — deadlock-free
    // from both task and ISR context.  Leaf-last order everywhere.
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(wake_mailbox_lock_[cpu]);
    uint64_t n = wake_mailbox_count_[cpu];
    if (n >= MAILBOX_SLOTS)
        panic("sched: wake mailbox overflow (fail-closed bound)");
    wake_mailbox_[cpu][n].task = &task;
    wake_mailbox_[cpu][n].id = task.id;
    wake_mailbox_[cpu][n].generation = task.generation;
    wake_mailbox_count_[cpu] = n + 1;
}

void Scheduler::mailbox_drain() noexcept {
    uint64_t cpu = sched_cpu() % CONFIG_MAX_CPUS;
    // Runs in the SCHED-IPI handler (IF=0).  try_lock the global (a BSP
    // task-context holder may be mid-mutation); skip on contention — the
    // entries stay queued for the next IPI/tick (never dropped here).
    // The leaf is blocking-safe (see publish: same-CPU publishers are
    // IF=0-excluded while we run).
    if (!scheduler_lock_.try_lock())
        return;
    SpinLockGuard<sync::SpinLock> outer(scheduler_lock_, adopt_lock);
    SpinLockGuard<sync::SpinLock> guard(wake_mailbox_lock_[cpu]);
    for (;;) {
        uint64_t n = wake_mailbox_count_[cpu];
        if (n == 0)
            return;
        --n;
        TaskControlBlock *task = wake_mailbox_[cpu][n].task;
        uint64_t id = wake_mailbox_[cpu][n].id;
        uint64_t generation = wake_mailbox_[cpu][n].generation;
        wake_mailbox_[cpu][n].task = nullptr;
        wake_mailbox_count_[cpu] = n;
        // Generation guard (irq_delivery waiter_gen pattern): a task
        // destroyed + block-reused between publish and drain must never
        // dispatch (UAF). Stale entries are dropped, never retried.
        if (!task || task->magic != TaskControlBlock::TCB_MAGIC ||
            task->id != id || task->generation != generation)
            continue;
        if (task->state == TaskState::TERMINATED ||
            task->state == TaskState::REAPED)
            continue;
        task->state = TaskState::READY;
        // Issue #19: route through enqueue_ready so EDF-eligible tasks
        // land in the deadline list (a direct bitmap enqueue here would
        // strand an EDF task in the wrong queue).
        task->in_ready_queue_ = false;
        task->in_edf_queue_ = false;
        task->edf_next_ = nullptr;
        task->edf_prev_ = nullptr;
        enqueue_ready(*task);
    }
}

void Scheduler::sched_ipi_handler() noexcept {
    mailbox_drain();
    __atomic_store_n(&Scheduler::SwSlots::need_resched(), true, __ATOMIC_RELEASE);
}

void Scheduler::ap_tick() noexcept {
    // AP dispatch-only tick (spec §3.4.4): quiesce parks everything;
    // otherwise run the standard decision on own state (try_lock inside;
    // loser retries next tick). No accounting/deadlines/watchdogs.
    if (__atomic_load_n(&sched_quiesced_, __ATOMIC_ACQUIRE))
        return;
    rate_monotonic_schedule();
}

}  // namespace kernel
