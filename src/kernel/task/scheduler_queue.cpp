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
#include <kernel/task/scheduler_debug_internal.hpp> // issue #582 A1 op-ring
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

/// @brief Guard-free EDF insert body (issue #602): caller holds IrqGuard.
///        No cross-list claim; internal linkage only.
static void edf_insert_irq_off(TaskControlBlock &t, uint64_t cpu) noexcept {
#if defined(CONFIG_DEBUG)
    ENSURE(!arch::interrupts_enabled());
#endif
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
    sched_debug::sched_runq_trace(sched_debug::RunqTraceTag::EdfInsert, t.id,
                                   0, 1, 0, 0, 0, cpu % CONFIG_MAX_CPUS);
}

/// @brief Guard-free EDF remove body (issue #602): caller holds IrqGuard.
static void edf_remove_irq_off(TaskControlBlock &t, uint64_t cpu) noexcept {
#if defined(CONFIG_DEBUG)
    ENSURE(!arch::interrupts_enabled());
#endif
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
    sched_debug::sched_runq_trace(sched_debug::RunqTraceTag::EdfRemove, t.id,
                                   1, 0, 0, 0, 0, cpu % CONFIG_MAX_CPUS);
}

/// @brief Ordered insert into CPU c's EDF list.  Caller guarantees the task
///        is not already queued (guarded anyway — never double-link).
///        Issue #582: own op tags + IRQ-atomic body (the EDF list is separate
///        from the ready queue with its own discipline; no cross-list claim).
///        Guard-free body lives in edf_insert_irq_off for composites (#602).
void edf_insert(TaskControlBlock &t, uint64_t cpu) noexcept {
    arch::IrqGuard irq_guard{};
    edf_insert_irq_off(t, cpu);
}

/// @brief Unlink from CPU c's EDF list (no-op when not queued).
///        Guard-free body lives in edf_remove_irq_off for composites (#602).
void edf_remove(TaskControlBlock &t, uint64_t cpu) noexcept {
    arch::IrqGuard irq_guard{};
    edf_remove_irq_off(t, cpu);
}

/// @brief Re-sort a queued EDF task after its key changed (deadline re-arm,
///        priority move).  No-op when not queued.  One IrqGuard unit for the
///        unlink→relink composite (issue #602, mirror of RQM::move_priority
///        #600 Part 4).  Pre-fix window let a same-CPU tick observe
///        flag-false/unlinked and force-drop it.
void edf_resort(TaskControlBlock &t) noexcept {
    arch::IrqGuard irq_guard{};
    if (!t.in_edf_queue_)
        return;
    uint64_t cpu = Scheduler::queue_target(t);
    edf_remove_irq_off(t, cpu);
    edf_insert_irq_off(t, cpu);
    sched_debug::sched_runq_trace(sched_debug::RunqTraceTag::EdfResort, t.id,
                                   1, 1, 0, 0, 0, cpu % CONFIG_MAX_CPUS);
}

/// @brief Bounded chain membership probe (issue #601, INV-AUDIT-4).
static bool edf_chain_contains(uint64_t cpu,
                               const TaskControlBlock &needle) noexcept {
    const EdfReadyList &l = edf_ready_[cpu % CONFIG_MAX_CPUS];
    uint64_t n = 0;
    for (TaskControlBlock *c = l.head_; c != nullptr && n < CONFIG_MAX_TASKS;
         c = c->edf_next_) {
        ++n;
        if (c->magic != TaskControlBlock::TCB_MAGIC)
            break;
        if (c == &needle)
            return true;
    }
    return false;
}

/// @brief Pending confirm slot per CPU for the two-pass audit (issue #601,
///        INV-AUDIT-3).  Written only under scheduler_lock_.
static constinit uint64_t s_edf_audit_pending_id_[CONFIG_MAX_CPUS] = {
    UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX,
    UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};

/// @brief Forced EDF membership detach (issue #582): clears flag+links as
///        one IRQ-atomic unit WITHOUT splicing — for the documented
///        whole-list reset sites where the heads are reset wholesale.
void edf_detach(TaskControlBlock &t) noexcept {
    arch::IrqGuard irq_guard{};
    if (!t.in_edf_queue_ && t.edf_next_ == nullptr && t.edf_prev_ == nullptr)
        return;
    t.in_edf_queue_ = false;
    t.edf_next_ = nullptr;
    t.edf_prev_ = nullptr;
    sched_debug::sched_runq_trace(sched_debug::RunqTraceTag::EdfRemove, t.id,
                                   1, 0, 0, 0, 0, UINT64_MAX);
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
        // Typed detach (remove clears flag+links+prio internally).
        Scheduler::rq_task(t).remove(t, t.rq_priority_);
    }
    if (t.in_edf_queue_)
        edf_remove(t, Scheduler::queue_target(t));
    if (was_queued)
        Scheduler::enqueue_ready(t);
}

