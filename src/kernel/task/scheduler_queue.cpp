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

/// @file scheduler_queue.cpp
/// @brief Ready-queue + EDF dispatch logic for Scheduler (issue #323).
///        Pure move out of scheduler.cpp: no logic changes.

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_queue_internal.hpp>
#include <kernel/task/scheduler_integrity.hpp>   // issue #369 T1b validate_task_ptr
#include <assert.hpp>                              // issue #369 T1b ENSURE
#include <kernel/debug/ipc_sched_trace.hpp> // CONFIG_DEBUG_IPC_SCHED
#include <kernel/arch/timer.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <lib/logger.hpp>

namespace kernel {

namespace sched_queue {

EdfReadyList edf_ready_[CONFIG_MAX_CPUS] = {};

/// @brief EDF eligibility (issue #19): explicit EDF forces the class but
///        still needs a finite deadline (fail-closed); AUTO maps
///        finite-deadline non-exempt tasks; everything else stays FIXED.
///        Issue #22: BACKGROUND-server tasks never enter EDF — they are
///        idle-time only by contract and must not preempt by deadline
///        (they dispatch at bg priority via the bitmap when nothing else
///        is ready; daemons additionally carry edf_exempt from spawn).
bool edf_eligible(const TaskControlBlock &t) noexcept {
    if (t.deadline_ticks == 0)
        return false; // I-4: untracked exclusion
    if (t.period_ticks == 0 ||
        t.period_ticks == TaskControlBlock::NO_PERIOD)
        return false;
    if (t.sched_policy == SchedPolicy::FIXED)
        return false;
    if (t.get_sporadic_server() != nullptr &&
        t.get_sporadic_server()->is_background())
        return false;
    if (t.sched_policy == SchedPolicy::EDF)
        return true;
    return !t.edf_exempt; // AUTO
}

/// @brief EDF order: earlier absolute deadline wins; ties break by higher
///        effective priority, then lower id (deterministic; same-period
///        tasks dispatch exactly as under fixed priorities).
bool edf_earlier(const TaskControlBlock &a,
                 const TaskControlBlock &b) noexcept {
    if (a.deadline_ticks != b.deadline_ticks)
        return a.deadline_ticks < b.deadline_ticks;
    uint64_t pa = Scheduler::effective_priority(&a);
    uint64_t pb = Scheduler::effective_priority(&b);
    if (pa != pb)
        return pa > pb;
    return a.id < b.id;
}

/// @brief Ordered insert into CPU c's EDF list.  Caller guarantees the task
///        is not already queued (guarded anyway — never double-link).
void edf_insert(TaskControlBlock &t, uint64_t cpu) noexcept {
    if (t.in_edf_queue_)
        return;
    EdfReadyList &l = edf_ready_[cpu % CONFIG_MAX_CPUS];
    TaskControlBlock *prev = nullptr;
    for (TaskControlBlock *c = l.head_; c != nullptr; c = c->edf_next_) {
        if (edf_earlier(t, *c))
            break;
        prev = c;
    }
    t.edf_prev_ = prev;
    if (prev != nullptr) {
        t.edf_next_ = prev->edf_next_;
        prev->edf_next_ = &t;
    } else {
        t.edf_next_ = l.head_;
        l.head_ = &t;
    }
    if (t.edf_next_ != nullptr)
        t.edf_next_->edf_prev_ = &t;
    t.in_edf_queue_ = true;
}

/// @brief Unlink from CPU c's EDF list (no-op when not queued).
void edf_remove(TaskControlBlock &t, uint64_t cpu) noexcept {
    if (!t.in_edf_queue_)
        return;
    EdfReadyList &l = edf_ready_[cpu % CONFIG_MAX_CPUS];
    if (t.edf_prev_ != nullptr)
        t.edf_prev_->edf_next_ = t.edf_next_;
    else
        l.head_ = t.edf_next_;
    if (t.edf_next_ != nullptr)
        t.edf_next_->edf_prev_ = t.edf_prev_;
    t.edf_next_ = nullptr;
    t.edf_prev_ = nullptr;
    t.in_edf_queue_ = false;
}

/// @brief Re-sort a queued EDF task after its key changed (deadline re-arm,
///        priority move).  No-op when not queued.
void edf_resort(TaskControlBlock &t) noexcept {
    if (!t.in_edf_queue_)
        return;
    uint64_t cpu = Scheduler::queue_target(t);
    edf_remove(t, cpu);
    edf_insert(t, cpu);
}

/// @brief Re-home a queued task after an eligibility change
///        (policy/exempt/period).  Caller holds scheduler_lock_.  The
///        running current task is never queued — it re-routes on its next
///        enqueue_ready.  Without this, an exempted task stays stranded in
///        the EDF list (which then evicts it as ineligible) and is never
///        dispatched again (issue #19 boot wedge).
void migrate_dispatch_queue(TaskControlBlock &t) noexcept {
    if (&t == Scheduler::current_task())
        return;
    bool was_queued = t.in_ready_queue_ || t.in_edf_queue_;
    if (t.in_ready_queue_) {
        Scheduler::rq_task(t).remove(t, t.rq_priority_);
        t.in_ready_queue_ = false;
    }
    if (t.in_edf_queue_)
        edf_remove(t, Scheduler::queue_target(t));
    if (was_queued)
        Scheduler::enqueue_ready(t);
}

/// @brief Earliest dispatchable EDF task on the calling CPU, or nullptr.
///        Lazily drops stale heads (dead/moved/ineligible), mirroring the
///        bitmap candidate loop in next_task().
TaskControlBlock *edf_peek_valid_own() noexcept {
    uint64_t cpu = Scheduler::sched_cpu();
    EdfReadyList &l = edf_ready_[cpu % CONFIG_MAX_CPUS];
    while (l.head_ != nullptr) {
        TaskControlBlock *c = l.head_;
        if (c->magic != TaskControlBlock::TCB_MAGIC || !edf_eligible(*c) ||
            (c->state != TaskState::READY &&
             c->state != TaskState::RUNNING) ||
            Scheduler::queue_target(*c) != cpu) {
            edf_remove(*c, cpu);
            continue;
        }
        return c;
    }
    return nullptr;
}

/// @brief The EDF task that should preempt the current task, or nullptr.
///        Shared by needs_switch(), next_task() and reschedule() so the
///        arm and the dispatch decision can never disagree.
TaskControlBlock *edf_preempt_candidate() noexcept {
    TaskControlBlock *e = edf_peek_valid_own();
    if (e == nullptr)
        return nullptr;
    TaskControlBlock *cur = Scheduler::current_task();
    // #345: never return current via EDF — absolute EDF precedence would
    // let a never-yielding AUTO task with earliest (expired) deadline
    // self-pick forever, starving the bitmap (harness). Fall through to
    // bitmap so lower prio READY tasks get a chance. Keep #19 semantics
    // for non-current EDF tasks (earlier deadline still preempts).
    if (e == cur)
        return nullptr;
    if (!cur || cur->magic != TaskControlBlock::TCB_MAGIC)
        return e;
    if (cur->state != TaskState::READY && cur->state != TaskState::RUNNING)
        return e;
    if (!edf_eligible(*cur))
        return e;
    if (e != cur && edf_earlier(*e, *cur))
        return e;
    return nullptr;
}

/// @brief Invalidate a pending deferred-switch arm that targets @p task_id.
///        The deferred-switch pair (save_rsp_to / load_rsp_from / load_cr3_from
///        / next_task_id / load_kstack_base-top) is published by
///        switch_to_task() and normally consumed by the same ISR epilogue.  When
///        the epilogue skips the apply (nested-ISR depth guard or the
///        generation re-check in isr_stubs.asm), the arm survives into a later
///        ISR, and the target task can be terminated + freed in between (IRQs on).
///        terminate() → release_zombie() removed the target from id_table_ but
///        never cleared the arm, so the later ISR iretq'd onto the freed task's
///        context.rsp — find_task(id)==null in scheduler_on_context_switch,
///        current-cache divergence, and the [H2W] harness displacement
///        (docs/specs/ipc.md §4, ROADMAP §v0.3.9).
///        Clearing the atoms makes ISRs entering after the invalidation skip
///        (save_rsp_to==0); bumping the generation makes an ISR that captured
///        the pre-invalidation generation fail its re-check (jne .restore).
/// @param task_id ID of the task being removed/freed.  A no-op unless the
///        pending switch targets it (a self-terminating CURRENT task's own
///        switch-away arm targets a successor, so it is preserved).
void invalidate_pending_switch_to(uint64_t task_id) noexcept {
    // Issue #25 C1: scan ALL CPUs' arms (an AP may hold an arm to a task
    // the BSP is removing, and vice versa).
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c) {
        uint64_t pending = __atomic_load_n(&scheduler_next_task_id[c],
                                           __ATOMIC_ACQUIRE);
        if (pending == UINT64_MAX || pending != task_id)
            continue;
        Scheduler::cancel_pending_switch_cpu(c);
    }
}

}  // namespace sched_queue

