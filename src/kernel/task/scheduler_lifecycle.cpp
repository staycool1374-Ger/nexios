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

/// @file scheduler_lifecycle.cpp
/// @brief Task disposal + zombie reaping for Scheduler (issue #323).
///        Pure move out of scheduler.cpp: no logic changes.

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_lifecycle_internal.hpp>
#include <kernel/task/scheduler_path.hpp>  // issue #324 Tick 13 teardown trace
#include <kernel/task/scheduler_queue_internal.hpp>
#include <kernel/task/scheduler_integrity.hpp>
#include <kernel/task/sporadic_server.hpp>
#include <kernel/memory/integrity.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/console/urc.hpp>
#include <services/terminal/terminal.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/msr.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/test/wait_publish_seam.hpp>
#include <lib/logger.hpp>
#include <assert.hpp>

namespace kernel {

namespace sched_lifecycle {

// P5a: Deferred-kill list.
constinit TaskControlBlock *s_deferred_kill_tasks[MAX_DEFERRED_KILLS] = {};
constinit uint64_t s_deferred_kill_count = 0;
// Issue #324 Tick 11: parallel generation snapshot (ABA guard on TCB slot
// reuse — see process_deferred_kills).  TU-local: only defer_kill and
// process_deferred_kills below use it.
static constinit uint32_t s_deferred_kill_gens[MAX_DEFERRED_KILLS] = {};

}  // namespace sched_lifecycle

using sched_lifecycle::MAX_DEFERRED_KILLS;
using sched_lifecycle::s_deferred_kill_count;
using sched_lifecycle::s_deferred_kill_gens;
using sched_lifecycle::s_deferred_kill_tasks;
using sched_queue::edf_ready_;
using sched_queue::edf_remove;
using sched_queue::invalidate_pending_switch_to;
using sched_verify::task_stack_ptr;
using sched_verify::validate_task_ptr;

void Scheduler::release_zombie(TaskControlBlock &task) noexcept {
    ENSURE(validate_task_ptr(&task, "release_zombie"));
    // Invariant: a zombie must never be in the ready queue.  terminate()
    // calls dequeue_ready() before reaching us; self-termination also
    // dequeues first.
    ENSURE(!task.in_ready_queue_);
    ENSURE(task.state == TaskState::TERMINATED);

    deadline_list_.remove(task);
    all_tasks_.remove(task);
    id_table_remove(&task);
    // H2 (docs/specs/ipc.md §4): invalidate any pending deferred-switch arm
    // targeting this task.  terminate() removes the target from id_table_
    // without clearing the switch atoms, so a surviving arm would iretq a
    // later ISR onto the task's freed stack (find_task null at apply →
    // current-cache lag → [H2W] harness displacement).  A self-terminating
    // current task's own switch-away arm targets a successor, so this is a
    // no-op for that path.
    invalidate_pending_switch_to(task.id);
    // Issue #324 Tick 5: table-ownership map — dequeue (bitmap+EDF) is
    // owned by dequeue_ready() (callers dequeue before release; the
    // ENSURE above enforces it); deadline/all_tasks/id_table removals
    // here are idempotent (membership-guarded), so the destroy() path's
    // second pass via remove_task() and the drain-pop edf_remove guards
    // are provably no-op safety nets, not double-frees.  H2 invalidation
    // is co-located with id_table removal (here + reap arm); remove_task
    // /unregister_task clear all four switch atoms instead.

    task.zombie_next_ = nullptr;
    // Issue #25 C1: list linkage under the zombie leaf lock (caller holds
    // the global lock (terminate) or runs IF=0 pre-gate (reboot teardown);
    // leaf-last order everywhere, never leaf→global).
    {
        SpinLockGuard<sync::SpinLock> zguard(zombie_lock_);
        if (zombie_tail_) {
            zombie_tail_->zombie_next_ = &task;
            zombie_tail_ = &task;
        } else {
            zombie_head_ = zombie_tail_ = &task;
        }
        __atomic_add_fetch(&zombie_count_, 1, __ATOMIC_RELAXED);
    }
    // Issue #324 Tick 13: teardown trace — tables released, list pushed.
    teardown_step(SchedTeardownSteps::Release);
}
uint64_t Scheduler::snapshot_zombies(TaskControlBlock **out,
                                     uint64_t max_out) noexcept {
    if (!out || max_out == 0)
        return 0; // Nothing to fill: empty count, not an error.
    // drain_zombie_list precedent: IrqGuard + leaf only (callers hold no
    // lock).  Read-only copy — no list surgery, no free.
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> zguard(zombie_lock_);
    uint64_t copied = 0;
    for (TaskControlBlock *task = zombie_head_;
         task && copied < max_out; task = task->zombie_next_) {
        if (task->magic != TaskControlBlock::TCB_MAGIC)
            break;
        out[copied++] = task;
    }
    return copied;
}
namespace {
// Issue #590 INV-WAIT-5: a flagged corpse is owned by its waiting parent
// and must not be freed while the owner lives.  The owner-liveness read
// is lock-free (find_task id-table read, same precedent as kill_declined
// below): a stale "absent" verdict frees a flagged corpse, but the loser
// then takes the R-1 miss leg (never a UAF — both the drain and the
// waiter's collect claim under the same zombie leaf).
bool wait_collect_spared(const TaskControlBlock &t) noexcept {
    if (!t.wait_collect_pending_)
        return false;
    auto *p = Scheduler::find_task(t.parent_id);
    return p != nullptr && p->state != TaskState::TERMINATED;
}
} // namespace

void Scheduler::flush_zombies(uint64_t max_flush) noexcept {
    // Caller: on_tick tail. NOTE (issue #369 T2a): the tail runs WITHOUT
    // the global lock (adopt scope closes before the tail); taking only
    // the leaf here is order-correct (never leaf→global). List surgery
    // under the leaf; free() runs per item with NEITHER held (two-phase
    // — a VMM-nested cleanup_step_try skips instead of deadlocking).
    for (uint64_t i = 0; i < max_flush; ++i) {
        TaskControlBlock *task;
        {
            SpinLockGuard<sync::SpinLock> zguard(zombie_lock_);
            task = zombie_head_;
            if (!task)
                break;
            // Check magic before accessing any field past offset 0.
            // Issue #324 Tick 14: corruption leg is cold.
            if (task->magic != TaskControlBlock::TCB_MAGIC) [[unlikely]] {
                zombie_head_ = nullptr;
                zombie_tail_ = nullptr;
                zombie_count_ = 0;
                continue;
            }
            zombie_head_ = task->zombie_next_;
            if (!zombie_head_)
                zombie_tail_ = nullptr;
            task->zombie_next_ = nullptr;
            // Issue #197: spare an AP-live zombie (never free a live
            // stack) — re-queue at the tail so other zombies still flush
            // within the bounded max_flush budget (no live-wait).
            if (is_current_on_any_cpu(task)) {
                task->zombie_next_ = nullptr;
                if (zombie_tail_ != nullptr) {
                    zombie_tail_->zombie_next_ = task;
                } else {
                    zombie_head_ = task;
                }
                zombie_tail_ = task;
                continue;
            }
            // Baseline-owned (pinned) zombies are never freed: the
            // snapshot owns the block and restore rewinds it (same rule
            // as TaskControlBlock::cleanup and the watchdog scan).
            // Spared like AP-live above — re-queued, budget-bounded.
            if (MemPool::is_block_pinned(task)) {
                task->zombie_next_ = nullptr;
                if (zombie_tail_ != nullptr) {
                    zombie_tail_->zombie_next_ = task;
                } else {
                    zombie_head_ = task;
                }
                zombie_tail_ = task;
                continue;
            }
            // Issue #590 INV-WAIT-5 (docs/specs/zombie-list.md §7):
            // a wait-collect-retained corpse is spared while its owner
            // lives — re-queued at the tail, budget-bounded like the
            // AP-live/pinned legs above.
            if (wait_collect_spared(*task)) {
                task->zombie_next_ = nullptr;
                if (zombie_tail_ != nullptr) {
                    zombie_tail_->zombie_next_ = task;
                } else {
                    zombie_head_ = task;
                }
                zombie_tail_ = task;
                continue;
            }
            if (task->in_ready_queue_)
                rq_task(*task).remove(*task, effective_priority(task));
            // Issue #197 defense-in-depth: unlink a stale EDF entry.
            if (task->in_edf_queue_)
                edf_remove(*task, queue_target(*task));
            __atomic_sub_fetch(&zombie_count_, 1, __ATOMIC_RELAXED);
            // Issue #324 Tick 13: drain-pop commit — free follows.
            teardown_step(SchedTeardownSteps::DrainPop);
        }
        if (task->magic == TaskControlBlock::TCB_MAGIC) {
            task->cleanup();
            MemPool::free(task);
        }
    }
}
void Scheduler::drain_zombie_list() noexcept {
    // Pop zombies under IRQ-disable + zombie leaf (ISR watchdog can't
    // race; cross-CPU pops serialize on the leaf).  No global take here
    // (callers hold none — OOM handler, snapshot, tests) so this never
    // self-deadlocks; queue-removes inherit the baseline single-core
    // exposure plus AP try-gating (spec §3.4.2).
    // Issue #197: spare AP-live zombies — a pass budget of one full list
    // length bounds rotations so a live head cannot wedge the drain.
    uint64_t spared = 0;
    const uint64_t pass_budget =
        __atomic_load_n(&zombie_count_, __ATOMIC_RELAXED) + 1;
    for (;;) {
        TaskControlBlock *task;
        {
    // Issue #26: reschedule() never raises TPR itself.  The IrqGuard
    // below stays mandatory (TPR never replaces IF=0, INV-TPR2); a
    // caller that additionally needs band masking wraps the section
    // in TprGuard (which ENSUREs IF=0 at construction).
    arch::IrqGuard irq_guard{};
            SpinLockGuard<sync::SpinLock> zguard(zombie_lock_);
            task = zombie_head_;
            if (!task)
                return;
            // Issue #324 Tick 14: corruption leg is cold.
            if (task->magic != TaskControlBlock::TCB_MAGIC) [[unlikely]] {
                zombie_head_ = nullptr;
                zombie_tail_ = nullptr;
                zombie_count_ = 0;
                continue;
            }
            if (is_current_on_any_cpu(task)) {
                // Never free a live stack: rotate head→tail (unless
                // sole, then nothing else to do) and retry on a later
                // pass — never block (§11.3).
                if (task->zombie_next_ == nullptr || zombie_tail_ == task) {
                    return;
                }
                zombie_head_ = task->zombie_next_;
                task->zombie_next_ = nullptr;
                zombie_tail_->zombie_next_ = task;
                zombie_tail_ = task;
                if (++spared >= pass_budget) {
                    return;
                }
                continue;
            }
            // Baseline-owned (pinned) zombies are never freed (see
            // flush_zombies): rotate like AP-live above, budget-bounded.
            if (MemPool::is_block_pinned(task)) {
                if (task->zombie_next_ == nullptr || zombie_tail_ == task) {
                    return;
                }
                zombie_head_ = task->zombie_next_;
                task->zombie_next_ = nullptr;
                zombie_tail_->zombie_next_ = task;
                zombie_tail_ = task;
                if (++spared >= pass_budget) {
                    return;
                }
                continue;
            }
            // Issue #590 INV-WAIT-5 (docs/specs/zombie-list.md §7):
            // a wait-collect-retained corpse is spared while its owner
            // lives — rotated like the legs above, charged to the same
            // spared/pass_budget bound.
            if (wait_collect_spared(*task)) {
                if (task->zombie_next_ == nullptr || zombie_tail_ == task) {
                    return;
                }
                zombie_head_ = task->zombie_next_;
                task->zombie_next_ = nullptr;
                zombie_tail_->zombie_next_ = task;
                zombie_tail_ = task;
                if (++spared >= pass_budget) {
                    return;
                }
                continue;
            }
            zombie_head_ = task->zombie_next_;
            if (!zombie_head_)
                zombie_tail_ = nullptr;
            task->zombie_next_ = nullptr;
            if (task->in_ready_queue_)
                rq_task(*task).remove(*task, effective_priority(task));
            // Issue #197 defense-in-depth: unlink a stale EDF entry —
            // bitmap-only removal would free a still-linked node.
            if (task->in_edf_queue_)
                edf_remove(*task, queue_target(*task));
            __atomic_sub_fetch(&zombie_count_, 1, __ATOMIC_RELAXED);
            // Issue #324 Tick 13: drain-pop commit — free follows.
            teardown_step(SchedTeardownSteps::DrainPop);
        }
        // IRQs on — cleanup and free without holding any lock. Issue #409:
        // claim ownership first; a rival-owned block is already unlisted by
        // this pop, so dropping the local pointer is leak-free — the owner
        // completes the tail and frees it.
        if (!TaskControlBlock::try_claim_teardown(task))
            continue;
        task->cleanup();
        MemPool::free(task);
    }
}
/// @brief Collect a released corpse on the waiter's behalf (issue #590,
///        second residence).  Pops the parent's child from the zombie
///        list under IrqGuard + zombie_lock_ and claims teardown in-leaf,
///        then runs the owner tail (remove_child + cleanup +
///        pinned-aware free) outside the leaf — the exact owner tail of
///        drain_zombie_list above.  Atomic pop+claim: the claim is taken
///        under the leaf so a concurrent drain either claims first (the
///        entry is never seen) or defers to us — never both.
bool Scheduler::collect_zombie_child(TaskControlBlock &parent,
                                     uint64_t target_pid,
                                     uint64_t &out_child_id,
                                     uint64_t &out_exit_code) noexcept {
    ENSURE(validate_task_ptr(&parent, "collect_zombie_child"));
    constexpr uint64_t kWaitAny = static_cast<uint64_t>(-1);
    TaskControlBlock *found = nullptr;
    {
        arch::IrqGuard irq_guard{};
        SpinLockGuard<sync::SpinLock> zguard(zombie_lock_);
        // Bounded walk: count + 1 iterations max (same budget shape as
        // the drain's pass_budget); no allocation, no unbounded loop.
        const uint64_t bound =
            __atomic_load_n(&zombie_count_, __ATOMIC_RELAXED) + 1;
        TaskControlBlock *prev = nullptr;
        TaskControlBlock *t = zombie_head_;
        for (uint64_t i = 0; i < bound && t != nullptr; ++i) {
            TaskControlBlock *next = t->zombie_next_;
            // Issue #593 (INV-593-5, #197 precedent): skip a corpse still
            // current on any CPU — its stack is live until the trap
            // epilogue applies the standing arm.  Keep walking (never abort
            // the scan): the waiter retries on the R-1 loud-deny leg.
            if (t->magic == TaskControlBlock::TCB_MAGIC &&
                !is_current_on_any_cpu(t) &&
                t->parent_id == parent.id &&
                (target_pid == kWaitAny || t->id == target_pid) &&
                !MemPool::is_block_pinned(t)) {
                if (prev) {
                    prev->zombie_next_ = next;
                } else {
                    zombie_head_ = next;
                }
                if (next == nullptr)
                    zombie_tail_ = prev;
                t->zombie_next_ = nullptr;
                __atomic_sub_fetch(&zombie_count_, 1, __ATOMIC_RELAXED);
                if (TaskControlBlock::try_claim_teardown(t)) {
                    found = t;
                } else {
                    // Lost the claim race to a concurrent drain: re-link
                    // at the tail and report not-collected (the winner
                    // owns the full tail per the #409 contract — never
                    // double-free, never leak).
                    if (zombie_tail_) {
                        zombie_tail_->zombie_next_ = t;
                    } else {
                        zombie_head_ = t;
                    }
                    zombie_tail_ = t;
                    __atomic_add_fetch(&zombie_count_, 1,
                                       __ATOMIC_RELAXED);
                    return false;
                }
                break;
            }
            prev = t;
            t = next;
        }
    }
    if (!found)
        return false;
    out_child_id = found->id;
    out_exit_code = found->exit_code;
    parent.remove_child(found);
    found->cleanup();
    if (!MemPool::is_block_pinned(found))
        MemPool::free(found);
    return true;
}
void Scheduler::cleanup_step() noexcept {
    // Pop one zombie under IRQ-disable + zombie leaf so the on_tick
    // watchdog and the other CPU's pops cannot race.  No global take
    // (callers — idle loops, tests — hold none), so VMM-nested calls
    // cannot self-deadlock; the VMM opportunistic paths use the
    // try-variant below instead.
    TaskControlBlock *task;
    {
        arch::IrqGuard irq_guard{};
        SpinLockGuard<sync::SpinLock> zguard(zombie_lock_);
        task = zombie_head_;
        if (!task)
            return;
        // Check magic before touching any field past offset 0 (may be freed).
        // Issue #324 Tick 14: corruption leg is cold.
        if (task->magic != TaskControlBlock::TCB_MAGIC) [[unlikely]] {
            zombie_head_ = nullptr;
            zombie_tail_ = nullptr;
            zombie_count_ = 0;
            return;
        }
        // Issue #197: spare an AP-live head (never free a live stack) —
        // single-pop variant: leave it listed, retry on a later pass.
        if (is_current_on_any_cpu(task)) {
            return;
        }
        // Baseline-owned (pinned) heads are never freed — leave listed.
        if (MemPool::is_block_pinned(task)) {
            return;
        }
        // Issue #590 INV-WAIT-5 (docs/specs/zombie-list.md §7): a
        // wait-collect-retained corpse is spared while its owner lives
        // — left listed, retry on a later pass (same shape as the
        // AP-live/pinned legs above).
        if (wait_collect_spared(*task)) {
            return;
        }
        zombie_head_ = task->zombie_next_;
        if (!zombie_head_)
            zombie_tail_ = nullptr;
        task->zombie_next_ = nullptr;
        if (task->in_ready_queue_)
            rq_task(*task).remove(*task, effective_priority(task));
        // Issue #197 defense-in-depth: unlink a stale EDF entry.
        if (task->in_edf_queue_)
            edf_remove(*task, queue_target(*task));
        __atomic_sub_fetch(&zombie_count_, 1, __ATOMIC_RELAXED);
            // Issue #324 Tick 13: drain-pop commit — free follows.
            teardown_step(SchedTeardownSteps::DrainPop);
        }
    // IRQs on — cleanup and free without holding any lock. Issue #409:
    // claim ownership first; a rival-owned block is already unlisted by
    // this pop, so returning early is leak-free — the owner frees it.
    if (!TaskControlBlock::try_claim_teardown(task))
        return;
    task->cleanup();
    MemPool::free(task);
}
/// @brief Opportunistic single-zombie drain for VMM teardown paths.
///        try-locks the zombie leaf and skips when contended (the zombies
///        remain for the idle loop / drain — safe to skip).  MUST be used
///        (instead of cleanup_step) anywhere that can run nested under a
///        leaf holder (flush/drain free paths); otherwise the non-recursive
///        leaf self-deadlocks.
void Scheduler::cleanup_step_try() noexcept {
    TaskControlBlock *task;
    {
        arch::IrqGuard irq_guard{};
        if (!zombie_lock_.try_lock())
            return;
        SpinLockGuard<sync::SpinLock> zguard(zombie_lock_, adopt_lock);
        task = zombie_head_;
        if (!task)
            return;
        // Issue #324 Tick 14: corruption leg is cold.
        if (task->magic != TaskControlBlock::TCB_MAGIC) [[unlikely]] {
            zombie_head_ = nullptr;
            zombie_tail_ = nullptr;
            zombie_count_ = 0;
            return;
        }
        // Issue #197: spare an AP-live head (never free a live stack).
        if (is_current_on_any_cpu(task)) {
            return;
        }
        // Baseline-owned (pinned) heads are never freed — leave listed.
        if (MemPool::is_block_pinned(task)) {
            return;
        }
        // Issue #590 INV-WAIT-5 (docs/specs/zombie-list.md §7): a
        // wait-collect-retained corpse is spared while its owner lives
        // — left listed, retry on a later pass (same shape as the
        // AP-live/pinned legs above).
        if (wait_collect_spared(*task)) {
            return;
        }
        zombie_head_ = task->zombie_next_;
        if (!zombie_head_)
            zombie_tail_ = nullptr;
        task->zombie_next_ = nullptr;
        if (task->in_ready_queue_)
            rq_task(*task).remove(*task, effective_priority(task));
        // Issue #197 defense-in-depth: unlink a stale EDF entry.
        if (task->in_edf_queue_)
            edf_remove(*task, queue_target(*task));
        __atomic_sub_fetch(&zombie_count_, 1, __ATOMIC_RELAXED);
            // Issue #324 Tick 13: drain-pop commit — free follows.
            teardown_step(SchedTeardownSteps::DrainPop);
        }
    // Issue #409: claim ownership first; a rival-owned block is already
    // unlisted by this pop, so returning early is leak-free.
    if (!TaskControlBlock::try_claim_teardown(task))
        return;
    task->cleanup();
    MemPool::free(task);
}
#if defined(CONFIG_DEBUG)
// Issue #590 step 14b: DEBUG-gated seam storage.  One TU-local hook,
// single-shot fire via exchange (two concurrent publishers cannot both
// fire), RAII scope with teardown discipline (dtor clears an unfired
// hook).  Debug-only, UP-only, one armer; no production caller arms it.
static void (*s_wait_publish_hook)(TaskControlBlock &) = nullptr;

namespace test {

void arm_wait_publish(void (*hook)(TaskControlBlock &)) noexcept {
    if (hook == nullptr)
        return;
    // UP-only, test-active only: SMP or non-test arming is ignored
    // (fail-closed — never arm silently outside the test window).
    if (Scheduler::up_cpu_count() != 1)
        return;
    if (!Scheduler::is_test_active())
        return;
    // One armer (first wins): an installed hook stays; a concurrent
    // publisher cannot double-fire (fire exchanges to null).
    void (*expected)(TaskControlBlock &) = nullptr;
    (void)__atomic_compare_exchange_n(&s_wait_publish_hook, &expected,
                                      hook, false, __ATOMIC_RELEASE,
                                      __ATOMIC_RELAXED);
}

void fire_wait_publish(TaskControlBlock &publisher) noexcept {
    // Issue #590 follow-up: run the seam hook under the same IRQ
    // discipline as the real terminate funnels — arch::IrqGuard spans
    // dequeue -> TERMINATED -> wake -> release in
    // Scheduler::terminate_err (scheduler_terminate.cpp:96, wake :152,
    // release :165) and Scheduler::terminate_err_from_trap (:254,
    // parent wake at :293).  Without it a timer tick lands mid-hook
    // and parks the BLOCKED, dequeued publisher with the wake
    // unfinished and no other waker (observed partials 1/0/0/0 and
    // 1/1/0/0).  Guard taken at entry as defense in depth: the true
    // hazard is a tick mid-hook, and entry gives the simpler
    // no-tick-from-fire-entry invariant.
    // Boundedness: leaf zombie_lock_ only; scheduler_lock_
    // unreachable on the UP arm gate via the set_affinity_err mask
    // clamp (scheduler_registry.cpp:172-180) + queue_target ctz;
    // id-table probes <= ID_TABLE_SIZE (128); the remove_child
    // sibling walk is MAX_TASKS-bounded and skipped on this path;
    // no alloc, log, IPC, or reschedule on the success path.
    arch::IrqGuard irq_guard{};
    auto *hook = __atomic_exchange_n(&s_wait_publish_hook, nullptr,
                                     __ATOMIC_ACQ_REL);
    if (hook != nullptr)
        hook(publisher);
}

WaitPublishSeamScope::WaitPublishSeamScope(
    void (*hook)(TaskControlBlock &)) noexcept
    : armed_(false) {
    if (hook == nullptr)
        return;
    arm_wait_publish(hook);
    armed_ = (__atomic_load_n(&s_wait_publish_hook, __ATOMIC_ACQUIRE) ==
              hook);
}

WaitPublishSeamScope::~WaitPublishSeamScope() noexcept {
    // Teardown discipline: clear any unfired hook so no arm crosses
    // the test boundary (a fired hook already exchanged to null).
    (void)__atomic_exchange_n(&s_wait_publish_hook, nullptr,
                              __ATOMIC_ACQ_REL);
    armed_ = false;
}

} // namespace test
#endif // defined(CONFIG_DEBUG)
/// @brief Wake a parent blocked in waitpid for a child that just terminated.
/// Mirrors the wake performed by Syscall::sys_exit, but for non-sys_exit
/// termination paths (Scheduler::terminate, deadline miss, cleanup_test_tasks,
/// EL0-fault kill — issue #217) so a parent blocked in waitpid is not left
/// waiting forever on a child that exited via a path other than sys_exit.
/// @brief Fault-armed wait-status delivery shared by all wake paths
///        (issue #588: wake_waiting_parent, sys_exit, sys_thread_exit).
///        Switches to the parent's page table, arms the single-slot
///        recovery IP around the 8-byte user store, then converges on a
///        shared tail that restores the caller's CR3 and clears the slot.
///        No lock, no allocation, lock-free dmesg only (safe from the
///        timer-tick watchdog KILL arm and IRQ-masked fault context).
bool Scheduler::deliver_wait_status(TaskControlBlock &parent,
                                    uint64_t code) noexcept {
    uint64_t *status_slot = parent.waiting_child_status;
    if (status_slot == nullptr)
        return true;
    // Single-slot recovery contract (docs/specs/external-pager.md §6):
    // save the outer arm — a nested arm (timer-tick watchdog KILL, fault
    // context) must not clobber it; the common case restores 0, so the
    // cleared-on-exit / pager-F2 precondition still holds.
    const uint64_t prev_arm = kernel::gs::user_access_recover_ip();
    bool delivered = false;
    uint64_t old_cr3 = 0;
    bool switched = false;
    if (parent.get_page_table() &&
        arch::read_cr3() != parent.get_page_table()) {
        old_cr3 = arch::read_cr3();
        arch::write_cr3(parent.get_page_table());
        switched = true;
    }
    kernel::gs::user_access_recover_ip() =
        reinterpret_cast<uint64_t>(&&recover_status);
    NEXIOS_FAULT_RECOVERY_KEEP(recover_status);
    arch::stac();
    *status_slot = code;
    arch::clac();
    delivered = true;
    kernel::gs::user_access_recover_ip() = prev_arm;
    goto converge;

recover_status:
    // S1: clac FIRST — a leaked AC panics debug/x86 at the next syscall
    // entry (syscall.cpp MP-4 detector) and hands SMAP bypass to ring 3
    // via sysret on release.  Then restore the outer arm (not zero) and
    // converge on the shared tail so the caller's CR3 is always restored.
    arch::clac();
    kernel::gs::user_access_recover_ip() = prev_arm;
    log::dmesg_push_sev(log::ErrorSubsystem::SCHED,
                        log::kDmesgBase_SCHED + 20, log::LogSeverity::WARN,
                        "Sched: waitpid status store faulted (not writable)",
                        parent.id);
converge:
    if (switched)
        arch::write_cr3(old_cr3);
    parent.waiting_child_status = nullptr;
    return delivered;
}

/// @param child The just-terminated child task.
/// @return WakeResult (issue #590): PARENT_COLLECTS when the matched
///         parent is BLOCKED (it collects the corpse itself), else the
///         non-collecting outcome.
Scheduler::WakeResult Scheduler::wake_waiting_parent(
    TaskControlBlock &child) noexcept {
    ENSURE(validate_task_ptr(&child, "wake_waiting_parent"));
    // Issue #590 INV-WAIT-3: pair the waiter's publish fence.  The child
    // state store (TERMINATED) and the waiter's publish store (pid) must
    // not both be missed via store buffering on weak models; SEQ_CST on
    // both sides forbids it (release/acquire alone does not).
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (child.parent_id == 0)
        return WakeResult::NO_WAITER;
    auto *p = Scheduler::find_task(child.parent_id);
    if (!p)
        return WakeResult::NO_WAITER;
    // Issue #217: match waitpid(-1) (any child) like sys_exit does — a
    // wait-any parent must observe every child death, not just sys_exit.
    // Single observation (issue #590): one load feeds both comparisons
    // so a concurrent publish/clear cannot half-match.
    constexpr uint64_t kWaitAny = static_cast<uint64_t>(-1);
    const uint64_t waited = p->waiting_child_pid;
    if (waited != child.id && waited != kWaitAny)
        return WakeResult::NO_WAITER;

    // Deliver the child's exit status to a parent blocked in waitpid
    // (issue #588: fault-armed shared helper — the slot was certified
    // mapped AND writable at waitpid entry, but kernel-task parents skip
    // the pre-cert and the mapping can change while the parent is BLOCKED,
    // so the store stays armed; AC cleared on both paths, slot cleared
    // inside the helper).
    if (p->waiting_child_status)
        (void)Scheduler::deliver_wait_status(*p, child.exit_code);

    // Clear the parent's wait and orphan the child for ANY waiting parent.
    // This lets reap_orphans collect the zombie even when the parent is a
    // manual-wait task (READY) that will never re-scan and reap it itself —
    // otherwise the child stays deferred forever and the scheduler deadlocks.
    // Only a BLOCKED parent is re-enqueued: a READY/WAITING parent is already
    // on the run queue, and re-enqueueing it would corrupt the intrusive
    // ready-queue links.
    // Issue #221: keep the linkage when the parent is BLOCKED-waiting — it
    // collects the zombie itself via waitpid rescan (in-trap user wait or
    // kernel self-loop).  Orphaning here would make the rescan miss
    // (parent_id cleared) and fail an already-satisfied wait.
    // Issue #590 INV-WAIT-5: the parent owns this corpse; no zombie path
    // and no deferred kill may free it while the owner lives.
    __atomic_store_n(&child.wait_collect_pending_, true, __ATOMIC_RELEASE);
    const bool waiter_collects = (p->state == TaskState::BLOCKED);
    if (p->state == TaskState::BLOCKED) {
        // Issue #582: typed paths (dequeue clears both queues +
        // deferral + stale arms; set_task_ready routes EDF-aware).
        Scheduler::dequeue_ready(*p);
        Scheduler::set_task_ready(*p);
        if (p->is_user_ && task_stack_ptr(p)) {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *stack = reinterpret_cast<uint64_t *>(task_stack_ptr(p));
            // Issue #221 keeps the override for a parent that resumes
            // through a real trap frame; issue #597 gates it on
            // is_user_ (task.hpp:669-674, authoritative since MP-1):
            // a kernel-task parent returns from waitpid by direct C++
            // return (proc_svc.cpp:390-391) and observes the cleared
            // publication + delivered status, so no patch is needed —
            // and its task_stack_ptr is the LIVE kernel stack, where
            // the store planted child.id on a live return address
            // (GDB: slot held 0x403c7e, fault fetch pc=0xa).
            // INV-597-1: write frame[0] (riscv64 frame[9], OFF_A0==72
            // in syscall_entry.S) for user parents only; riscv64
            // frame[0] is x1/ra, not the return-value slot.
            uint64_t *frame = stack;
#if defined(CONFIG_ARCH_RISCV64)
            frame[9] = child.id;
#else
            frame[0] = child.id;
#endif
        }
    }
    p->waiting_child_pid = 0;
    // Issue #53: joinable threads stay parented so a later JOIN can
    // collect them (EXIT-before-JOIN must converge); detached threads
    // orphan like regular tasks so the reaper auto-reaps them.
    if (!waiter_collects &&
        (!child.is_thread() || child.join_detached_)) {
        p->remove_child(&child);
        child.parent_id = 0;
        return WakeResult::WOKE_NOT_COLLECTING;
    }
    return waiter_collects ? WakeResult::PARENT_COLLECTS
                           : WakeResult::WOKE_NOT_COLLECTING;
}
// Bounded stack buffer, no locks, no heap; write_fb early-outs on null
// instance / disabled fb, so this is safe wherever the Logger line
// itself is safe (incl. the reap loop under scheduler_lock_).
// Issue #407 item 2: the formatter is extracted so the reap path can
// enqueue the same byte-identical line as a URC (console deferral) while
// fb_terminated_line keeps its pinned rendering (test_framebuffer pin).
static void format_terminated_line(char *out, size_t cap, const char *name,
                                   uint64_t id) {
    if (out == nullptr || cap == 0)
        return;
    uint64_t written = 0;
    const char *line_head = "Scheduler: task '";
    while (*line_head && written < cap - 1)
        out[written++] = *line_head++;
    if (name) {
        for (uint64_t name_idx = 0; name[name_idx] != '\0' &&
                                    name_idx < CONFIG_TASK_NAME_LEN &&
                                    written < cap - 1;
              ++name_idx)
            out[written++] = name[name_idx];
    }
    const char *line_mid = "' (ID=";
    while (*line_mid && written < cap - 1)
        out[written++] = *line_mid++;
    char digit_buf[20];
    uint64_t digit_pos = 0;
    uint64_t remaining = id;
    do {
        digit_buf[digit_pos++] = static_cast<char>('0' + (remaining % 10));
        remaining /= 10;
    } while (remaining && digit_pos < sizeof(digit_buf));
    while (digit_pos > 0 && written < cap - 1)
        out[written++] = digit_buf[--digit_pos];
    const char *line_tail = ") terminated";
    while (*line_tail && written < cap - 1)
        out[written++] = *line_tail++;
    out[written] = '\0';
}

void Scheduler::fb_terminated_line(const char *name, uint64_t id) noexcept {
    char line[160] = {};
    format_terminated_line(line, sizeof(line), name, id);
    service::Terminal::write_fb(line);
    service::Terminal::write_fb("\n");
}
// ---------------------------------------------------------------------------
// reap_orphans
// ---------------------------------------------------------------------------

void Scheduler::note_termination_and_free(TaskControlBlock *t) noexcept {
    ENSURE(validate_task_ptr(t, "note_termination_and_free"));
    // Issue #409: claim ownership first; on decline the reaper already
    // removed the tables above, and the owner completes the tail and
    // frees — returning early is table-clean and leak-free.
    if (!TaskControlBlock::try_claim_teardown(t))
        return;
    if (!suppress_terminated_log_) {
        // Issue #407 item 2: the console line is a deferred URC, drained
        // at the next prompt redraw.  The dmesg record (TASK+17) preserves
        // the machine-readable notice; on queue overflow the legacy
        // immediate pair keeps the line visible.  Lock order:
        // scheduler_lock_ -> urc lock (leaf: bounded copy, no calls out).
        char line[160] = {};
        format_terminated_line(line, sizeof(line), t->name, t->id);
        log::dmesg_push_sev(log::ErrorSubsystem::TASK,
                            log::kDmesgBase_TASK + 17,
                            log::LogSeverity::INFO, line, 0);
        if (!kernel::console::urc::emit(kernel::console::urc::Tag::Kill,
                                        line)) {
            Logger::info("Scheduler: task '%s' (ID=%u) terminated", t->name,
                         t->id);
            fb_terminated_line(t->name, t->id); // issue #269
        }
    }
    t->cleanup();
    // Baseline-owned (pinned) blocks are never freed (see can_reap gate
    // above and the zombie-drain paths).
    if (!MemPool::is_block_pinned(t))
        MemPool::free(t);
}

void Scheduler::reap_orphans() noexcept {
    auto *current = current_task();
    auto *init_task = idle_task_;
    TaskControlBlock *new_idle = nullptr;

    // Collect reapable tasks first (can't mutate all_tasks_ during iteration)
    static constexpr uint64_t MAX_REAP = 64;
    TaskControlBlock *to_reap[MAX_REAP];
    uint64_t num_to_reap = 0;

    for (auto *t = all_tasks_.first_ptr(); t; t = all_tasks_.next_ptr(t)) {
        if (num_to_reap >= MAX_REAP)
            break;
        // Issue #324 Tick 14: poisoned-slot leg is cold (snapshot-rewind
        // artifact, never a live task).
        if (t->magic != TaskControlBlock::TCB_MAGIC) [[unlikely]]
            continue;
        if (t->state != TaskState::TERMINATED)
            continue;
        if (!is_idle_task(t) && t == current)
            continue;

        // Adopt children to init_task via the existing intrusive child list.
        // All children are tracked in the first_child/next_sibling linked list
        // (set up in init_task_common / add_child) — no need for a full-table
        // TaskIter scan to find children by parent_id.
        if (init_task && init_task != t) {
            if (t->first_child) {
                auto *child = t->first_child;
                t->first_child = nullptr;
                if (t->num_children > 0)
                    t->num_children = 0;
                while (child) {
                    auto *next = child->next_sibling;
                    child->prev_sibling = nullptr;
                    child->next_sibling = nullptr;
                    child->parent_id = 0;
                    init_task->add_child(child);
                    child = next;
                }
            }
        }

        // Determine if reapable
        bool can_reap = (t->parent_id == 0) ||
                        (init_task && t->parent_id == init_task->id &&
                         init_task->state == TaskState::RUNNING);
        // Baseline-owned (pinned) blocks are never freed: the snapshot
        // owns the block and restore rewinds it (same rule as the
        // zombie-drain paths and TaskControlBlock::cleanup/destroy).
        // In production nothing is pinned.
        if (can_reap && MemPool::is_block_pinned(t))
            can_reap = false;
        if (!can_reap) {
            // Issue #324 Tick 12 (JRVS-SCHED-006 remainder): O(1)-ish
            // parent lookup via the id table instead of a full TaskIter
            // scan — the scan made reap O(n^2) worst case on the tick path.
            // id_table and all_tasks_ stay in sync on every add/remove path,
            // so the verdict matches the scan on all consistent states
            // (pairing e.g. release_zombie lifecycle:74-75).
            auto *p = Scheduler::id_table_find(t->parent_id);
            if (p != nullptr) {
                const bool terminated =
                    (p->state == TaskState::TERMINATED);
                // Issue #221: reap only when the parent is dead.  A live
                // parent owns its un-reaped children (Unix zombie
                // semantics): the waitpid in-trap wait re-scans on wake
                // and must find the TERMINATED child still linked —
                // reaping it first makes the rescan miss (WAITPID-FAILED
                // on an already-satisfied wait).  Fire-and-forget children
                // linger until the parent exits (bounded: parent lifetime,
                // MAX_TASKS cap); adoption above re-homes them to init on
                // the way.  A parent blocked in waitpid is covered too
                // (live ⇒ deferred regardless of waiting state).
                can_reap = terminated;
            } else {
                // Parent left the tables (dead and unregistered, or never
                // registered): the child is an orphan — reap it.
                can_reap = true;
            }
        }

        if (can_reap && t->is_thread() &&
            !TaskControlBlock::thread_join_try_claim(*t)) {
            // Issue #53: a concurrent JOIN-collect, DETACH-consume, or
            // waitpid thread-reap won this zombie — back off instead of
            // double-reaping.  A woken joiner reads its handoff latch.
            can_reap = false;
        }

        if (!can_reap)
            continue;

        // page_table_shared_ is never set (deep copy replaced shared page
        // tables) — skip the stale full-table scan.

        to_reap[num_to_reap++] = t;
    }

    // Destroy reaped tasks
    for (uint64_t ri = 0; ri < num_to_reap; ++ri) {
        auto *t = to_reap[ri];
        dequeue_ready(*t);
        all_tasks_.remove(*t);
        id_table_remove(t);
        deadline_list_.remove(*t);
        // H2: the task is leaving id_table_ — a pending deferred-switch arm to
        // it must not survive the free below (see release_zombie).
        invalidate_pending_switch_to(t->id);
        // Issue #324 Tick 13: reap-arm drain commit — note_termination frees.
        teardown_step(SchedTeardownSteps::DrainPop);
        if (t == idle_task_) {
            auto *created = TaskControlBlock::create(
                kernel::integrity::idle_task_main, 0,
                TaskControlBlock::NO_PERIOD);
            if (created) {
                created->state = TaskState::READY;
                note_termination_and_free(t);
                new_idle = created;
            } else {
                if (!suppress_terminated_log_)
                    Logger::warn("Scheduler: idle recreate OOM — keeping old idle");
                // Issue #234: failed idle restore enters the ring.
                log::dmesg_push_sev(log::ErrorSubsystem::TASK,
                                    log::kDmesgBase_TASK + 1,
                                    log::LogSeverity::ERROR,
                                    "idle recreate OOM", 0);
                all_tasks_.append(*t);
                ENSURE(id_table_insert(t->id, t) &&
                       "id_table full in reap (idle restore)");
            }
        } else {
            note_termination_and_free(t);
        }
    }

    // If idle was recreated, register it
    if (new_idle) {
        all_tasks_.append(*new_idle);
        idle_task_ = new_idle;
        ENSURE(id_table_insert(new_idle->id, new_idle) && "id_table full in reap");
    }

    // Restore current_task()
    if (current == idle_task_ && new_idle) {
        set_current_ptr(new_idle);
    } else {
        // Verify current_task() is still valid
        if (current_task() &&
            current_task()->magic != TaskControlBlock::TCB_MAGIC) {
            set_current_ptr(all_tasks_.first_ptr());
            // Issue #513 M1: reap-recovery republishes current (writer
            // table; no-op when the recovered current is a kernel task).
            republish_entry_kstack(7); // site: reap-recovery
        }
    }

}
// ---------------------------------------------------------------------------
// cleanup_test_tasks
// ---------------------------------------------------------------------------

void Scheduler::cleanup_test_tasks() noexcept {
    TaskControlBlock *const running = current_task();

    // Issue #25 C1: quiesce across teardown (flag + cancel all arms — the
    // AP cannot dispatch or drain while tasks are freed).  NO outer global
    // take (non-recursive lock; terminate()/drain_zombie_list() take it
    // per-op below).
    quiesce_enter();
    // Collect all non-idle, non-running tasks (can't mutate all_tasks_
    // during iteration: terminate → release_zombie removes from the list).
    // Spare any idle AND anything current on ANY CPU (never free a live
    // stack — an AP-running non-idle here warns as a test bug).
    static constexpr uint64_t MAX_CLEANUP = 64;
    TaskControlBlock *to_kill[MAX_CLEANUP];
    uint64_t num_to_kill = 0;
    for (auto *t = all_tasks_.first_ptr(); t && num_to_kill < MAX_CLEANUP;
         t = all_tasks_.next_ptr(t)) {
        if (is_idle_task(t) || t == running || is_current_on_any_cpu(t))
            continue;
        if (t->magic == TaskControlBlock::TCB_MAGIC)
            to_kill[num_to_kill++] = t;
    }

    // Terminate each collected task: dequeue, set TERMINATED, wake parent,
    // release_zombie (removes from all_tasks_, deadline_list_, id_table_,
    // appends to zombie list). Issue #197: race-window second gate — a
    // task collected as non-current may have been dispatched on the AP
    // since; refusal skips it (the AP-parked teardown frees it later).
    for (uint64_t i = 0; i < num_to_kill; ++i) {
        const errors::SchedulerError cleanup_err =
            terminate_err(*to_kill[i], 0);
        (void)cleanup_err;
    }

    // Drain zombies: cleanup() + MemPool::free() for each.
    drain_zombie_list();

    // Rebuild clean tables: only idle and the running task survive.
    for (uint64_t i = 0; i < ID_TABLE_SIZE; ++i)
        id_table_[i] = nullptr;
    all_tasks_.clear();
    all_tasks_.append(*idle_task_);
    ENSURE(id_table_insert(idle_task_->id, idle_task_) &&
           "id_table full in restore");
    if (running && running != idle_task_) {
        all_tasks_.append(*running);
        ENSURE(id_table_insert(running->id, running) &&
               "id_table full in restore");
    }
    set_current_ptr(idle_task_);
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c)
        rq_for(c).reset();
    // Issue #19: drop EDF heads with the bitmaps (queued tasks are dead;
    // a stale head would dangle at freed blocks).
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c)
        edf_ready_[c].head_ = nullptr;
    quiesce_exit();
}
// ---------------------------------------------------------------------------
// P5a: Deferred-kill helpers
// ---------------------------------------------------------------------------