/// @brief Earliest dispatchable EDF task on the calling CPU, or nullptr.
///        Lazily drops stale heads (dead/moved/ineligible), mirroring the
///        bitmap candidate loop in next_task().
///        Issue #600 Part 3 (INV-RQ-5): the walk terminates in at most
///        CONFIG_MAX_TASKS iterations (== Scheduler::
///        STRANDED_RESCUE_SCAN_MAX, private — same bound by definition;
///        CODING_STYLE §6 forbids unbounded loops).  A head whose
///        in_edf_queue_ is false cannot be spliced by edf_remove (flag
///        gate, scheduler_queue.cpp:109-110 — the exact asymmetry
///        try_unlink documents and rejects for the runq,
///        task_queue.cpp:126-141), so the old unconditional while could
///        spin forever on one stale head; it is force-dropped instead.
///        A magic-bad head never donates its links (fail-closed: the head
///        is nulled, never advanced through a possibly-wild edf_next_).
TaskControlBlock *edf_peek_valid_own() noexcept {
    uint64_t cpu = Scheduler::sched_cpu();
    EdfReadyList &l = edf_ready_[cpu % CONFIG_MAX_CPUS];
    // == Scheduler::STRANDED_RESCUE_SCAN_MAX (private member; same bound).
    constexpr uint64_t EDF_WALK_BOUND = CONFIG_MAX_TASKS;
    uint64_t guard = 0;
    while (l.head_ != nullptr && guard < EDF_WALK_BOUND) {
        ++guard;
        TaskControlBlock *c = l.head_;
        if (c->magic == TaskControlBlock::TCB_MAGIC && edf_eligible(*c) &&
            (c->state == TaskState::READY ||
             c->state == TaskState::RUNNING) &&
            Scheduler::queue_target(*c) == cpu) {
            return c;
        }
        // Stale head: drop it without stalling dispatch.
        if (c->magic != TaskControlBlock::TCB_MAGIC) {
            // Design-review S3 (issue #600 r1): never adopt the links of
            // a magic-bad node — its edf_next_ may be wild. Fail closed.
            l.head_ = nullptr;
            Scheduler::note_edf_chain_drop(c->id);
            return nullptr;
        }
        if (c->in_edf_queue_) {
            edf_remove(*c, cpu);
            continue;
        }
        // Flag-gated edf_remove is a no-op here (the non-advancing
        // shape): force the head forward without following the stale
        // node's links, then clear the node itself.
        l.head_ = c->edf_next_;
        if (l.head_ != nullptr)
            l.head_->edf_prev_ = nullptr;
        c->edf_next_ = nullptr;
        c->edf_prev_ = nullptr;
        Scheduler::note_edf_chain_drop(c->id);
    }
    if (l.head_ != nullptr) {
        // Bound hit with a live head: the chain cycles (or exceeds the
        // task-set size). Fail closed but not dead: bounded second walk
        // clearing every reachable valid node, then null the head. READY
        // tasks are re-established by the Part-2 sweep on the next tick.
        uint64_t nuke_guard = 0;
        TaskControlBlock *n = l.head_;
        while (n != nullptr && nuke_guard < EDF_WALK_BOUND) {
            ++nuke_guard;
            if (n->magic != TaskControlBlock::TCB_MAGIC)
                break;
            TaskControlBlock *nx = n->edf_next_;
            n->edf_next_ = nullptr;
            n->edf_prev_ = nullptr;
            n->in_edf_queue_ = false;
            n = nx;
        }
        l.head_ = nullptr;
        Scheduler::note_edf_chain_cycle();
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
using sched_queue::edf_detach;
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
    // F4 INV-ADM-1 layer b (issue #557): structural owning refusal.
    // The scheduler OWNS this TCB (unlike the H2 orphan above), so the
    // refusal restores BLOCKED and latches the wake — the one
    // intentional deviation from INV-RQ-2's byte-identical refusal.
    // Exists so a missed absorb site can never strand a READY-queued
    // suspended task.  Must-read-0 outside the dedicated negative
    // test (adm_wake_backstop_count_).
    if (__atomic_load_n(&task.admin_parked, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&task.admin_wake_pending, true,
                         __ATOMIC_RELEASE);
        task.state = TaskState::BLOCKED;
        Scheduler::note_block_transition("sched_queue_adm_refuse", &task);
        if (task.in_ready_queue_) {
            rq_for(queue_target(task)).remove(task, task.rq_priority_);
        }
        if (task.in_edf_queue_) {
            edf_remove(task, queue_target(task));
        }
        __atomic_fetch_add(&adm_wake_backstop_count_, 1ULL,
                           __ATOMIC_RELAXED);
#if defined(CONFIG_DEBUG)
        Logger::warn("sched: admin backstop refused link id=%u",
                     static_cast<unsigned>(task.id));
#endif
        return;
    }
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
            // INV-RQ-2: the refusal leaves the TCB byte-identical (no
            // defensive flag write — the TCB is not owned here).
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
        // F4 INV-ADM-1 layer a (issue #557): absorb before the READY
        // write below — a parked current is impossible by construction
        // (park dequeues), so this fires only on a missed-site bug.
        if (Scheduler::admin_absorb_wake(task)) {
            return;
        }
        if (task.state != TaskState::TERMINATED &&
            task.state != TaskState::REAPED) {
            task.state = TaskState::READY;
            task.ready_deferred_ = true;
        }
        // Typed detach (remove clears flag+links+prio internally).
        if (task.in_ready_queue_) {
            rq_for(queue_target(task)).remove(task, task.rq_priority_);
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
    // F4 INV-ADM-1 layer b (issue #557): same owning refusal as
    // enqueue_ready — the routing half is reachable without passing
    // through the H2/deferral gates above.
    if (__atomic_load_n(&task.admin_parked, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&task.admin_wake_pending, true,
                         __ATOMIC_RELEASE);
        task.state = TaskState::BLOCKED;
        Scheduler::note_block_transition("sched_queue_adm_refuse_inner",
                                         &task);
        if (task.in_ready_queue_) {
            rq_for(queue_target(task)).remove(task, task.rq_priority_);
        }
        if (task.in_edf_queue_) {
            edf_remove(task, queue_target(task));
        }
        __atomic_fetch_add(&adm_wake_backstop_count_, 1ULL,
                           __ATOMIC_RELAXED);
#if defined(CONFIG_DEBUG)
        Logger::warn("sched: admin backstop refused link id=%u",
                     static_cast<unsigned>(task.id));
#endif
        return;
    }
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
        // Typed detach (remove clears flag+links+prio internally).
        if (task.in_ready_queue_) {
            rq_for(target).remove(task, task.rq_priority_);
        }
        edf_insert(task, target);
        return;
    }
    if (task.in_edf_queue_)
        edf_remove(task, target);
    // Typed commit: refusals are counted + reported, never silent.
    Scheduler::note_runq_enqueue_outcome(
        rq_for(target).enqueue(task, effective_priority(&task)), task,
        target);
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
        // Issue #582: typed detaches (runq flag+links+prio as one unit;
        // EDF flag+links without splicing — the heads above were nulled
        // wholesale) instead of raw field writes.
        rq_task(*t).detach(*t);
        edf_detach(*t);
        sched_debug::sched_runq_trace(
            sched_debug::RunqTraceTag::RebuildLevel, t->id, 0, 0, 0, 0, 0,
            queue_target(*t));
        if (t->state == TaskState::READY) {
            enqueue_ready(*t);
        }
    }
}