// Bring the EDF/list helpers into kernel scope for the member functions
// below (same unqualified call sites as the pre-move code).
using sched_queue::edf_earlier;
using sched_queue::edf_eligible;
using sched_queue::edf_insert;
using sched_queue::edf_peek_valid_own;
using sched_queue::edf_preempt_candidate;
using sched_queue::edf_ready_;
using sched_queue::edf_remove;
using sched_queue::edf_resort;
using sched_queue::invalidate_pending_switch_to;
using sched_queue::migrate_dispatch_queue;
using sched_verify::validate_task_ptr;

// ---------------------------------------------------------------------------
// Scheduler member functions — ready-queue / EDF routing (issue #323).
// ---------------------------------------------------------------------------

uint64_t Scheduler::effective_priority(const TaskControlBlock *t) noexcept {
    // FIX(sched-race): t->priority and t->sporadic_server->state_ are plain
    // (non-atomic, non-volatile) fields mutated concurrently by the timer ISR
    // (sporadic consume/replenish, deadline demote) and by task-context code
    // (IPC priority inheritance under q.lock_).  Read both as one consistent
    // snapshot.  IrqGuard is a no-op when IRQs are already disabled (ISR
    // context) and cheap otherwise; on single-core it excludes the timer ISR.
    arch::IrqGuard irq_guard{};
    if (t && t->get_sporadic_server()) {
        uintptr_t ss = reinterpret_cast<uintptr_t>(t->get_sporadic_server());
        if (!TaskControlBlock::is_kernel_pointer(
                reinterpret_cast<const void *>(ss)))
            return t->priority;
        const uint64_t *vp = reinterpret_cast<const uint64_t *>(ss);
        if (*vp == 0xDDDDDDDDDDDDDDDDULL)
            return t->priority;
        return t->get_sporadic_server()->current_priority();
    }
    return t ? t->priority : 0;
}