// Issue #409: killability predicate — true when this task's teardown is
// already owned elsewhere (claimed by a rival, completed, REAPED by an
// owner running cleanup, or already removed from the registry), so a
// deferred kill must be declined, never destroyed. Shared by the arm
// (defer_kill: queue hygiene) and the process instant
// (process_deferred_kills: the necessary check — arm-time cannot see a
// teardown that starts after the arm). Same-tick scan arms never hit
// this: the owner cannot interleave inside the ISR.
static bool kill_declined(TaskControlBlock *task) noexcept {
    // Issue #590 INV-WAIT-5: a wait-collect-retained corpse is owned by
    // its waiting parent — the owner completes the tail.  Matches the
    // "Dropped, not retained" contract in process_deferred_kills below.
    return task->teardown_claimed_ || task->teardown_completed_ ||
           task->wait_collect_pending_ ||
           task->state == TaskState::REAPED ||
           Scheduler::find_task(task->id) != task;
}

void Scheduler::defer_kill(TaskControlBlock *task) noexcept {
    ENSURE(validate_task_ptr(task, "defer_kill"));
    // Issue #409 (b): do not queue a kill for a task whose teardown is
    // already owned — the owner completes it.
    if (kill_declined(task))
        return;
    // Callers: watchdog/deadline KILL arms (scan or idle-seam fire paths),
    // which may run without the global lock — append-only, lock-free;
    // global→leaf order is unaffected (no lock taken here).
    if (s_deferred_kill_count < MAX_DEFERRED_KILLS) {
        s_deferred_kill_gens[s_deferred_kill_count] = task->generation;
        s_deferred_kill_tasks[s_deferred_kill_count++] = task;
    } else {
        Logger::warn("[DMD] deferred-kill list full, task %lu not added",
                     task->id);
        // Issue #234: dropped kill enters the ring (table-full family).
        log::dmesg_push_sev(log::ErrorSubsystem::SCHED,
                            log::kDmesgBase_SCHED + 1, log::LogSeverity::ERROR,
                            "deferred-kill list full", task->id);
    }
}