namespace {
// One-shot latch for the invalid-enqueue ERROR (issue #582 §5.5).
bool s_enqueue_invalid_warned = false;
} // namespace

void Scheduler::note_runq_enqueue_outcome(
    ReadyQueueManager::RunqEnqueueResult result, const TaskControlBlock &tcb,
    uint64_t prio) noexcept {
    using Outcome = ReadyQueueManager::RunqEnqueueResult;
    if (result == Outcome::Linked)
        return;
    if (result == Outcome::AlreadyQueued) {
        note_rqm_enqueue_refused_already();
#if defined(CONFIG_DEBUG)
        // DEBUG membership assert (issue #600 Part 1): an already-queued
        // node must be physically present somewhere, or the flag/link
        // pair desynced (the sweep then rescues + counts it as an
        // orphan).  The pre-#600 scan reused one index as CPU *and*
        // level (rq_for masks q % CONFIG_MAX_CPUS), so it misjudged
        // every legitimately queued task at a level with
        // level % CONFIG_MAX_CPUS != cpu — hundreds of false ERRORs on
        // passing runs.  The corrected scan never reuses an index.
        bool phys = false;
        for (uint64_t cpu = 0; cpu < CONFIG_MAX_CPUS && !phys; ++cpu) {
            for (uint64_t lvl = 0;
                 lvl <= CONFIG_PRIORITY_CEILING && !phys; ++lvl) {
                if (rq_for(cpu).queue(lvl).contains(tcb))
                    phys = true;
            }
        }
        if (!phys) {
            const uint64_t rescue_no =
                __atomic_fetch_add(&runq_nowhere_linked_count_, 1ULL,
                                   __ATOMIC_RELAXED) +
                1ULL;
            if (rescue_no == 1 ||
                (rescue_no % STRANDED_RESCUE_WARN_EVERY) == 0) {
                kernel::Logger::error(
                    "[RUNQ] already-queued but nowhere linked id=%lu (n=%lu)",
                    tcb.id, rescue_no);
                if (rescue_no == 1) {
                    sched_debug::sched_runq_trace_dump(tcb.id, 8);
                }
            }
        }
#else
        (void)tcb;
        (void)prio;
#endif
        return;
    }
    // RefusedInvalid: the TCB is byte-identical (API guarantee); count +
    // one-shot ERROR with caller ra; the tick sweep rescues + counts.
    note_rqm_enqueue_refused_invalid();
    if (!s_enqueue_invalid_warned) {
        s_enqueue_invalid_warned = true;
        kernel::Logger::error(
            "[RUNQ] enqueue refused (invalid TCB) id=%lu prio=%lu ra=0x%lx",
            tcb.id, prio,
            reinterpret_cast<uint64_t>(__builtin_return_address(0)));
        sched_debug::sched_runq_trace_dump(tcb.id, 8);
    }
}