void Scheduler::enqueue_ready(TaskControlBlock &task) noexcept {
    ENSURE(validate_task_ptr(&task, "enqueue_ready"));
    // H2 liveness guard (ROADMAP §v0.4.0 direction #1): the scheduler must not
    // enqueue a TCB it no longer owns.  A task removed from id_table_ (via
    // release_zombie / remove_task) is dead — the harness can still re-run
    // yield_to_task()'s enqueue_ready() on it when its stored context.rsp is a
    // stale frame from a test body's setup path, recreating an orphan runq
    // node.  next_task() then returns it, the harness arms a deferred switch
    // to it, the apply-side find_task(id)==NULL drops the arm, and the harness
    // hlt-waits forever.  Refusing the enqueue neutralizes the orphan at the
    // source: a task not owned by id_table_ must never enter the runq.
    {
        auto *t = Scheduler::find_task(task.id);
        if (!t || t != &task) {
#if defined(CONFIG_DEBUG_IPC_SCHED)
            // H2 enqueue liveness audit (cold): dump the orphan source so the
            // stale-resume path that triggered it is traceable in the log.
            kernel::Logger::raw_write("[H2-ENQDEAD] id=");
            kernel::Logger::print_dec(task.id);
            kernel::Logger::raw_write(" st=");
            kernel::Logger::print_dec(static_cast<uint64_t>(task.state));
            kernel::Logger::raw_write(" inrq=");
            kernel::Logger::print_dec(task.in_ready_queue_ ? 1u : 0u);
            kernel::Logger::raw_write(" ra=0x");
            kernel::Logger::print_hex(
                reinterpret_cast<uint64_t>(__builtin_return_address(0)));
            kernel::Logger::raw_write(" tcb=0x");
            kernel::Logger::print_hex(reinterpret_cast<uint64_t>(&task));
            kernel::Logger::raw_write(" tick=");
            kernel::Logger::print_dec(arch::Timer::ticks());
            kernel::Logger::raw_write("\n");
#endif
            // Refuse the enqueue: the scheduler no longer owns this TCB.
            task.in_ready_queue_ = false;
            return;
        }
    }
    // Issue #250: wake-while-current deferral (current-never-queued).
    // A wake targeting the executing task must not physically link it —
    // the next next_task() would evict it via the candidate==current
    // branch and strand it READY with empty queues. Mark READY (unless
    // dead — TERMINATED/REAPED are never deferred, §12.1), set the flag,
    // unlink defensively (cannot happen by invariant; fail-closed), and
    // return; the deschedule paths consume the flag (switch_to_task,
    // set_current). EDF-eligible self-wakes defer identically.
    if (&task == Scheduler::current_task() &&
        Scheduler::queue_target(task) == Scheduler::sched_cpu()) {
        if (task.state != TaskState::TERMINATED &&
            task.state != TaskState::REAPED) {
            task.state = TaskState::READY;
            task.ready_deferred_ = true;
        }
        if (task.in_ready_queue_) {
            rq_for(queue_target(task)).remove(task, task.rq_priority_);
            task.in_ready_queue_ = false;
        }
        if (task.in_edf_queue_)
            edf_remove(task, queue_target(task));
        return;
    }
    Scheduler::enqueue_ready_inner(task);
}