void Scheduler::process_deferred_kills() noexcept {
    // Called from the on_tick gated tail (tick.cpp) with IrqGuard only:
    // the adopt scope releases scheduler_lock_ before the tail, and the
    // lock_acquired gate is freshness (the lock WAS taken this tick), not
    // held.  Lock-free is REQUIRED here, not merely allowed: destroy()
    // takes the global per item via remove_task(), whose no-reentrant-take
    // ENSURE would fire on entry under lock.  Issue #324 Tick 1: the prior
    // "under scheduler_lock_" comment was stale (same class as #369 T2b).
    ENSURE(scheduler_lock_depth_own() == 0);
    // Issue #324 Tick 11: compact-and-retain (matches the drain/flush spare
    // pattern).  Both KILL arms are one-shot — the watchdog scan disarms on
    // fire, the deadline scan skips TERMINATED — so a spared AP-live entry
    // would otherwise lose the kill until reap.  Retained entries re-check
    // the generation snapshot below (ABA guard on slot reuse).
    uint64_t kept = 0;
    for (uint64_t i = 0; i < s_deferred_kill_count; ++i) {
        auto *task = s_deferred_kill_tasks[i];
        const uint32_t gen = s_deferred_kill_gens[i];
        // Issue #324 Tick 14: stale-entry leg is cold (freed/recycled slot).
        if (!task || task->magic != TaskControlBlock::TCB_MAGIC ||
            task->generation != gen) [[unlikely]]
            continue;
        // Issue #409 (a): decline kills for tasks whose teardown is owned
        // elsewhere (seam-armed entry overtaken by owner teardown — the
        // scan path cannot hit this: the owner cannot interleave inside
        // the ISR). Dropped, not retained: the owner completes the tail.
        if (kill_declined(task)) {
            Logger::warn("[SCHED] deferred kill declined for id=%lu "
                         "(claimed=%u done=%u reaped=%u reg=%u)",
                         task->id, task->teardown_claimed_ ? 1u : 0u,
                         task->teardown_completed_ ? 1u : 0u,
                         task->state == TaskState::REAPED ? 1u : 0u,
                         Scheduler::find_task(task->id) == task ? 1u : 0u);
            continue;
        }
        // Issue #197: spare AP-live tasks (never free a live stack) —
        // retain the entry (compacted); the kill completes once the task
        // deschedules.
        if (is_current_on_any_cpu(task)) {
            s_deferred_kill_tasks[kept] = task;
            s_deferred_kill_gens[kept] = gen;
            ++kept;
            continue;
        }

        if (task->get_sporadic_server()) {
            // Contract: SporadicServer::on_completion() (and on_activation /
            // consume / process_replenishments) must NEVER release the object.
            // The ScopedRef pins it across the call; the ownership reference
            // (refcount==1) is dropped by the destroy → cleanup() →
            // release_all_objects() below.  If a future change lets the SS
            // callback trigger teardown, extend this guard to cover the
            // destroy() statement instead.
            kernel::ScopedRef ss_ref{task->get_sporadic_server()};
            task->get_sporadic_server()->on_completion(arch::Timer::ticks());
        }

        Logger::info("[DMD] Task %lu (%s) killed and cleaned up", task->id,
                     task->name);
        TaskControlBlock::destroy(task);
    }
    s_deferred_kill_count = kept;
}

}  // namespace kernel
