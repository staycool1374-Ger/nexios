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
#include <kernel/log/dmesg.hpp>
#include <services/terminal/terminal.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/msr.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/spinlock_guard.hpp>
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
        // IRQs on — cleanup and free without holding any lock.
        task->cleanup();
        MemPool::free(task);
    }
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
    // IRQs on — cleanup and free without holding any lock.
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
    task->cleanup();
    MemPool::free(task);
}
/// @brief Wake a parent blocked in waitpid for a child that just terminated.
/// Mirrors the wake performed by Syscall::sys_exit, but for non-sys_exit
/// termination paths (Scheduler::terminate, deadline miss, cleanup_test_tasks,
/// EL0-fault kill — issue #217) so a parent blocked in waitpid is not left
/// waiting forever on a child that exited via a path other than sys_exit.
/// @param child The just-terminated child task.
void Scheduler::wake_waiting_parent(TaskControlBlock &child) noexcept {
    ENSURE(validate_task_ptr(&child, "wake_waiting_parent"));
    if (child.parent_id == 0)
        return;
    auto *p = Scheduler::find_task(child.parent_id);
    if (!p)
        return;
    // Issue #217: match waitpid(-1) (any child) like sys_exit does — a
    // wait-any parent must observe every child death, not just sys_exit.
    constexpr uint64_t kWaitAny = static_cast<uint64_t>(-1);
    if (p->waiting_child_pid != child.id &&
        p->waiting_child_pid != kWaitAny)
        return;

    // Deliver the child's exit status to a parent blocked in waitpid.
    if (p->waiting_child_status) {
        uint64_t old_cr3 = 0;
        bool switched = false;
        if (p->page_table_ && arch::read_cr3() != p->page_table_) {
            old_cr3 = arch::read_cr3();
            arch::write_cr3(p->page_table_);
            switched = true;
        }
        // MP-4 (SMAP): the status page is a user page pre-certified MAPPED by
        // waitpid's virt_to_phys_in_pml4 check (syscall_handlers_process.cpp).
        // The write runs with AC set (stac) — the page cannot fault (present),
        // so no recover_ip is needed.  clac restores AC for the kernel after
        // the user-page store.
        arch::stac();
        *p->waiting_child_status = child.exit_code;
        arch::clac();
        if (switched)
            arch::write_cr3(old_cr3);
        p->waiting_child_status = nullptr;
    }

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
    const bool waiter_collects = (p->state == TaskState::BLOCKED);
    if (p->state == TaskState::BLOCKED) {
        p->in_ready_queue_ = false;
        Scheduler::set_task_ready(*p);
        if (task_stack_ptr(p)) {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *stack = reinterpret_cast<uint64_t *>(task_stack_ptr(p));
            // Issue #221: override the saved RETURN VALUE (a0) with the
            // child's PID — the "saved RAX" slot on x86 is frame[0], but
            // the riscv64 trap frame returns via idx9 (OFF_A0==72 in
            // syscall_entry.S; cf. exec_into_current regs[9]).  frame[0]
            // is x1/ra: writing the PID there corrupts the return address
            // while the parent observes the unpatched -1 (WAITPID-FAILED).
            uint64_t *frame = stack;
#if defined(CONFIG_ARCH_RISCV64)
            frame[9] = child.id;
#else
            frame[0] = child.id;
#endif
        }
    }
    p->waiting_child_pid = 0;
    if (!waiter_collects) {
        p->remove_child(&child);
        child.parent_id = 0;
    }
}
// Bounded stack buffer, no locks, no heap; write_fb early-outs on null
// instance / disabled fb, so this is safe wherever the Logger line
// itself is safe (incl. the reap loop under scheduler_lock_).
void Scheduler::fb_terminated_line(const char *name, uint64_t id) noexcept {
    char line[160] = {};
    uint64_t n = 0;
    const char *p0 = "Scheduler: task '";
    while (*p0 && n < sizeof(line) - 1)
        line[n++] = *p0++;
    if (name) {
        for (uint64_t i = 0; name[i] != '\0' && i < CONFIG_TASK_NAME_LEN &&
                             n < sizeof(line) - 1;
             ++i)
            line[n++] = name[i];
    }
    const char *p1 = "' (ID=";
    while (*p1 && n < sizeof(line) - 1)
        line[n++] = *p1++;
    char rev[20];
    uint64_t rp = 0;
    uint64_t v = id;
    do {
        rev[rp++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v && rp < sizeof(rev));
    while (rp > 0 && n < sizeof(line) - 1)
        line[n++] = rev[--rp];
    const char *p2 = ") terminated";
    while (*p2 && n < sizeof(line) - 1)
        line[n++] = *p2++;
    line[n] = '\0';
    service::Terminal::write_fb(line);
    service::Terminal::write_fb("\n");
}
// ---------------------------------------------------------------------------
// reap_orphans
// ---------------------------------------------------------------------------

void Scheduler::note_termination_and_free(TaskControlBlock *t) noexcept {
    ENSURE(validate_task_ptr(t, "note_termination_and_free"));
    if (!suppress_terminated_log_) {
        Logger::info("Scheduler: task '%s' (ID=%u) terminated", t->name,
                     t->id);
        fb_terminated_line(t->name, t->id); // issue #269
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

void Scheduler::defer_kill(TaskControlBlock *task) noexcept {
    ENSURE(validate_task_ptr(task, "defer_kill"));
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
    ENSURE(scheduler_lock_.holder() == nullptr);
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