// Issue #250: routing half of enqueue_ready (no H2 check, no deferral).
// Deschedule-side consume only (the task is leaving the CPU, so linking
// it cannot create a queued-while-current window).
void Scheduler::enqueue_ready_inner(TaskControlBlock &task) noexcept {
    ENSURE(validate_task_ptr(&task, "enqueue_ready_inner"));
    // Issue #25 C1: shared-TSS backstop — user tasks never leave CPU0 no
    // matter what the mask says (set_affinity clamps persistently; this
    // covers every other path to AP execution).
    uint64_t target = queue_target(task);
    if (task.is_user_ && target != 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Logger::warn("sched: user task id=%u clamped to CPU0", task.id);
        }
        target = 0;
    }
    // Issue #19: EDF-eligible tasks dispatch from the deadline-ordered
    // list, never the bitmap.  Single routing point — a task is in at
    // most one queue; cross-queue moves unlink first (S2 guard).
    if (edf_eligible(task)) {
        if (task.in_ready_queue_) {
            rq_for(target).remove(task, task.rq_priority_);
            task.in_ready_queue_ = false;
        }
        edf_insert(task, target);
        return;
    }
    if (task.in_edf_queue_)
        edf_remove(task, target);
    rq_for(target).enqueue(task, effective_priority(&task));
}

void Scheduler::dequeue_ready(TaskControlBlock &task) noexcept {
    ENSURE(validate_task_ptr(&task, "dequeue_ready"));
    if (task.in_edf_queue_) [[unlikely]]
        edf_remove(task, queue_target(task));
    rq_task(task).remove(task, effective_priority(&task));
    // Issue #250: leaving the queue voids any wake-while-current deferral
    // (a blocking task is no longer runnable; a later wake re-enqueues
    // normally since the task is no longer current).
    task.ready_deferred_ = false;
    // Issue #221: a task leaving the ready queue must not remain the target
    // of a pending deferred-switch arm.  The arm was published when the task
    // was READY; if the task blocks (or dies) before the epilogue applies
    // it, a later apply would spuriously resume it (observed: a waitpid
    // parent resumed with the unpatched -1 while still BLOCKED, failing an
    // already-satisfied wait).  next_task() dequeues-then-publishes, and
    // RMS clears arms first, so no live arm is harmed here — only stale
    // arms to a task that is no longer schedulable.
    invalidate_pending_switch_to(task.id);
}

// FIX(rms-o1): Explicit priority movement in the O(1) ReadyQueue.  The queue
// does NOT re-derive the node's bucket from tcb.priority — it stores nodes at
// the position where they were originally enqueued.  Whenever tcb.priority or
// effective_priority changes (PI boost, sporadic server replenish, deadline
// miss demote), the caller MUST invoke move_priority with both old and new
// values.  Failure to do so leaves the task in the wrong priority bucket,
// causing missed preemptions or priority inversion.
void Scheduler::move_priority(TaskControlBlock &task, uint64_t old_prio,
                              uint64_t new_prio) noexcept {
    ENSURE(validate_task_ptr(&task, "move_priority"));
    // Issue #19: EDF tasks are not bitmap members — re-sort the deadline
    // list instead (priority feeds the tie-break), never touch the bitmap.
    if (task.in_edf_queue_) {
        edf_resort(task);
        return;
    }
    rq_task(task).move_priority(task, old_prio, new_prio);
}

uint64_t Scheduler::queue_target(const TaskControlBlock &t) noexcept {
    uint64_t m = t.cpu_affinity ? t.cpu_affinity : 1U;
    uint64_t c = static_cast<uint64_t>(__builtin_ctzll(m));
    return c < CONFIG_MAX_CPUS ? c : 0;
}