void Scheduler::note_edf_chain_drop(uint64_t id) noexcept {
    const uint64_t drop_no =
        __atomic_fetch_add(&edf_chain_drop_count_, 1ULL,
                           __ATOMIC_RELAXED) +
        1ULL;
    if (drop_no == 1 || (drop_no % STRANDED_RESCUE_WARN_EVERY) == 0) {
        kernel::Logger::warn(
            "sched: EDF head not membership-marked, dropped id=%lu (n=%lu)",
            id, drop_no);
        if (drop_no == 1) {
            sched_debug::sched_runq_trace_dump(id, 8);
        }
    }
}

void Scheduler::note_edf_chain_cycle() noexcept {
    const uint64_t drop_no =
        __atomic_fetch_add(&edf_chain_drop_count_, 1ULL,
                           __ATOMIC_RELAXED) +
        1ULL;
    kernel::Logger::error("sched: EDF chain cycle, nuked (n=%lu)", drop_no);
    sched_debug::sched_runq_trace_dump(UINT64_MAX, 8);
}

void Scheduler::note_edf_unreachable(uint64_t id, uint64_t cpu) noexcept {
    const uint64_t n =
        __atomic_fetch_add(&edf_unreachable_count_, 1ULL, __ATOMIC_RELAXED) +
        1ULL;
    if (n == 1 || (n % STRANDED_RESCUE_WARN_EVERY) == 0) {
        kernel::Logger::warn(
            "sched: EDF member unreachable from CPU%lu head id=%lu (n=%lu)",
            cpu, id, n);
        if (n == 1) {
            sched_debug::sched_runq_trace_dump(id, 8);
        }
    }
}

void Scheduler::edf_membership_audit() noexcept {
    arch::IrqGuard irq_guard{};
    if (scheduler_lock_depth_own() != 0) {
        return;
    }
    if (!scheduler_lock_.try_lock()) {
        return;
    }
    SchedulerLockGuard guard(scheduler_lock_, adopt_lock);
    uint64_t scanned = 0;
    for (auto *t = all_tasks_.first_ptr(); t != nullptr && scanned < STRANDED_RESCUE_SCAN_MAX;
         t = all_tasks_.next_ptr(t)) {
        ++scanned;
        if (t->magic != TaskControlBlock::TCB_MAGIC)
            continue;
        if (!t->in_edf_queue_)
            continue;
        uint64_t cpu = Scheduler::queue_target(*t);
        if (cpu >= CONFIG_MAX_CPUS)
            continue;
        if (sched_queue::edf_chain_contains(cpu, *t) ||
            sched_queue::edf_chain_contains(cpu, *t)) {
            sched_queue::s_edf_audit_pending_id_[cpu] = UINT64_MAX;
            continue;
        }
        if (sched_queue::s_edf_audit_pending_id_[cpu] == t->id) {
            note_edf_unreachable(t->id, cpu);
            sched_queue::s_edf_audit_pending_id_[cpu] = UINT64_MAX;
        } else if (sched_queue::s_edf_audit_pending_id_[cpu] ==
                   UINT64_MAX) {
            sched_queue::s_edf_audit_pending_id_[cpu] = t->id;
        }
        // Else: a different id is already staged for this CPU — keep it
        // until confirmed+cleared; this task stages in a later period.
    }
}

}  // namespace kernel