uint64_t Scheduler::queue_depth(uint64_t cpu) noexcept {
    return rq_for(cpu).depth();
}

bool Scheduler::set_sched_policy(TaskControlBlock &task,
                                 SchedPolicy policy) noexcept {

    ENSURE(scheduler_lock_depth_own() == 0); // issue #369 T2b: no reentrant take (non-recursive)
    ENSURE(validate_task_ptr(&task, "set_sched_policy"));
    arch::IrqGuard irq_guard{};
    SchedulerLockGuard guard(scheduler_lock_);
    if (policy == SchedPolicy::EDF) {
        // Fail-closed: EDF without a finite deadline is meaningless.
        if (task.period_ticks == 0 ||
            task.period_ticks == TaskControlBlock::NO_PERIOD ||
            task.deadline_ticks == 0)
            return false;
    }
    task.sched_policy = policy;
    // Migrate queue membership to the new eligibility (issue #19).
    migrate_dispatch_queue(task);
    return true;
}

void Scheduler::set_edf_exempt(TaskControlBlock &task, bool exempt) noexcept {

    ENSURE(scheduler_lock_depth_own() == 0); // issue #369 T2b: no reentrant take (non-recursive)
    ENSURE(validate_task_ptr(&task, "set_edf_exempt"));
    arch::IrqGuard irq_guard{};
    SchedulerLockGuard guard(scheduler_lock_);
    task.edf_exempt = exempt;
    // Same migration as a policy change: an exempted task must leave the
    // EDF list for the bitmap (and vice versa), or it is stranded.
    migrate_dispatch_queue(task);
}

void Scheduler::assign_deadline_priority(TaskControlBlock &task) noexcept {
    ENSURE(validate_task_ptr(&task, "assign_deadline_priority"));
    // Deadline-monotonic: shorter relative deadline (period) maps to a
    // higher fixed priority.  Log2 compression maps the unbounded period
    // domain into the DM band; the clamp keeps idle/background (0-1) and
    // system (121-127) bands intact.  Aperiodic tasks are not assignable.
    if (task.period_ticks == 0 ||
        task.period_ticks == TaskControlBlock::NO_PERIOD)
        return;
    uint64_t lz = 0;
    for (uint64_t p = task.period_ticks; (p >> (lz + 1)) > 0 && lz < 63;
         ++lz) {
    }
    uint64_t span = CONFIG_DM_PRIO_MAX - CONFIG_DM_PRIO_MIN;
    uint64_t prio = (lz >= span)
                        ? CONFIG_DM_PRIO_MIN
                        : CONFIG_DM_PRIO_MAX - lz;
    if (prio == task.priority)
        return;
    arch::IrqGuard irq_guard{};
    ENSURE(scheduler_lock_depth_own() == 0); // issue #369 T2b: no reentrant take (non-recursive)
    SchedulerLockGuard guard(scheduler_lock_);
    task.priority = prio;
    task.base_priority = prio;
    // Re-bucket (bitmap) or re-sort (EDF tie-break) so the new value
    // takes effect immediately (FIX rms-o1 contract).
    if (task.in_edf_queue_)
        edf_resort(task);
    else if (task.in_ready_queue_)
        rq_task(task).move_priority(task, task.rq_priority_, prio);
}

void Scheduler::reset_ready_queue() noexcept {
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c)
        rq_for(c).reset();
}

void Scheduler::rebuild_ready_queue() noexcept {
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c)
        rq_for(c).reset();
    // Issue #19: EDF heads must reset exactly like the bitmap queues.
    // Re-inserting below without clearing links the same task twice
    // (stale neighbor links + fresh insert) — a list cycle that hangs
    // the timer ISR on its next scan.
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c)
        edf_ready_[c].head_ = nullptr;
    for (auto *t = all_tasks_.first_ptr(); t; t = all_tasks_.next_ptr(t)) {
        if (t->magic != TaskControlBlock::TCB_MAGIC)
            continue;
        // Clear membership first (enqueue refuses already-flagged nodes).
        // Issue #19: route through enqueue_ready so EDF-eligible tasks
        // land in the deadline list, not the bitmap (direct bitmap
        // enqueue here would corrupt dispatch).
        t->in_ready_queue_ = false;
        t->in_edf_queue_ = false;
        t->runq_next_ = nullptr;
        t->runq_prev_ = nullptr;
        t->edf_next_ = nullptr;
        t->edf_prev_ = nullptr;
        if (t->state == TaskState::READY) {
            enqueue_ready(*t);
        }
    }
}

}  // namespace kernel
