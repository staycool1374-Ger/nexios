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

/// @file scheduler.cpp
/// @brief Scheduler implementation: task lifecycle, ready-queue management,
///        rate-monotonic dispatch, context switching, and test isolation.

#include <kernel/task/scheduler.hpp>
#include <kernel/memory/tlb_shootdown.hpp>
#include <kernel/time/timer_wheel.hpp>
#include <kernel/time/posix_time.hpp>
#include <kernel/task/tcb_write_log.hpp>
#include <kernel/ipc/pager_registry.hpp>
#include <kernel/arch/gdt.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/msr.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/hal/iopb.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/debug/dump.hpp>
#include <kernel/debug/ipc_sched_trace.hpp>

extern "C" void debug_write(const char *s);
extern "C" void debug_write_hex(uint64_t value);
extern "C" void debug_write_dec(uint64_t value);
#include <kernel/memory/integrity.hpp>
#include <kernel/core/global_state.hpp>
#include <assert.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/test/test_isolate.hpp>
#include <kernel/test/test_watchdog.hpp>
#include <kernel/daemon/daemon_mgr.hpp>
#include <kernel/vfs/vfsd.hpp>
#include <kernel/driver/iocd.hpp>
#include <kernel/arch/apic.hpp>
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/percpu.hpp>
#endif
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/smp.hpp>
#endif
#if defined(CONFIG_ARCH_X86_64) && CONFIG_PCID
#include <kernel/arch/x86_64/hal/pcid.hpp>
#endif

/// @brief Read the current stack pointer (portable across arches).
/// @return Current SP (kernel stack pointer for the running context).
static inline uint64_t current_sp() noexcept {
#if defined(CONFIG_ARCH_X86_64)
    uint64_t sp{};
    asm volatile("mov %%rsp, %0" : "=r"(sp));
    return sp;
#elif defined(CONFIG_ARCH_AARCH64)
    uint64_t sp{};
    asm volatile("mov %0, sp" : "=r"(sp));
    return sp;
#elif defined(CONFIG_ARCH_RISCV64)
    uint64_t sp{};
    asm volatile("mv %0, sp" : "=r"(sp));
    return sp;
#else
    return 0;
#endif
}

// Boot stack (section .boot_stack, bounded by the linker's _stack_start /
// _stack_end symbols).  The harness (init, PID 1) physically runs on this
// stack in test mode — it is never switched onto its TCB kernel_stack, so no
// task's kernel_stack range covers the live RSP while the harness executes.
extern "C" {
extern char _stack_start[];
extern char _stack_end[];
}

/// @brief Validate an iret-frame candidate (audit-scheduler-tasks LOW: the
///        duplicated frame validation in switch_to_task and
///        switch_away_from_terminating).  A frame is valid only for a known
///        CS (ring0 0x8 / ring3 0x1B), with interrupts enabled in RFLAGS,
///        and — for ring0 — an RSP inside @p next's own kernel stack (or the
///        linker boot stack when dispatching the harness).  A foreign RSP
///        would iretq the task onto foreign memory (H2 hazard).
/// @param next   The task whose frame is being validated.
/// @param nbase  Next task's kernel-stack base VA.
/// @param f_rflags RFLAGS field of the candidate frame.
#if defined(CONFIG_ARCH_X86_64)
static bool validate_iret_frame(const kernel::TaskControlBlock &next,
                                uint64_t nbase, uint64_t f_rflags, uint64_t rip,
                                uint64_t cs, uint64_t rsp,
                                uint64_t ss) noexcept {
    const bool ring0 = (cs == 0x8);
    const bool ring3 = (cs == 0x1B);
    if (rip == 0 || (!ring0 && !ring3))
        return false;
    if ((ring0 || ring3) && (f_rflags & 0x2) == 0)
        return false;
    if (ring3 && (ss != 0x23 || rsp == 0))
        return false;
    if (ring0) {
        const bool in_own = rsp >= nbase && rsp <= next.kernel_stack_top;
        const bool harness_boot =
            (&next == kernel::Scheduler::get_harness_task() &&
             rsp >= reinterpret_cast<uint64_t>(_stack_start) &&
             rsp < reinterpret_cast<uint64_t>(_stack_end));
        if (!in_own && !harness_boot)
            return false;
    }
    return true;
}
#endif
#include <kernel/task/sporadic_server.hpp>

/// @brief Named constants for the iret-frame layout indices (magic-numeric
///        offsets audit-scheduler-tasks LOW: 136/8, 144/8, 152/8, 160/8,
///        168/8).  Two orderings exist: the "created" frame (rip first) and
///        the "isr_common/CPU" frame (ss first); both are validated.
namespace {
constexpr size_t IRET_RIP_IDX = 136 / 8;    ///< RIP (created order).
constexpr size_t IRET_CS_IDX = 144 / 8;     ///< CS.
constexpr size_t IRET_RFLAGS_IDX = 152 / 8; ///< RFLAGS.
constexpr size_t IRET_RSP_IDX = 160 / 8;    ///< RSP (created order).
constexpr size_t IRET_SS_IDX = 168 / 8;     ///< SS.
} // namespace
#include <signal.hpp>
#include <logger.hpp>

#if defined(CONFIG_DEBUG_IPC_SCHED)
// Wedge diagnostics for deferred-switch / ready-queue desync analysis.
// PfA-B: folded into per-CPU CpuContext debug state (design §4.A VAR-13).
#endif

namespace kernel {

// Architecture-aware stack pointer access.  Returns a reference so the
// address-of usage `&task_stack_ptr(t)` (switch_to_task's save target) keeps
// writing the live TCB field (audit-scheduler-tasks LOW: macro → inline fn).
static inline uint64_t &task_stack_ptr(TaskControlBlock *t) noexcept {
#if defined(CONFIG_ARCH_X86_64)
    return t->context.rsp;
#elif defined(CONFIG_ARCH_AARCH64)
    return t->context.sp_el0;
#elif defined(CONFIG_ARCH_RISCV64)
    return t->context.sp;
#else
    static uint64_t dummy = 0;
    (void)t;
    return dummy;
#endif
}

// Deferred-switch event codes for the H2 ring (see kernel::debug::H2Event).
inline constexpr uint64_t H2_EV_ARM      = 1;
inline constexpr uint64_t H2_EV_APPLY    = 2;
// H2_EV_SKIP and H2_EV_REENQ are now in global_state.hpp for cross-TU visibility
inline constexpr uint64_t H2_EV_CLR_RMS  = 4;
inline constexpr uint64_t H2_EV_CLR_SET  = 5;
inline constexpr uint64_t H2_EV_CLR_MISC = 6;
inline constexpr uint64_t H2_EV_IDLE_ARM = 7;

/// @brief MP-3 canary sampling for the scheduler hooks (issue #92 canary
///        relocation).  Verifies the user-segment canaries of a task that is
///        about to run (context switch) or currently running (timer tick).
///        Runs only for user tasks, only under CONFIG_CANARY_GUARD, and is a
///        pure page-table read (no locks, no allocation) — safe in ISR/tick
///        context.  Test mode latches g_canary_trip (the harness survives);
///        production panics.  This restores corruption detection for FAST
///        syscalls, which no longer consult canaries on entry.
/// @param task    Task to verify (must be magic-valid).
/// @param rip     Approximate fault RIP for the latch (0 in scheduler hooks).
/// @return true if verified (or no user task / no canary check enabled).
bool Scheduler::canary_check_in_scheduler_hooks(TaskControlBlock *task, uint64_t rip) {
#if CONFIG_CANARY_GUARD
    if (!task || !task->is_user_)
        return true;
    uint8_t bad_seg = 0;
    uint64_t bad_va = 0;
    if (!canary_verify_user_segments(task, bad_seg, bad_va)) {
        if (kernel::Scheduler::is_test_active()) {
            kernel::gs::set_canary_trip(task->id, bad_seg, rip);
            return false;
        }
        kernel::Logger::fatal(
            "CANARY TRIP (sched): task '%s' id=%u segment=%u va=0x%lx",
            task->name, static_cast<unsigned>(task->id),
            static_cast<unsigned>(bad_seg), bad_va);
        panic("software sentinel canary violated");
    }
#else
    (void)task;
    (void)rip;
#endif
    return true;
}

// ---------------------------------------------------------------------------
// Global deferred-switch event ring (H2 instrumentation, CONFIG_DEBUG).
// Extends the per-TCB debug_switch_ring[4] idiom to the GLOBAL scheduler event
// stream: every ARM / APPLY / SKIP / CLEAR of the deferred-switch pair, with
// the atoms (load_rsp_from / save_rsp_to / next_task_id) + generation + ISR
// depth.  In-memory only (no serial writes in the hot path — QEMU gdb-stub
// hardware watchpoints are broken and per-event serial output perturbs the
// race away).  Dump post-hang via GDB memory read, e.g.:
//   x/120gx &kernel::debug::g_h2_ring
// (entries indexed by g_h2_idx % H2_RING_SIZE; see h2_dump_ring()).
// ---------------------------------------------------------------------------
#ifdef CONFIG_DEBUG
namespace debug {
inline constexpr size_t H2_RING_SIZE = 512;

/// @brief One deferred-switch event.  `ev` selects the meaning of a/b/c:
///        ARM=1 (a=next.id, b=load_rsp_from, c=save_target),
///        APPLY=2 (a=applied id), SKIP=3 (a=captured gen, b=current gen),
///        CLR-RMS=4 / CLR-SET=5 / CLR-MISC=6 (a=pending next_task_id),
///        IDLE-ARM=7 (a=current id).
struct H2Event {
    uint64_t ev;
    uint64_t a;
    uint64_t b;
    uint64_t c;
    uint64_t gen;   ///< scheduler_switch_generation at record time
    uint64_t depth; ///< isr_nesting_depth at record time
};
static H2Event g_h2_ring[H2_RING_SIZE];
static uint64_t g_h2_idx = 0;

inline void h2_record(uint64_t ev, uint64_t a, uint64_t b, uint64_t c) noexcept {
    // Issue #25 C1: atomic index (two CPUs append concurrently; a torn
    // ENTRY is possible but debug-only — the generation recorded is the
    // CALLER's CPU slot, so ordering per CPU stays coherent).
    uint64_t idx =
        __atomic_fetch_add(&g_h2_idx, 1ULL, __ATOMIC_RELAXED);
    g_h2_ring[idx % H2_RING_SIZE] = {
        ev, a, b, c,
        __atomic_load_n(&Scheduler::SwSlots::generation(), __ATOMIC_RELAXED),
        __atomic_load_n(&kernel::isr_nesting_own(), __ATOMIC_RELAXED)};
}

/// @brief Dump the most recent H2_RING_SIZE events to the debug console.
void h2_dump_ring() noexcept {
    uint64_t idx = g_h2_idx;
    uint64_t start = (idx > H2_RING_SIZE) ? (idx - H2_RING_SIZE) : 0;
    for (uint64_t i = start; i < idx; ++i) {
        const H2Event &r = g_h2_ring[i % H2_RING_SIZE];
        Logger::raw_write("[H2EV] i=");
        Logger::print_dec(i);
        Logger::raw_write(" ev=");
        Logger::print_dec(r.ev);
        Logger::raw_write(" a=0x");
        Logger::print_hex(r.a);
        Logger::raw_write(" b=0x");
        Logger::print_hex(r.b);
        Logger::raw_write(" c=0x");
        Logger::print_hex(r.c);
        Logger::raw_write(" gen=");
        Logger::print_dec(r.gen);
        Logger::raw_write(" depth=");
        Logger::print_dec(r.depth);
        Logger::raw_write("\n");
    }
}
}  // namespace debug
#endif  // CONFIG_DEBUG

// H2_REC macro is now in global_state.hpp for cross-TU visibility

// P5a: Deferred-kill list.
static constexpr uint64_t MAX_DEFERRED_KILLS = 16;
static TaskControlBlock *s_deferred_kill_tasks[MAX_DEFERRED_KILLS] = {};
static uint64_t s_deferred_kill_count = 0;

// TEMP DEBUG (BUGS.md#020): detect a SporadicServer pointer that aliases a
// freed/poisoned MemPool block (first 8 bytes == 0xDDDDDDDDDDDDDDDD).  A freed
// server still referenced by a live TCB is the use-after-free behind the
// intermittent GPF (sporadic_server.hpp:114) / #PF / #NM panic in the `memory`
// class.  This converts the silent corruption into a precise, catchable fault
// naming the offending task instead of corrupting the stack with a 0xDDDD
// return address.
static inline bool is_poisoned_block(const void *p) noexcept {
#ifdef CONFIG_DEBUG
    if (!p)
        return false;
    const uint64_t *q = static_cast<const uint64_t *>(p);
    return *q == 0xDDDDDDDDDDDDDDDDULL;
#else
    (void)p;
    return false;
#endif
}

// ---------------------------------------------------------------------------
// EDF dispatch (issue #19 — global EDF with exemptions).
//
// A second dispatch queue per CPU holds EDF-eligible tasks ordered by
// absolute deadline.  A task is in at most one queue (bitmap or EDF);
// in_edf_queue_/in_ready_queue_ are authoritative.  All ops run under the
// caller's scheduler_lock_/IRQ-off discipline, mirroring the bitmap path.
// ---------------------------------------------------------------------------
/// @brief Per-CPU EDF-ready list head (intrusive, deadline-ordered).
struct EdfReadyList {
    TaskControlBlock *head_ = nullptr;
};
static EdfReadyList edf_ready_[CONFIG_MAX_CPUS];

/// @brief EDF eligibility (issue #19): explicit EDF forces the class but
///        still needs a finite deadline (fail-closed); AUTO maps
///        finite-deadline non-exempt tasks; everything else stays FIXED.
///        Issue #22: BACKGROUND-server tasks never enter EDF — they are
///        idle-time only by contract and must not preempt by deadline
///        (they dispatch at bg priority via the bitmap when nothing else
///        is ready; daemons additionally carry edf_exempt from spawn).
static bool edf_eligible(const TaskControlBlock &t) noexcept {
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
static bool edf_earlier(const TaskControlBlock &a,
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
static void edf_insert(TaskControlBlock &t, uint64_t cpu) noexcept {
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
static void edf_remove(TaskControlBlock &t, uint64_t cpu) noexcept {
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
static void edf_resort(TaskControlBlock &t) noexcept {
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
static void migrate_dispatch_queue(TaskControlBlock &t) noexcept {
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
static TaskControlBlock *edf_peek_valid_own() noexcept {
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
static TaskControlBlock *edf_preempt_candidate() noexcept {
    TaskControlBlock *e = edf_peek_valid_own();
    if (e == nullptr)
        return nullptr;
    TaskControlBlock *cur = Scheduler::current_task();
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
        if (ss < 0xFFFF800000000000ULL)
            return t->priority;
        const uint64_t *vp = reinterpret_cast<const uint64_t *>(ss);
        if (*vp == 0xDDDDDDDDDDDDDDDDULL)
            return t->priority;
        return t->get_sporadic_server()->current_priority();
    }
    return t ? t->priority : 0;
}

void Scheduler::enqueue_ready(TaskControlBlock &task) noexcept {
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
    if (task.in_edf_queue_)
        edf_remove(task, queue_target(task));
    rq_task(task).remove(task, effective_priority(&task));
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
    // Issue #19: EDF tasks are not bitmap members — re-sort the deadline
    // list instead (priority feeds the tie-break), never touch the bitmap.
    if (task.in_edf_queue_) {
        edf_resort(task);
        return;
    }
    rq_task(task).move_priority(task, old_prio, new_prio);
}

void Scheduler::set_priority(TaskControlBlock &task,
                             uint64_t new_prio) noexcept {
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    if (new_prio == task.priority)
        return;
    // Re-bucket in the O(1) ready queue if the task is queued, so the
    // effective priority change is visible on the next tick.  move_priority
    // must be called BEFORE writing the field (move uses the current bucket).
    // Issue #25 C1: takes the global lock (was IrqGuard-only) — remote
    // queues are written from task context only under the lock.
    if (task.in_ready_queue_)
        move_priority(task, task.priority, new_prio);
    task.priority = new_prio;
    task.base_priority = new_prio;
}

// ---------------------------------------------------------------------------
// Issue #25 C1: affinity, mailbox, quiesce, AP tick.
// ---------------------------------------------------------------------------

uint64_t Scheduler::queue_target(const TaskControlBlock &t) noexcept {
    uint64_t m = t.cpu_affinity ? t.cpu_affinity : 1U;
    uint64_t c = static_cast<uint64_t>(__builtin_ctzll(m));
    return c < CONFIG_MAX_CPUS ? c : 0;
}

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
    // Bump the generation LAST (release): an ISR epilogue that captured
    // the pre-cancel generation fails its re-check and skips the apply.
    uint64_t gen = __atomic_load_n(&scheduler_switch_generation[cpu],
                                   __ATOMIC_RELAXED);
    __atomic_store_n(&scheduler_switch_generation[cpu], gen + 1,
                     __ATOMIC_RELEASE);
}

void Scheduler::quiesce_enter() noexcept {
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
    __atomic_store_n(&sched_quiesced_, false, __ATOMIC_RELEASE);
#if defined(CONFIG_ARCH_X86_64)
    // Same re-assert on the way out: teardown paths must not leak a
    // raised class into the resumed world.
    arch::per_cpu_current()->tpr_shadow = arch::APIC::TPR_CLASS_ACCEPT_ALL;
    arch::APIC::set_tpr_class(arch::APIC::TPR_CLASS_ACCEPT_ALL);
#endif
}

/// @brief Number of CPUs currently up (BSP + parked/running APs).
static uint64_t sched_up_cpus() noexcept {
#if defined(CONFIG_ARCH_X86_64)
    return 1 + kernel::smp::ap_count();
#else
    return 1;
#endif
}

errors::SchedulerError
Scheduler::set_affinity_err(TaskControlBlock &task, uint64_t mask) noexcept {
    // Quiesce window fences the lock-free AP current-apply path (spec
    // §3.4.5): with arms cancelled and AP dispatch parked, the running
    // check below is stable across the dequeue+enqueue.
    arch::IrqGuard irq_guard{};
    quiesce_enter();
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    uint64_t up = sched_up_cpus();
    uint64_t allowed = (up >= 64) ? ~0ULL : ((up == 0) ? 0ULL : ((1ULL << up) - 1));
    if ((mask & allowed) == 0) {
        Logger::warn(
            "sched: set_affinity id=%u mask=0x%lx clamps to CPU0", task.id,
            mask);
        mask = 1;
    }
    mask &= allowed;
    if (task.is_user_ && mask != 1) {
        // Shared-TSS limitation (spec §3.4.5): user tasks stay on CPU0.
        static bool warned = false;
        if (!warned) {
            warned = true;
            Logger::warn(
                "sched: user task id=%u forced to CPU0 (no per-CPU TSS)",
                task.id);
        }
        mask = 1;
    }
    if (is_current_on_any_cpu(&task) && &task != current_task()) {
        Logger::warn("sched: set_affinity id=%u refused (running)", task.id);
        quiesce_exit();
        return errors::SCHED_ERR_OK; // no-op refuse: legacy success contract
    }
    if (task.cpu_affinity != mask) {
        // Issue #23: partitioned-EDF destination probe BEFORE any mutation.
        // The candidate is counted on the destination partition; denial
        // leaves mask, queues, EDF lists and tracker untouched
        // (fail-closed).  Exempt moves are always allowed.
        uint64_t m = (mask != 0) ? mask : 1U;
        uint64_t new_target =
            static_cast<uint64_t>(__builtin_ctzll(m));
        if (new_target >= CONFIG_MAX_CPUS)
            new_target = 0;
        if (new_target != queue_target(task) &&
            !admission_exempted(task)) {
            uint64_t util = 0;
            uint32_t bound = 0;
            errors::SchedulerError adm =
                admission_check_cpu_locked(task, new_target, &util, &bound);
            if (adm != errors::SCHED_ERR_OK) {
                Logger::warn("sched: set_affinity id=%u denied move to "
                             "CPU%u (err=%u, util=%u > bound=%u)",
                             task.id, new_target,
                             static_cast<uint64_t>(adm), util, bound);
                quiesce_exit();
                return adm;
            }
        }
        // Issue #25 C1: preserve queue membership across the move.  A
        // queued-but-BLOCKED task (legal: placement is stable across ticks
        // since nothing can dispatch it) must land in the new CPU's queue;
        // the old code only re-enqueued READY tasks and silently dropped
        // BLOCKED ones (smp_sched_cross_move).  was_queued covers every
        // state; the READY disjunct keeps the unqueued-READY enqueue.
        // Issue #23: route through the single enqueue/dequeue points so an
        // EDF-eligible task moves between deadline lists, not bitmaps
        // (a direct bitmap move would strand it in the wrong queue; the
        // EDF pop lazily drops non-READY heads, so BLOCKED placement is
        // equally safe in either queue).
        bool was_queued = task.in_ready_queue_ || task.in_edf_queue_;
        if (was_queued) {
            dequeue_ready(task);
        }
        task.cpu_affinity = mask;
        // Issue #25 C1: never enqueue a task the scheduler doesn't own.
        // create → set_affinity → add_task is the API pattern: at
        // set_affinity time the task isn't registered yet, and the direct
        // enqueue below (no enqueue_ready H2 guard on this path) would link
        // it into a ready queue; add_task then resets its runq links while
        // still linked → intrusive-list corruption → AP next_task garbage
        // → #GP at iret (smp_sched_ap_runs_pinned).  Record the mask now;
        // add_task enqueues after registration.
        bool owned = (Scheduler::find_task(task.id) == &task);
        if (was_queued ||
            (owned && task.state == TaskState::READY &&
             !task.in_ready_queue_ && !task.in_edf_queue_)) {
            enqueue_ready(task);
        }
    }
    quiesce_exit();
    return errors::SCHED_ERR_OK;
}

void Scheduler::set_affinity(TaskControlBlock &task, uint64_t mask) noexcept {
    // Issue #23: legacy warn-only wrapper — denial is logged inside
    // set_affinity_err and swallowed here; fallible callers use _err.
    (void)set_affinity_err(task, mask);
}

errors::SchedulerError
Scheduler::set_tls_base_err(TaskControlBlock &task, uint64_t base) noexcept {
    // Issue #74: IrqGuard spans the gate + live write (re-audit S3) — a
    // preemption between is_current and the register write would load a
    // stale base. The spinlock covers the store only, never a reschedule.
    arch::IrqGuard irq_guard{};
    {
        SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
        TaskControlBlock *cur = current_task();
        if (!cur)
            return errors::SCHED_ERR_NO_CURRENT;
        (void)cur;
        task.tls_base_ = base;
    }
    if (&task == Scheduler::current_task()) {
        // Target is the running task: apply now (§13 crt0 sequence).
#if defined(CONFIG_ARCH_X86_64)
        arch::wrmsr(arch::MSR_FS_BASE, base);
#elif defined(CONFIG_ARCH_AARCH64)
        arch::write_tpidr_el0(base);
#elif defined(CONFIG_ARCH_RISCV64)
        arch::write_tp(base);
#endif
    }
    return errors::SCHED_ERR_OK;
}

// ---------------------------------------------------------------------------
// balancer_tick — RT load balancer (issue #61)
// ---------------------------------------------------------------------------
uint64_t Scheduler::queue_depth(uint64_t cpu) noexcept {
    return rq_for(cpu).depth();
}

uint64_t Scheduler::migration_count(uint64_t cpu) noexcept {
    return __atomic_load_n(&migration_count_[cpu % CONFIG_MAX_CPUS],
                           __ATOMIC_ACQUIRE);
}

uint64_t Scheduler::get_affinity(const TaskControlBlock &task) noexcept {
    return task.cpu_affinity;
}

bool Scheduler::is_rt_task(const TaskControlBlock &task) noexcept {
    return task.period_ticks != 0 &&
           task.period_ticks != TaskControlBlock::NO_PERIOD;
}

uint64_t Scheduler::up_cpu_count() noexcept {
    return sched_up_cpus();
}

void Scheduler::reset_migration_counts() noexcept {
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c)
        __atomic_store_n(&migration_count_[c], 0ULL, __ATOMIC_RELAXED);
}

void Scheduler::balancer_tick() noexcept {
    // BSP-only cadence point (AP ticks are dispatch-only, spec §3.4.4).
    if (sched_cpu() != 0)
        return;
    uint64_t up = sched_up_cpus();
    if (up < 2)
        return;
    if (up > CONFIG_MAX_CPUS)
        up = CONFIG_MAX_CPUS;
    arch::IrqGuard irq_guard{};
    quiesce_enter();
    if (!scheduler_lock_.try_lock()) {
        quiesce_exit();
        return;
    }
    // Bounded moves per tick (RT budget); depths recomputed per move.
    for (uint64_t move = 0; move < BALANCER_MAX_MIGRATIONS_PER_TICK;
         ++move) {
        uint64_t busy = 0;
        uint64_t idle = 0;
        uint64_t busy_depth = rq_for(0).depth();
        uint64_t idle_depth = busy_depth;
        for (uint64_t c = 1; c < up; ++c) {
            uint64_t depth = rq_for(c).depth();
            if (depth > busy_depth) {
                busy_depth = depth;
                busy = c;
            }
            if (depth < idle_depth) {
                idle_depth = depth;
                idle = c;
            }
        }
        if (busy == idle || busy_depth <= idle_depth + BALANCER_THRESHOLD)
            break;
        // Busiest queue: first movable task (set_affinity move fence:
        // queued, quiesced, non-RT/user/idle/current, owned, affine).
        TaskControlBlock *candidate = nullptr;
        for (auto *t = all_tasks_.first_ptr(); t != nullptr;
             t = all_tasks_.next_ptr(t)) {
            if (t->magic != TaskControlBlock::TCB_MAGIC)
                continue;
            if (!t->in_ready_queue_ || queue_target(*t) != busy)
                continue;
            if (t->state != TaskState::READY &&
                t->state != TaskState::BLOCKED)
                continue;
            if (t->is_user_ || is_idle_task(t) || is_rt_task(*t))
                continue;
            if (is_current_on_any_cpu(t) || find_task(t->id) != t)
                continue;
            if ((t->cpu_affinity & (1ULL << idle)) == 0)
                continue;
            candidate = t;
            break;
        }
        if (candidate == nullptr)
            break;
        // Issue #23: partitioned-EDF destination probe BEFORE the move.
        // Today's candidates are always non-RT (is_rt_task filter above),
        // hence admission-exempt, so this passes — it is the enforcement
        // point that keeps migration schedulable if an RT class ever
        // becomes migratable, and the hook #167 CPU-pinning will reuse.
        // Denial stops balancing for this tick (break: depths are
        // unchanged, so re-scanning would reselect the same candidate;
        // next tick retries).  No move, no count bump, no logging.
        if (!admission_exempted(*candidate) &&
            admission_check_cpu_locked(*candidate, idle, nullptr, nullptr) !=
                errors::SCHED_ERR_OK) {
            break;
        }
        // Move + re-pin (queue==target invariant, same as set_affinity;
        // remove() clears the queue flags, enqueue() re-arms them).
        rq_for(busy).remove(*candidate, candidate->rq_priority_);
        candidate->cpu_affinity = 1ULL << idle;
        rq_for(idle).enqueue(*candidate, effective_priority(candidate));
        __atomic_fetch_add(&migration_count_[busy], 1ULL, __ATOMIC_RELAXED);
    }
    scheduler_lock_.unlock();
    quiesce_exit();
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

/// @brief Clear every deferred-switch atom and bump the switch generation.
///        Any ISR that captured the pre-clear generation fails its re-check
///        (isr_stubs.asm jne .restore); any ISR entering after sees
///        save_rsp_to==0.  Called from the apply-side liveness re-check and
///        from task removal paths.
void Scheduler::cancel_pending_switch() noexcept {
    H2_REC(H2_EV_CLR_MISC,
           __atomic_load_n(&Scheduler::SwSlots::next_task_id(), __ATOMIC_RELAXED),
           0, 0);
    __atomic_store_n(&Scheduler::SwSlots::save_rsp_to(), (uint64_t *)nullptr,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_rsp_from(), (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_tls_from(), (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::next_task_id(), UINT64_MAX,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_kstack_base(), (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_kstack_top(), (uint64_t)0,
                     __ATOMIC_RELEASE);
    uint64_t gen =
        __atomic_load_n(&Scheduler::SwSlots::generation(), __ATOMIC_RELAXED);
    __atomic_store_n(&Scheduler::SwSlots::generation(), gen + 1,
                     __ATOMIC_RELEASE);
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
static void invalidate_pending_switch_to(uint64_t task_id) noexcept {
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

void Scheduler::release_zombie(TaskControlBlock &task) noexcept {
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
}

uint64_t Scheduler::snapshot_zombies(TaskControlBlock **out,
                                     uint64_t max_out) noexcept {
    if (!out || max_out == 0)
        return 0;
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
    // Caller (on_tick tail) holds the global lock; take the leaf here
    // (order global→leaf). List surgery under both; free() runs per item
    // with NEITHER held (two-phase — a VMM-nested cleanup_step_try skips
    // instead of deadlocking).
    for (uint64_t i = 0; i < max_flush; ++i) {
        TaskControlBlock *task;
        {
            SpinLockGuard<sync::SpinLock> zguard(zombie_lock_);
            task = zombie_head_;
            if (!task)
                break;
            // Check magic before accessing any field past offset 0.
            if (task->magic != TaskControlBlock::TCB_MAGIC) {
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
            if (task->in_ready_queue_)
                rq_task(*task).remove(*task, effective_priority(task));
            // Issue #197 defense-in-depth: unlink a stale EDF entry.
            if (task->in_edf_queue_)
                edf_remove(*task, queue_target(*task));
            __atomic_sub_fetch(&zombie_count_, 1, __ATOMIC_RELAXED);
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
            if (task->magic != TaskControlBlock::TCB_MAGIC) {
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
        if (task->magic != TaskControlBlock::TCB_MAGIC) {
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
        if (task->magic != TaskControlBlock::TCB_MAGIC) {
            zombie_head_ = nullptr;
            zombie_tail_ = nullptr;
            zombie_count_ = 0;
            return;
        }
        // Issue #197: spare an AP-live head (never free a live stack).
        if (is_current_on_any_cpu(task)) {
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
    }
    task->cleanup();
    MemPool::free(task);
}

#if CONFIG_MEMORY_BUDGET
void Scheduler::init_memory_budget(uint64_t total_pages) noexcept {
    memory_budget_pages_ = total_pages;
}

bool Scheduler::reserve_memory_pages(uint64_t count) noexcept {
    if (memory_budget_pages_ < count)
        return false;
    memory_budget_pages_ -= count;
    return true;
}

void Scheduler::release_memory_pages(uint64_t count) noexcept {
    memory_budget_pages_ += count;
}

uint64_t Scheduler::remaining_memory_budget() noexcept {
    return memory_budget_pages_;
}
#endif

void Scheduler::set_task_ready(TaskControlBlock &task) noexcept {
    arch::IrqGuard irq_guard{};
    task.state = TaskState::READY;
    uint64_t target = queue_target(task);
    if (task.is_user_ && target != 0)
        target = 0; // shared-TSS backstop (see enqueue_ready)
    uint64_t here = sched_cpu();
    if (target != here) {
#if defined(CONFIG_ARCH_X86_64)
        uint64_t up = sched_up_cpus();
        if (target < up && up > 1) {
            // Remote up CPU: mailbox + SCHED IPI (never direct remote
            // enqueue). Target drains in its IPI handler and dispatches
            // on its next tick (≤1 tick latency).
            mailbox_publish(target, task);
            uint32_t lapic = arch::per_cpu[target].lapic_id;
            arch::APIC::send_ipi(lapic, arch::APIC::SCHED_VECTOR,
                                 arch::APIC::IpiMode::FIXED);
            return;
        }
#endif
        // Target not up (or single-core build): direct-enqueue under the
        // global lock; the task waits like single-core parked work.
        SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
        task.in_ready_queue_ = false;
        rq_for(target).enqueue(task, effective_priority(&task));
        return;
    }
    enqueue_ready(task);
}

/// @brief Wake a parent blocked in waitpid for a child that just terminated.
/// Mirrors the wake performed by Syscall::sys_exit, but for non-sys_exit
/// termination paths (Scheduler::terminate, deadline miss, cleanup_test_tasks)
/// so a parent blocked in waitpid is not left waiting forever on a child that
/// exited via a path other than sys_exit.
/// @param child The just-terminated child task.
static void wake_waiting_parent(TaskControlBlock &child) {
    if (child.parent_id == 0)
        return;
    auto *p = Scheduler::find_task(child.parent_id);
    if (!p)
        return;
    if (p->waiting_child_pid != child.id)
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
    if (p->state == TaskState::BLOCKED) {
        p->in_ready_queue_ = false;
        Scheduler::set_task_ready(*p);
        if (task_stack_ptr(p)) {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *stack = reinterpret_cast<uint64_t *>(task_stack_ptr(p));
            stack[0] = child.id;
        }
    }
    p->waiting_child_pid = 0;
    p->remove_child(&child);
    child.parent_id = 0;
}

// Forward declaration — defined later in this translation unit.
static void switch_to_task(TaskControlBlock *current, TaskControlBlock &next,
                           sync::SpinLock *held_lock);

/// @brief Bill (ns_monotonic() - exec_stamp_ns) into the task's executed-ns
///        totals (issue #21 — feeds admission, WCET validation, SYS_TIMES).
///        Saturating adds (never wrap); always restamps.  A zero stamp means
///        "never stamped" (fresh task): stamp without billing so the first
///        charge never bills creation-to-first-dispatch as execution.
///        Caller must hold scheduler_lock_ (or run IRQ-off on the tick path)
///        so the add+restamp is atomic — never double-charge, never lose.
/// @param t Task to charge (must be the running task or a terminating task).
static void charge_exec(TaskControlBlock &t) noexcept {
    const uint64_t now = arch::Timer::ns_monotonic();
    if (t.exec_stamp_ns == 0) {
        t.exec_stamp_ns = now;
        return;
    }
    const uint64_t delta =
        (now >= t.exec_stamp_ns) ? now - t.exec_stamp_ns : 0;
    if (delta > 0) {
        t.exec_ns_total = (t.exec_ns_total > UINT64_MAX - delta)
                              ? UINT64_MAX
                              : t.exec_ns_total + delta;
        t.exec_period_ns = (t.exec_period_ns > UINT64_MAX - delta)
                               ? UINT64_MAX
                               : t.exec_period_ns + delta;
    }
    t.exec_stamp_ns = now;
}

/// @brief Stamp a task being switched in: it starts accruing from now, and
///        any stale stamp (e.g. from a previous quantum) is discarded so no
///        descheduled interval is ever billed (issue #21).
/// @param t Task being dispatched.
static void stamp_exec(TaskControlBlock &t) noexcept {
    t.exec_stamp_ns = arch::Timer::ns_monotonic();
}

errors::SchedulerError
Scheduler::terminate_err(TaskControlBlock &task,
                         uint64_t exit_code) noexcept {
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
    }
}

void Scheduler::read_times(TaskControlBlock &task, TaskTimes &out) noexcept {
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    if (&task == current_task())
        charge_exec(task); // charge-before-read: sample includes time to now
    out.exec_ns_total = task.exec_ns_total;
    out.exec_period_ns = task.exec_period_ns;
    out.executed_ticks = task.executed_ticks;
    out.wcet_ticks = task.wcet_ticks;
}

bool Scheduler::set_sched_policy(TaskControlBlock &task,
                                 SchedPolicy policy) noexcept {
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
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
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    task.edf_exempt = exempt;
    // Same migration as a policy change: an exempted task must leave the
    // EDF list for the bitmap (and vice versa), or it is stranded.
    migrate_dispatch_queue(task);
}

void Scheduler::assign_deadline_priority(TaskControlBlock &task) noexcept {
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
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    task.priority = prio;
    task.base_priority = prio;
    // Re-bucket (bitmap) or re-sort (EDF tie-break) so the new value
    // takes effect immediately (FIX rms-o1 contract).
    if (task.in_edf_queue_)
        edf_resort(task);
    else if (task.in_ready_queue_)
        rq_task(task).move_priority(task, task.rq_priority_, prio);
}

TaskControlBlock *const Scheduler::ID_TOMBSTONE =
    reinterpret_cast<TaskControlBlock *>(static_cast<uintptr_t>(1));

AllTasksRegistry Scheduler::all_tasks_;

constinit TaskControlBlock *Scheduler::id_table_[ID_TABLE_SIZE] = {};

constinit uint64_t Scheduler::next_task_id_ = 0;
constinit uint64_t Scheduler::sporadic_task_count_ = 0;
constinit bool Scheduler::preempt_enabled_ = false;
#if CONFIG_MEMORY_BUDGET
constinit uint64_t Scheduler::memory_budget_pages_ = 0;
#endif
constinit bool Scheduler::suppress_terminated_log_ = false;
TestContext *Scheduler::test_context_ = nullptr;
#if CONFIG_DEADLINE_MONITOR_TASK
constinit TaskControlBlock *Scheduler::s_monitor_task_ = nullptr;
bool Scheduler::s_scan_requested_ = false;
#endif
ReadyQueueManager Scheduler::ready_queues_[CONFIG_MAX_CPUS];
DeadlineList Scheduler::deadline_list_;
    constinit TaskControlBlock *Scheduler::idle_task_ = nullptr;
TaskControlBlock *Scheduler::idle_tasks_[CONFIG_MAX_CPUS] = {};
Scheduler::MailboxEntry
    Scheduler::wake_mailbox_[CONFIG_MAX_CPUS][MAILBOX_SLOTS] = {};
constinit uint64_t Scheduler::wake_mailbox_count_[CONFIG_MAX_CPUS] = {};
sync::SpinLock Scheduler::wake_mailbox_lock_[CONFIG_MAX_CPUS];
constinit bool Scheduler::sched_quiesced_ = false;
sync::SpinLock Scheduler::zombie_lock_;
    constinit TaskControlBlock *Scheduler::shell_task_ptr_ = nullptr;
    constinit TaskControlBlock *Scheduler::harness_task_ptr_ = nullptr;
constinit TaskControlBlock *Scheduler::zombie_head_ = nullptr;
constinit TaskControlBlock *Scheduler::zombie_tail_ = nullptr;
constinit uint64_t Scheduler::zombie_count_ = 0;
uint64_t Scheduler::migration_count_[CONFIG_MAX_CPUS] = {};
uint64_t Scheduler::loadavg_1min_ = 0;
uint64_t Scheduler::loadavg_5min_ = 0;
uint64_t Scheduler::loadavg_15min_ = 0;
sync::SpinLock Scheduler::scheduler_lock_;

// Liu-Leyland Rate-Monotonic LUB bounds (scaled by 1000000)
static constexpr uint64_t LIU_LEYLAND_MAX_TASKS = 20;
static constexpr uint32_t LIU_LEYLAND_BOUNDS[LIU_LEYLAND_MAX_TASKS + 1] = {
    0,      1000000, 828427, 779763, 756828, 743491, 734772,
    728626, 724061,  720537, 717734, 715451, 713557, 711958,
    710592, 709409,  708378, 707472, 706669, 705952, 705298};
static constexpr uint32_t LIU_LEYLAND_LIMIT = 693147;

// ---------------------------------------------------------------------------
// Issue #20 — enforced admission control (upgrades deadline.md I-8 from
// advisory warn-only to a fail-closed gate).  All helpers are
// allocation-free integer math with no logging; the caller (add_task_err)
// holds scheduler_lock_ throughout, so the check-and-enqueue is atomic.
// Wakeups of already-admitted tasks (set_task_ready) are deliberately NOT
// re-gated: denying a wakeup would wedge IPC waiters (deadlock risk, see
// the H2 history) — admission is a create-time gate, the standard
// real-time doctrine (once admitted, guaranteed).
// ---------------------------------------------------------------------------

bool Scheduler::admission_exempted(const TaskControlBlock &t) noexcept {
    if (is_idle_task(&t))
        return true;
    if (t.magic != TaskControlBlock::TCB_MAGIC)
        return true; // corrupt: never counted (fail-closed elsewhere)
    if (t.edf_exempt)
        return true; // SS daemons, harness (taskdefs spawn sites)
    if (t.period_ticks == 0 || t.period_ticks == TaskControlBlock::NO_PERIOD)
        return true; // aperiodic / untracked (I-4)
    if (t.deadline_ticks == 0)
        return true;
    return false;
}

errors::SchedulerError
Scheduler::check_wcet_locked(const TaskControlBlock &t) noexcept {
    if (t.wcet_ticks == 0)
        return errors::SCHED_ERR_OK; // implicit 100%: validated via LUB sum
    if (t.period_ticks == 0 || t.period_ticks == TaskControlBlock::NO_PERIOD)
        return errors::SCHED_ERR_WCET_INVALID; // explicit WCET, untracked task
    if (t.wcet_ticks > t.period_ticks)
        return errors::SCHED_ERR_WCET_INVALID;
    return errors::SCHED_ERR_OK;
}

/// @brief Frozen admission numerator (issues #20/#22): explicit WCET, else
///        the attached server's max budget C (mode-agnostic — SPORADIC,
///        DEFERRABLE and BACKGROUND all expose C via max_budget(), so
///        server budgets count identically without any gate change), else
///        remaining_ticks.  Caller guarantees t is non-exempt (period > 0).
static uint64_t server_budget_for_admission(const TaskControlBlock &t) noexcept {
    if (t.wcet_ticks > 0)
        return t.wcet_ticks;
    if (t.get_sporadic_server() != nullptr)
        return t.get_sporadic_server()->max_budget();
    return t.remaining_ticks;
}

errors::SchedulerError Scheduler::admission_check_locked(
    const TaskControlBlock &candidate, uint64_t *out_util,
    uint32_t *out_bound) noexcept {
    // Issue #23: the global gate is the single-partition special case keyed
    // on the candidate's own target — one math path, no duplication, no
    // behavior change on single-CPU.
    return admission_check_cpu_locked(candidate, queue_target(candidate),
                                      out_util, out_bound);
}

errors::SchedulerError Scheduler::admission_check_cpu_locked(
    const TaskControlBlock &candidate, uint64_t cpu, uint64_t *out_util,
    uint32_t *out_bound) noexcept {
    cpu %= CONFIG_MAX_CPUS;
    // Audit S3 (issue #20): exemption precedes WCET-validity, matching the
    // I-8 scope text exactly ("periodic non-exempt tasks only").  An
    // otherwise-exempt task (boot daemon, aperiodic) carrying a stale
    // explicit WCET must never be rejected — denying a boot daemon would
    // wedge the boot.  WCET-validity binds only tasks inside the gate.
    if (admission_exempted(candidate)) {
        if (out_util != nullptr)
            *out_util = 0;
        if (out_bound != nullptr)
            *out_bound = 0;
        return errors::SCHED_ERR_OK;
    }
    errors::SchedulerError w = check_wcet_locked(candidate);
    if (w != errors::SCHED_ERR_OK)
        return w;
    // Numerator rule frozen from the advisory path: explicit WCET, else the
    // sporadic-server max budget, else remaining_ticks.  S2 note: wcet <=
    // period <= NO_PERIOD (2^32-1), so wcet*1000000 < 2^63 — no overflow;
    // period == 0 is excluded above, so the divide is safe.
    uint64_t total_util = 0;
    uint64_t real_tasks = 0;
    for (TaskControlBlock *t = all_tasks_.first_ptr(); t != nullptr;
         t = all_tasks_.next_ptr(t)) {
        if (t->id == candidate.id)
            continue; // re-check safe: never double-count the candidate
        if (admission_exempted(*t))
            continue;
        // Issue #23: partitioned accounting — only tasks targeting this
        // CPU enter this partition's numerator (queue_target: lowest-set
        // affinity bit, empty mask reads as CPU0).
        if (queue_target(*t) != cpu)
            continue;
        ++real_tasks;
        uint64_t wcet = server_budget_for_admission(*t);
        total_util += (wcet * 1000000) / t->period_ticks;
    }
    ++real_tasks;
    uint64_t cwcet = server_budget_for_admission(candidate);
    total_util += (cwcet * 1000000) / candidate.period_ticks;
    uint32_t bound = real_tasks <= LIU_LEYLAND_MAX_TASKS
                         ? LIU_LEYLAND_BOUNDS[real_tasks]
                         : LIU_LEYLAND_LIMIT;
    if (out_util != nullptr)
        *out_util = total_util;
    if (out_bound != nullptr)
        *out_bound = bound;
    if (total_util > bound)
        return errors::SCHED_ERR_ADMISSION_DENIED;
    return errors::SCHED_ERR_OK;
}

// ---------------------------------------------------------------------------
// Init / lifecycle
// ---------------------------------------------------------------------------

void Scheduler::init(const SchedulerConfig &cfg) {
    for (uint64_t i = 0; i < ID_TABLE_SIZE; ++i)
        id_table_[i] = nullptr;

    idle_task_ = TaskControlBlock::create(kernel::integrity::idle_task_main, 0,
                                          TaskControlBlock::NO_PERIOD);
    if (!idle_task_) panic("Scheduler::init: idle task OOM");
    // G2: after the panic guard the object is provably live — bind a
    // reference (docs/irqguard-ledger.md §G2-A).
    TaskControlBlock &idle = *idle_task_;
    idle.state = TaskState::READY;
    __builtin_strncpy(idle.name, "idle", CONFIG_TASK_NAME_LEN - 1);
    idle.name[CONFIG_TASK_NAME_LEN - 1] = '\0';

    all_tasks_.append(*idle_task_);
    ENSURE(id_table_insert(idle_task_->id, idle_task_) && "id_table full at init");
    idle_tasks_[0] = idle_task_;
    set_current_ptr(idle_task_);
    sporadic_task_count_ = cfg.sporadic_task_count;
    preempt_enabled_ = cfg.preempt_enabled;
    suppress_terminated_log_ = cfg.suppress_terminated_log;

    // Static kernel CR3 for the isr_stubs.asm fallback when returning to the
    // kernel/harness context (VMM::init has already captured kernel_pml4_).
    scheduler_kernel_cr3 = VMM::get_kernel_pml4();
    // Issue #25 C1: next_id starts empty (UINT64_MAX) on every CPU.
    // Zero-init would fake-arm CPUs 1..7 (task ID 0, the idle, is valid).
    // Single owner here: init runs on the BSP before any AP exists.
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c)
        scheduler_next_task_id[c] = UINT64_MAX;

#if CONFIG_DEADLINE_MONITOR_TASK
    ensure_monitor();
#endif
}

TaskControlBlock *Scheduler::create_ap_idle(uint64_t cpu) noexcept {
    cpu %= CONFIG_MAX_CPUS;
    if (cpu == 0 || idle_tasks_[cpu] != nullptr)
        return idle_tasks_[cpu];
    // Table mutations under the global lock (blocking; the AP runs IF=0
    // here and the BSP never waits on the AP, so the take completes).
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    if (idle_tasks_[cpu] != nullptr)
        return idle_tasks_[cpu];
    TaskControlBlock *idle = TaskControlBlock::create(
        kernel::integrity::ap_idle_main, 0, TaskControlBlock::NO_PERIOD);
    if (!idle)
        return nullptr;
    idle->state = TaskState::READY;
    idle->cpu_affinity = static_cast<uint64_t>(1) << cpu;
    __builtin_strncpy(idle->name, "apidle", CONFIG_TASK_NAME_LEN - 1);
    idle->name[CONFIG_TASK_NAME_LEN - 1] = '\0';
    all_tasks_.append(*idle);
    if (!id_table_insert(idle->id, idle))
        return nullptr;
    idle_tasks_[cpu] = idle;
    return idle;
}

void Scheduler::register_task(TaskControlBlock &task) {
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    ENSURE(id_table_insert(task.id, &task) && "id_table full");
    all_tasks_.append(task);
    if (task.period_ticks > 0 && task.deadline_ticks > 0) {
        deadline_list_.insert(task);
    }
    task.in_ready_queue_ = false;
    task.runq_next_ = nullptr;
    task.runq_prev_ = nullptr;
    kernel::test::ResourceTracker::instance().track_task_add();
}

void Scheduler::add_task(TaskControlBlock &task) {
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    ENSURE(task.state == TaskState::READY);
    ENSURE(id_table_insert(task.id, &task) && "id_table full");
    all_tasks_.append(task);
    if (task.period_ticks > 0 && task.deadline_ticks > 0) {
        deadline_list_.insert(task);
    }
    // Issue #19: route through enqueue_ready so EDF-eligible tasks land in
    // the deadline-ordered list.  A direct bitmap enqueue here would make
    // every new deadline task priority-dispatched (EDF silently dead).
    task.in_ready_queue_ = false;
    task.runq_next_ = nullptr;
    task.runq_prev_ = nullptr;
    task.in_edf_queue_ = false;
    task.edf_next_ = nullptr;
    task.edf_prev_ = nullptr;
    enqueue_ready(task);
    kernel::test::ResourceTracker::instance().track_task_add();

    Logger::info("Scheduler: task '%s' (ID=%u, prio=%u) started", task.name,
                 task.id, task.priority);

    // Issue #20: legacy ADVISORY path — warn-only by design (void return
    // cannot report denial; silently deferring would break every caller
    // that assumes enqueue).  New production paths (fork, exec) and all
    // admission-sensitive code must use add_task_err(), the enforced gate.
    // Liu-Leyland LUB admission test
    if (task.period_ticks > 0 && task.period_ticks <= 100) {
        uint64_t total_util = 0;
        uint64_t real_tasks = 0;
        TaskControlBlock *t = all_tasks_.first_ptr();
        for (; t; t = all_tasks_.next_ptr(t)) {
            if (is_idle_task(t) || t->magic != TaskControlBlock::TCB_MAGIC)
                continue;
            if (t->period_ticks > 0) {
                ++real_tasks;
                uint64_t wcet = server_budget_for_admission(*t);
                uint64_t util = (wcet * 1000000) / t->period_ticks;
                total_util += util;
            }
        }
        uint32_t bound = real_tasks <= LIU_LEYLAND_MAX_TASKS
                             ? LIU_LEYLAND_BOUNDS[real_tasks]
                             : LIU_LEYLAND_LIMIT;
        if (total_util > bound) {
            Logger::warn("Scheduler: task %d (prio=%d, period=%d) exceeds "
                         "Liu-Leyland bound (%d > %d) — overrun possible",
                         task.id, task.priority, task.period_ticks, total_util,
                         bound);
        }
    }
}

void Scheduler::remove_task(TaskControlBlock &task) {
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    if (task.magic != TaskControlBlock::TCB_MAGIC) {
        Logger::error("remove_task: TCB %p magic=0x%lx (expected 0x%lx)",
                      &task, (uint64_t)task.magic,
                      (uint64_t)TaskControlBlock::TCB_MAGIC);
        kernel::diag::dump_tcb_write_log("[REMOVE_TASK] corrupted TCB");
        // Despite the corruption, try to remove from all_tasks_ to prevent
        // cascading corruption.  If all_bucket_ is also corrupted (poisoned
        // to >= NUM_PRIORITIES), fall back to a full priority scan.
        if (task.all_bucket_ < CONFIG_PRIORITY_CEILING + 1) {
            all_tasks_.remove(task);
        } else {
            all_tasks_.remove_unsafe(task);
        }
        return;
    }
    // BUGS.md#019/#020: never leave current_task() aliasing a TCB that is
    // about to be freed.  If the removed task is the current task, redirect
    // current_task() to the idle task (always valid, the scheduler's safe
    // fallback) BEFORE the block is recycled.  Otherwise a later
    // TaskControlBlock::create() can MemPool::alloc() the same block and
    // memset() it to zero, zeroing the live current task's context (ctx.rip=0)
    // and corrupting the scheduler / producing 0xDD-poisoned use-after-free
    // crashes.  The next tick's deferred switch will pick a real successor.
    if (&task == current_task() && own_idle() && own_idle() != &task) {
        set_current_ptr(own_idle());
    }
    all_tasks_.remove(task);
    deadline_list_.remove(task);
    id_table_remove(&task);
    dequeue_ready(task);

    __atomic_store_n(&Scheduler::SwSlots::save_rsp_to(), nullptr, __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_rsp_from(), (uint64_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), (uint64_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_tls_from(), (uint64_t)0, __ATOMIC_RELEASE);

    // NOTE: ResourceTracker::track_task_remove() is intentionally NOT called
    // here.  Task teardown has two styles (operator delete, and the reaper's
    // manual MemPool::free), and BOTH route through TaskControlBlock::cleanup(),
    // which is the single canonical point that calls track_task_remove().
    // Tracking here too would double-count every deleted task (remove_task is
    // also invoked by operator delete) and produce a false ResourceTracker leak.
}

bool Scheduler::unregister_task(TaskControlBlock &task) noexcept {
    if (!scheduler_lock_.try_lock())
        return false;
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_, adopt_lock);

    if (&task == current_task() && own_idle() && own_idle() != &task) {
        set_current_ptr(own_idle());
    }
    all_tasks_.remove(task);
    deadline_list_.remove(task);
    id_table_remove(&task);
    dequeue_ready(task);

    __atomic_store_n(&Scheduler::SwSlots::save_rsp_to(), nullptr, __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_rsp_from(), (uint64_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), (uint64_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_tls_from(), (uint64_t)0, __ATOMIC_RELEASE);

    return true;
}

// ---------------------------------------------------------------------------
// Current-task / lookup
// ---------------------------------------------------------------------------

// Defined below (near switch_to_task); used by set_current / rate_monotonic
// schedule to restore state when a deferred-switch arm is cleared un-applied.
static void restore_preempted_current(TaskControlBlock *current,
                                      uint64_t armed_target_id) noexcept;

TaskControlBlock *Scheduler::current_task() noexcept {
    return current_cpu().current;
}

uint64_t Scheduler::task_count() noexcept {
    return all_tasks_.size();
}

uint64_t Scheduler::current_index() noexcept {
    uint64_t i = 0;
    for (auto *t = all_tasks_.first_ptr(); t; t = all_tasks_.next_ptr(t)) {
        if (t == current_task())
            return i;
        ++i;
    }
    return 0;
}

void Scheduler::set_current_index(uint64_t idx) noexcept {
    uint64_t i = 0;
    for (auto *t = all_tasks_.first_ptr(); t; t = all_tasks_.next_ptr(t)) {
        if (i++ == idx) {
            set_current_ptr(t);
            return;
        }
    }
}

void Scheduler::set_current_task(TaskControlBlock *t) noexcept {
    if (t && t->magic == TaskControlBlock::TCB_MAGIC) {
        // Issue #172: adoption path publishes placement like a dispatch.
        __atomic_store_n(&t->running_on_cpu, Scheduler::sched_cpu(),
                         __ATOMIC_RELEASE);
        set_current_ptr(t);
    }
}

TaskControlBlock *Scheduler::task_at(uint64_t index) noexcept {
    // Index 0 is reserved for the idle (reaper) task, the root adoptive parent.
    // It is NOT necessarily the highest-priority bucket head in
    // AllTasksRegistry (idle is clamped to CONFIG_PRIORITY_CEILING and shares
    // that bucket with the deadline-monitor task), so return it explicitly
    // rather than via first_ptr().
    if (index == 0)
        return idle_task_;
    // Indices 1..N-1 map to the remaining (non-idle) tasks, by priority then
    // insertion order, so callers iterating 0..task_count()-1 still visit every
    // task exactly once (task_count() includes idle).
    uint64_t i = 0;
    for (auto *t = all_tasks_.first_ptr(); t; t = all_tasks_.next_ptr(t)) {
        if (t == idle_task_)
            continue;
        if (i++ == index - 1)
            return t;
    }
    return nullptr;
}

TaskControlBlock *Scheduler::find_task(uint64_t id) noexcept {
    return id_table_find(id);
}

bool Scheduler::debug_id_table_references(void *block) noexcept {
    auto *b = static_cast<TaskControlBlock *>(block);
    for (uint64_t i = 0; i < ID_TABLE_SIZE; ++i) {
        if (id_table_[i] == b)
            return true;
    }
    return false;
}

bool Scheduler::needs_switch() noexcept {
    if (all_tasks_.size() <= 1)
        return false;
    auto *current = current_task();
    if (!current || current->magic != TaskControlBlock::TCB_MAGIC)
        return false;
    if (current == own_idle())
        return false;

    // A blocked/terminated current task must always yield to let a runnable
    // task take over — otherwise a high-priority task that blocks
    // (e.g. inside IPC::send_sync) would keep "winning" the priority compare
    // against lower-priority ready peers and the scheduler would never switch
    // to them, deadlocking the handshake (kernel hang in the test suite).
    if (current->state != TaskState::READY &&
        current->state != TaskState::RUNNING)
        return true;

    // Issue #19: earlier-deadline EDF preemption.  Cross-class comparison
    // stays priority-based below, so exempt/system tasks keep exact
    // current behavior — EDF only ADDS a preemption source, never removes
    // the bitmap one.
    if (edf_preempt_candidate() != nullptr)
        return true;

    uint64_t cur_eff = effective_priority(current);
    // O(1): check if any higher-priority task exists in the ready queue
    uint64_t highest_ready = rq_own().highest_ready_priority();
    return highest_ready > cur_eff;
}

TaskControlBlock *Scheduler::next_task() noexcept {
    if (all_tasks_.size() <= 1)
        return own_idle();

    // Issue #19: the EDF candidate agreed with needs_switch() — dispatch
    // it first so the arm and the decision can never disagree.
    if (auto *edf_next = edf_preempt_candidate()) {
        edf_remove(*edf_next, sched_cpu());
        return edf_next;
    }

    {
        // Issue #25 C1: dequeue from the OWN queue only (no stealing);
        // candidates not affine here are skipped (defense-in-depth).
        while (auto *candidate = rq_own().peek_highest()) {
            if (candidate == current_task() ||
                (candidate->state != TaskState::READY &&
                 candidate->state != TaskState::RUNNING) ||
                queue_target(*candidate) != sched_cpu()) {
                rq_own().dequeue_highest();
                continue;
            }
            rq_own().dequeue_highest();
            return candidate;
        }
    }

    return own_idle();
}

void Scheduler::set_current(TaskControlBlock &task) noexcept {
    auto *old = current_task();
    if (old == &task) {
        // Same-task set_current: a no-op, but a pending deferred-switch arm
        // must not survive — and if the preempted current was set READY +
        // enqueued by switch_to_task (boot-stack harness), restore it to
        // RUNNING so next_task() cannot skip it and fall to idle (INV-4).
        uint64_t armed = __atomic_load_n(&Scheduler::SwSlots::next_task_id(),
                                         __ATOMIC_RELAXED);
        H2_REC(H2_EV_CLR_SET, armed, 0, 0);
        __atomic_store_n(&Scheduler::SwSlots::load_rsp_from(), (uint64_t)0, __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), (uint64_t)0, __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::load_tls_from(), (uint64_t)0, __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::save_rsp_to(), (uint64_t *)nullptr,
                         __ATOMIC_RELEASE);
        restore_preempted_current(old, armed);
        return;
    }
    // Invariant: a task that is physically executing (current) must never sit
    // in the ready queue, or next_task() will re-select it.  The idle/harness
    // task runs at priority 0xFFFFFFFF, so leaving it queued makes it always
    // preempt — wedging the `all` suite (e.g. deadlocking at
    // ipc_send_sync_no_cli / test 536).  Remove the previous current so it is
    // no longer eligible; it stays resumable because next_task() returns it as
    // the idle/default task when no other task is ready.
    if (old && old->magic == TaskControlBlock::TCB_MAGIC && old->in_ready_queue_) {
        // remove() unlinks old from the intrusive list AND clears
        // in_ready_queue_/rq_priority_ itself.  Clearing in_ready_queue_ here
        // first would make TaskQueue::remove early-return (it guards on
        // !in_ready_queue_), leaving old physically linked with a stale
        // runq_next_/runq_prev_ — a dangling node that later corrupts the list
        // (pop_front dereferences a freed/reused TCB → #GP) once old is freed.
        rq_own().remove(*old, old->rq_priority_);
    }
    // State symmetry: a pending deferred-switch arm is being discarded here.
    // If the preempted current (the boot-stack harness) was set READY +
    // enqueued by switch_to_task, restore it to RUNNING and re-enqueue the
    // armed target so neither is stranded (INV-4 / INV-2, H2 residual).
    uint64_t armed = __atomic_load_n(&Scheduler::SwSlots::next_task_id(), __ATOMIC_RELAXED);
    H2_REC(H2_EV_CLR_SET, armed, 0, 0);
    __atomic_store_n(&Scheduler::SwSlots::load_rsp_from(), (uint64_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), (uint64_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_tls_from(), (uint64_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::save_rsp_to(), (uint64_t *)nullptr,
                     __ATOMIC_RELEASE);
    restore_preempted_current(old, armed);

    // Re-enqueue the previous task if it is still runnable but NOT current
    // or idle.  Without this, a higher-priority task that was preempted by a
    // lower-priority one (e.g. IPC receiver after wakeup) cannot be found
    // again — it is not in the ready queue and not current, so the scheduler
    // has no path to dispatch it (lazy rebuild in next_task only runs when
    // dequeue_highest returns null, which never happens while the receiver
    // sits in the queue).  See docs/specs/ipc.md §4 (H2) and
    // docs/specs/scheduler.md §6 (VIOL-1).
    if (old && old != &task && !is_idle_task(old) &&
        old->magic == TaskControlBlock::TCB_MAGIC &&
        (old->state == TaskState::READY ||
         old->state == TaskState::RUNNING)) {
        old->in_ready_queue_ = false;
        old->rq_priority_ = 0;
        enqueue_ready(*old);
    }

    set_current_ptr(&task);
}

// ---------------------------------------------------------------------------
// O(1) task-ID→TCB hash table (open addressing, linear probing)
// ---------------------------------------------------------------------------

uint64_t Scheduler::id_table_probe(uint64_t id) {
    return id & ID_TABLE_MASK;
}

bool Scheduler::id_table_insert(uint64_t id, TaskControlBlock *tcb) {
    uint64_t idx = id_table_probe(id);
    for (uint64_t probes = 0; probes < ID_TABLE_SIZE; ++probes) {
        if (id_table_[idx] == nullptr || id_table_[idx] == ID_TOMBSTONE) {
            id_table_[idx] = tcb;
            return true;
        }
        idx = (idx + 1) & ID_TABLE_MASK;
    }
    return false;
}

uint64_t Scheduler::alloc_id() noexcept {
    // H-1/H-2 (audit-scheduler-tasks-v0.4.2): honor the documented contract —
    // return TASK_INVALID when the ID/table capacity is exhausted, and
    // increment the counter atomically.  Monotonic allocation with no ID
    // reuse: TASK_INVALID (UINT64_MAX) is the out-of-capacity sentinel.
    // Issue #25 C1: skip LIVE ids.  reboot_from_table() resets the counter
    // to 1 (so init keeps PID 1) while the AP idle tasks stay live
    // (spared via is_idle_task); without the skip the respawned daemons
    // steal a live AP idle's id (observed: vfsd ID 2 == apidle id 2) →
    // id_table ambiguity, enqueue_ready refusals (degraded daemons), and
    // crossed snapshot restores (AP-idle affinity clobbered → AP runs a
    // foreign task → triple fault).  restore_state()'s next_task_id_
    // rewind is covered the same way.  Bounded retries, same sentinel.
    if (all_tasks_.size() >= MAX_TASKS)
        return UINT64_MAX;
    for (uint64_t tries = 0; tries < MAX_TASKS; ++tries) {
        uint64_t cand =
            __atomic_fetch_add(&next_task_id_, 1UL, __ATOMIC_RELAXED);
        if (id_table_find(cand) == nullptr)
            return cand;
    }
    return UINT64_MAX;
}

void Scheduler::reset_next_task_id(uint64_t id) noexcept {
    __atomic_store_n(&next_task_id_, id, __ATOMIC_RELAXED);
}

void Scheduler::id_table_remove(TaskControlBlock *task) {
    uint64_t idx = id_table_probe(task->id);
    for (uint64_t probe = 0; probe < ID_TABLE_SIZE; ++probe) {
        if (id_table_[idx] == nullptr)
            return;
        if (id_table_[idx] != ID_TOMBSTONE && id_table_[idx] == task) {
            id_table_[idx] = ID_TOMBSTONE;
            return;
        }
        idx = (idx + 1) & ID_TABLE_MASK;
    }
}

TaskControlBlock *Scheduler::id_table_find(uint64_t id) {
    // Issue #25 C1: bounded probe (insert/remove already bound theirs).
    // The old unbounded `while (!= nullptr)` wrapped forever when the
    // table held no nullptr on the probe chain (live entries + tombstones
    // fill it over long suites) and the id was absent — alloc_id's
    // skip-live scan stepped straight into it (test 309 hang: 80-slot
    // table, 127 historical ids).  A present id is always found within
    // ID_TABLE_SIZE probes; absent ids now return null.
    uint64_t idx = id_table_probe(id);
    for (uint64_t probes = 0; probes < ID_TABLE_SIZE; ++probes) {
        if (id_table_[idx] == nullptr)
            return nullptr;
        if (id_table_[idx] != ID_TOMBSTONE && id_table_[idx]->id == id)
            return id_table_[idx];
        idx = (idx + 1) & ID_TABLE_MASK;
    }
    return nullptr;
}

bool Scheduler::charge_task_memory(uint64_t pages) {
    auto *cur = current_task();
    if (!cur || cur->memory_budget_pages_ == 0)
        return true;  // unlimited
    if (cur->memory_used_pages_ + pages > cur->memory_budget_pages_)
        return false;  // over budget
    cur->memory_used_pages_ += pages;
    return true;
}

void Scheduler::credit_task_memory(uint64_t pages) {
    auto *cur = current_task();
    if (!cur || cur->memory_used_pages_ == 0)
        return;
    if (pages > cur->memory_used_pages_)
        cur->memory_used_pages_ = 0;
    else
        cur->memory_used_pages_ -= pages;
}

// ---------------------------------------------------------------------------
// on_tick — timer tick handler
// ---------------------------------------------------------------------------

void Scheduler::on_tick() noexcept {
    // Issue #25 C1: APs run dispatch-only ticks (spec §3.4.4).  The full
    // body below is BSP-only (unchanged); the AP skips accounting,
    // deadlines, watchdogs, zombies and sporadic (non-real-time in C1).
    if (sched_cpu() != 0) {
        ap_tick();
        return;
    }
    uint64_t current_tick = arch::Timer::ticks();
#if defined(CONFIG_DEBUG_IPC_SCHED)
    if (all_tasks_.size() >= 6) {
        bool lk_held = scheduler_lock_.try_lock();
        IPC_SCHED_TRACE("[TICK]", "t=", current_tick, "lk=",
                        (uint64_t)lk_held, "h=",
                        (uint64_t)scheduler_lock_.holder(), "nt=",
                        (uint64_t)all_tasks_.size());
        if (lk_held)
            scheduler_lock_.unlock();
    }
    // ==== Lock-contention detector (diagnostic only) ====
    // NOTE: on_tick may be called from task context (e.g. tests calling
    // Scheduler::on_tick() in a loop).  In that case the lock is legitimately
    // held by the *caller's* on_tick body, not by the ISR.  The timer ISR's
    // on_tick then repeatedly finds lk=0 until the caller's on_tick returns.
    // This is NOT a stuck lock — it is temporary contention.  Track the holder
    // address across invocations: if the holder changes, the lock was released
    // and re-acquired — not stuck.
    {
        bool held = scheduler_lock_.try_lock();
        const void *curr_holder = scheduler_lock_.holder();
        if (held) {
            current_cpu().lk0_count = 0;
            current_cpu().last_holder = nullptr;
            scheduler_lock_.unlock();
        } else {
            if (curr_holder != current_cpu().last_holder) {
                // Holder changed — lock was released and re-acquired
                current_cpu().lk0_count = 1;
                current_cpu().last_holder = curr_holder;
            } else {
                ++current_cpu().lk0_count;
            }
            if (current_cpu().lk0_count >= 200) {
                char buf[128];
                int p = 0;
                kernel::debug::fmt_str(buf, p, "[LK-CONTEND] h=");
                p = kernel::debug::fmt_u64(buf, p,
                    (uint64_t)curr_holder);
                kernel::debug::fmt_str(buf, p, " cur=");
                p = kernel::debug::fmt_u64(buf, p,
                    current_task() ? (uint64_t)current_task()->id : 0u);
                kernel::debug::fmt_str(buf, p, " st=");
                p = kernel::debug::fmt_u64(buf, p,
                    current_task()
                        ? (uint64_t)current_task()->state
                        : 99u);
                kernel::debug::fmt_str(buf, p, " sched=");
                p = kernel::debug::fmt_u64(buf, p,
                    (uint64_t)Scheduler::SwSlots::need_resched());
                buf[p] = 0;
                kernel::debug::trace(buf);
                for (auto *t = all_tasks_.first_ptr(); t;
                     t = all_tasks_.next_ptr(t)) {
                    if (t->magic != TaskControlBlock::TCB_MAGIC) continue;
                    char tb[96];
                    int tp = 0;
                    kernel::debug::fmt_str(tb, tp, "  T");
                    tp = kernel::debug::fmt_u64(tb, tp, t->id);
                    kernel::debug::fmt_str(tb, tp, " st=");
                    tp = kernel::debug::fmt_u64(tb, tp,
                        (uint64_t)t->state);
                    kernel::debug::fmt_str(tb, tp, " inrq=");
                    tp = kernel::debug::fmt_u64(tb, tp,
                        t->in_ready_queue_ ? 1u : 0u);
                    tb[tp] = 0;
                    kernel::debug::trace(tb);
                }
                // Log warning but do NOT halt — high contention is not
                // necessarily a stuck lock (e.g. task-context on_tick).
                kernel::debug::trace("[LK-CONTEND] possible lock contention");
                current_cpu().lk0_count = 0;
                current_cpu().last_holder = nullptr;
            }
        }
    }
#endif

    if (!preempt_enabled_) {
        return;
    }

    bool lock_acquired = scheduler_lock_.try_lock();
    if (lock_acquired) {
        SpinLockGuard<sync::SpinLock> guard(scheduler_lock_, adopt_lock);
#if defined(CONFIG_DEBUG_IPC_SCHED)
        // Universal hang detector: if no actual context switch has occurred for
        // STALL_LIMIT ticks while a non-idle task is still runnable, the
        // scheduler is frozen (any of: deferred-switch CR3 race, blocked-in-runq
        // live-lock, stale-trigger self-deadlock).  Dump full state and halt so
        // ONE run yields complete evidence.
        {
            const uint64_t now = arch::Timer::ticks();
            if (current_cpu().last_switch_tick == 0)
                current_cpu().last_switch_tick = now; // prime on first tick
            // A stall is only real if a context switch is actually REQUIRED
            // (needs_switch()) but none has occurred.  The previous predicate
            // ("any non-idle task READY/RUNNING") fired during legitimate
            // long-running kernel tests (e.g. PmmExhaustion's 100k-iteration
            // allocation loop) where the current RUNNING task is the highest
            // effective priority and no preemption is due — that is correct
            // scheduler behavior, not a freeze.  Use needs_switch() so only a
            // genuine failure to dispatch a due task is reported.
            bool runnable = needs_switch();
            const uint64_t since_switch =
                (now > current_cpu().last_switch_tick) ? (now - current_cpu().last_switch_tick) : 0;
            if (runnable && since_switch > 300 && current_cpu().wedge_emitted < 8) {
                ++current_cpu().wedge_emitted;
                char wb[128];
                int wp = 0;
                 kernel::debug::fmt_str(wb, wp, "[STALL] ticks_since_switch=");
                 wp = kernel::debug::fmt_u64(wb, wp, since_switch);
                 kernel::debug::fmt_str(wb, wp, " cur=");
                 wp = kernel::debug::fmt_u64(
                     wb, wp,
                     (current_task()
                          ? static_cast<uint64_t>(current_task()->id)
                          : 0u));
                kernel::debug::fmt_str(wb, wp, " bm_lo=");
                wp = kernel::debug::fmt_u64(
                    wb, wp, rq_own().bitmap().raw_lo());
                kernel::debug::fmt_str(wb, wp, " bm_hi=");
                wp = kernel::debug::fmt_u64(
                    wb, wp, rq_own().bitmap().raw_hi());
                wb[wp] = 0;
                kernel::debug::trace(wb);
                for (auto *t = all_tasks_.first_ptr(); t;
                     t = all_tasks_.next_ptr(t)) {
                    if (t->magic != TaskControlBlock::TCB_MAGIC)
                        continue;
                    char tb[128];
                    int tp = 0;
                    kernel::debug::fmt_str(tb, tp, "  T");
                    tp = kernel::debug::fmt_u64(tb, tp, t->id);
                    kernel::debug::fmt_str(tb, tp, " st=");
                    tp = kernel::debug::fmt_u64(
                        tb, tp, static_cast<uint64_t>(t->state));
                    kernel::debug::fmt_str(tb, tp, " inrq=");
                    tp = kernel::debug::fmt_u64(
                        tb, tp, t->in_ready_queue_ ? 1u : 0u);
                    kernel::debug::fmt_str(tb, tp, " rq=");
                    tp = kernel::debug::fmt_u64(tb, tp, t->rq_priority_);
                    kernel::debug::fmt_str(tb, tp, " eff=");
                    tp = kernel::debug::fmt_u64(
                        tb, tp, effective_priority(t));
                    kernel::debug::fmt_str(tb, tp, " pg=");
                    tp = kernel::debug::fmt_u64(tb, tp, t->page_table_);
                    tb[tp] = 0;
                    kernel::debug::trace(tb);
                }
                kernel::debug::trace("[STALL] HALT");
                arch::cli();
                for (;;)
                    arch::hlt();
            }
        }
        // H2 diagnostic detectors.  Both halt the CPU on catch so a SINGLE run
        // freezes QEMU with the full serial evidence (no brute-forcing).
        {
            uint64_t phys_rsp{};
            phys_rsp = current_sp();
            const uint64_t save_to = reinterpret_cast<uint64_t>(
                __atomic_load_n(&Scheduler::SwSlots::save_rsp_to(), __ATOMIC_ACQUIRE));

            // --- (C) INV-2 desync: a BLOCKED/WAITING task still in the runq
            //     (in_ready_queue_==1).  next_task() will keep selecting it,
            //     producing the live-lock (constant RSP, next=X repeated).
            bool blocked_in_runq = false;
            for (auto *t = all_tasks_.first_ptr(); t;
                 t = all_tasks_.next_ptr(t)) {
                if (t->magic != TaskControlBlock::TCB_MAGIC)
                    continue;
                if (is_idle_task(t))
                    continue;
                if (queue_target(*t) != sched_cpu())
                    continue; // issue #25 C1: own-CPU tasks only (no remote
                              // queue reads — torn metadata false-alarms)
                if (t->state == TaskState::BLOCKED ||
                    t->state == TaskState::WAITING) {
                    if (t->in_ready_queue_) {
                        blocked_in_runq = true;
                        if (current_cpu().wedge_emitted < 8) {
                            char wb[128];
                            int wp = 0;
                            kernel::debug::fmt_str(wb, wp,
                                                  "[WEDGE] blocked-in-runq id=");
                            wp = kernel::debug::fmt_u64(wb, wp, t->id);
                            kernel::debug::fmt_str(wb, wp, " st=");
                            wp = kernel::debug::fmt_u64(
                                wb, wp,
                                static_cast<uint64_t>(t->state));
                            kernel::debug::fmt_str(wb, wp, " inrq=1 phys=");
                            bool phys = false;
                            for (uint64_t p = 0;
                                 p <= CONFIG_PRIORITY_CEILING && !phys; ++p) {
                                if (rq_task(*t).queue(p).contains(*t))
                                    phys = true;
                            }
                            wp = kernel::debug::fmt_u64(wb, wp,
                                                        phys ? 1u : 0u);
                            wb[wp] = 0;
                            kernel::debug::trace(wb);
                        }
                    }
                }
            }

            // --- orphan: READY/RUNNING task flagged in_ready_queue_ but not
            //     physically linked in any priority queue.
            bool orphan_found = false;
            for (auto *t = all_tasks_.first_ptr(); t;
                 t = all_tasks_.next_ptr(t)) {
                if (t->magic != TaskControlBlock::TCB_MAGIC)
                    continue;
                if (is_idle_task(t) || t == current_task())
                    continue;
                if (queue_target(*t) != sched_cpu())
                    continue; // issue #25 C1: own-CPU tasks only
                if (t->state != TaskState::READY &&
                    t->state != TaskState::RUNNING)
                    continue;
                if (!t->in_ready_queue_)
                    continue;
                bool phys = false;
                for (uint64_t p = 0;
                     p <= CONFIG_PRIORITY_CEILING && !phys; ++p) {
                    if (rq_task(*t).queue(p).contains(*t))
                        phys = true;
                }
                if (!phys) {
                    orphan_found = true;
                    if (current_cpu().wedge_emitted < 8) {
                        IPC_SCHED_TRACE(
                            "[WEDGE]", "orphan=", t->id, "st=",
                            static_cast<uint64_t>(t->state), "inrq=",
                            t->in_ready_queue_ ? 1u : 0u, "phys=", 0u);
                    }
                }
            }

            if ((orphan_found || blocked_in_runq) && current_cpu().wedge_emitted < 8) {
                ++current_cpu().wedge_emitted;
                // Full dump then halt so this ONE run is sufficient evidence.
                char wb[128];
                int wp = 0;
                kernel::debug::fmt_str(wb, wp, "[WEDGE] save_to=");
                wp = kernel::debug::fmt_u64(wb, wp, save_to);
                kernel::debug::fmt_str(wb, wp, " cur=");
                wp = kernel::debug::fmt_u64(
                    wb, wp,
                    (current_task()
                         ? static_cast<uint64_t>(current_task()->id)
                         : 0u));
                kernel::debug::fmt_str(wb, wp, " bm_lo=");
                wp = kernel::debug::fmt_u64(
                    wb, wp, rq_own().bitmap().raw_lo());
                kernel::debug::fmt_str(wb, wp, " bm_hi=");
                wp = kernel::debug::fmt_u64(
                    wb, wp, rq_own().bitmap().raw_hi());
                kernel::debug::fmt_str(wb, wp, " physrsp=");
                wp = kernel::debug::fmt_u64(wb, wp, phys_rsp);
                wb[wp] = 0;
                kernel::debug::trace(wb);
                // Per-task summary.
                for (auto *t = all_tasks_.first_ptr(); t;
                     t = all_tasks_.next_ptr(t)) {
                    if (t->magic != TaskControlBlock::TCB_MAGIC)
                        continue;
                    char tb[128];
                    int tp = 0;
                    kernel::debug::fmt_str(tb, tp, "  T");
                    tp = kernel::debug::fmt_u64(tb, tp, t->id);
                    kernel::debug::fmt_str(tb, tp, " st=");
                    tp = kernel::debug::fmt_u64(
                        tb, tp, static_cast<uint64_t>(t->state));
                    kernel::debug::fmt_str(tb, tp, " inrq=");
                    tp = kernel::debug::fmt_u64(
                        tb, tp, t->in_ready_queue_ ? 1u : 0u);
                    kernel::debug::fmt_str(tb, tp, " rq=");
                    tp = kernel::debug::fmt_u64(tb, tp, t->rq_priority_);
                    kernel::debug::fmt_str(tb, tp, " eff=");
                    tp = kernel::debug::fmt_u64(
                        tb, tp, effective_priority(t));
                    kernel::debug::fmt_str(tb, tp, " pg=");
                    tp = kernel::debug::fmt_u64(tb, tp, t->page_table_);
                    tb[tp] = 0;
                    kernel::debug::trace(tb);
                }
                // Only halt for orphans (INV-2 violation: READY/RUNNING task
                // not physically linked).  BLOCKED+inrq is benign — the
                // scheduler handles it via next_task()'s while loop.
                if (!orphan_found) {
                    kernel::debug::trace("[WEDGE] block-in-runq skipped (benign)");
                } else {
                    kernel::debug::trace("[WEDGE] HALT");
                    arch::cli();
                    for (;;)
                        arch::hlt();
                }
            }
        }
#endif
#if CONFIG_DEADLINE_MONITOR_TASK
        // FIX(pret): s_test_active_ disables the deadline-monitor wake and
        // periodic reap_orphans() during test execution.  Without this guard,
        // the reaper (tick 100 reap_orphans below) frees terminated test tasks
        // before the test's ScopeGuard or snapshot_restore can clean them up,
        // causing double-free use-after-free.  Test termination is handled
        // entirely by the harness — the reaper must not race with it.
        if (!is_test_active()) {
            __atomic_store_n(&s_scan_requested_, 1, __ATOMIC_RELEASE);
            // on_tick already holds scheduler_lock_ (acquired at line 548 and
            // released at line 839).  The monitor's block transition also takes
            // scheduler_lock_ (around both dequeue and the BLOCKED store), so
            // this wake's READY+enqueue is mutually exclusive with the monitor's
            // block — no half-blocked task can be re-enqueued (the [WEDGE] INV-5
            // violation).  Do NOT take the lock again here (non-recursive).
            if (s_monitor_task_ &&
                s_monitor_task_->magic == TaskControlBlock::TCB_MAGIC &&
                s_monitor_task_->state == TaskState::BLOCKED) {
                // G2: magic-guarded, provably live — bind a reference
                // (docs/irqguard-ledger.md §G2-A).
                TaskControlBlock &m = *s_monitor_task_;
                m.state = TaskState::READY;
                enqueue_ready(m);
            }
        }
#else
#if CONFIG_DEADLINE_MISS_DETECTION
        // O(1) deadline scan via DeadlineList.  Issue #25 C1: skip tasks
        // not affine to this CPU (re-insert + stop: the list is
        // time-ordered, so later entries expire later — another CPU's
        // tick services them; AP-affine tasks are not serviced in C1).
        while (auto *task = deadline_list_.pop_earliest_if_expired()) {
            if (queue_target(*task) != sched_cpu()) {
                deadline_list_.insert(*task);
                break;
            }
            if (task->get_sporadic_server()) {
                task->ss_state_on_deadline_miss =
                    static_cast<uint8_t>(task->get_sporadic_server()->state());
                task->ss_budget_on_deadline_miss =
                    task->get_sporadic_server()->remaining_budget();
            }
            task->deadline_missed = true;
            ++task->deadline_miss_count;
            deadline_miss_handler(*task,
                                  arch::Timer::ticks() - task->deadline_ticks);
        }
#endif
#endif // CONFIG_DEADLINE_MONITOR_TASK

        // Issue #17: event-timer wheel expiries for this CPU. Pure callee
        // of the tick (no timer programming, no source reads): now_ns
        // comes from the #16 monotonic clock. Bounded pop cap; skipped
        // expiries stay armed for the next tick. Callbacks run under the
        // outer scheduler_lock_: they must be non-blocking and must not
        // take scheduler services or reschedule (audit #17 contract).
        time::TimerWheel::on_tick(arch::Timer::ns_monotonic(),
                                  sched_cpu());

        // Issue #18: bounded-receive timeout apply. Wheel callbacks only
        // set recv_timed_out; the wake is applied here under the
        // already-held scheduler_lock_ (deadline-monitor wake precedent —
        // never take the non-recursive lock again). Guards mirror the
        // accounting walk below (magic, liveness, affinity).
        // Level-triggered (not edge): the wake re-applies every tick until
        // the waiter observes it (resume cancel) or dies (cleanup cancel).
        // A silently dequeued waiter is thus re-queued next tick instead
        // of stranding: enqueue_ready refuses duplicates, so re-apply is
        // idempotent. Never clobber a RUNNING state.
        for (auto *task = all_tasks_.first_ptr(); task;
             task = all_tasks_.next_ptr(task)) {
            if (task->magic != TaskControlBlock::TCB_MAGIC)
                continue;
            if (!__atomic_load_n(&task->recv_timeout_armed,
                                 __ATOMIC_ACQUIRE) ||
                !__atomic_load_n(&task->recv_timed_out, __ATOMIC_ACQUIRE))
                continue;
            if (task->generation != task->recv_timeout_gen)
                continue;
            if (queue_target(*task) != sched_cpu())
                continue;
            if (task->state == TaskState::BLOCKED)
                task->state = TaskState::READY;
            // SIL 3 guard (audit issue #18): never queue a task that is not
            // BLOCKED-turned-READY or already READY. A dispatched waiter is
            // RUNNING with its slot still armed until its resume path runs
            // recv_wait_cancel; queueing it violates the
            // current-never-queued invariant (set_current defense, H2).
            else if (task->state != TaskState::READY)
                continue;
            enqueue_ready(*task);
            __atomic_store_n(&Scheduler::SwSlots::need_resched(), true,
                             __ATOMIC_RELEASE);
        }

        // Issue #76: bounded-nanosleep apply + timerfd waiter wakes. Wheel
        // callbacks only set sleep_expired / waiter_done; the wakes are
        // applied here under the already-held scheduler_lock_ (same
        // contract as the #18 block above). Level-triggered: a missed wake
        // re-applies next tick; enqueue_ready is idempotent. Never clobber
        // a RUNNING state (H2 guard, copied from above).
        for (auto *task = all_tasks_.first_ptr(); task;
             task = all_tasks_.next_ptr(task)) {
            if (task->magic != TaskControlBlock::TCB_MAGIC)
                continue;
            if (!__atomic_load_n(&task->sleep_armed, __ATOMIC_ACQUIRE) ||
                !__atomic_load_n(&task->sleep_expired, __ATOMIC_ACQUIRE))
                continue;
            if (task->generation != task->sleep_gen)
                continue;
            if (queue_target(*task) != sched_cpu())
                continue;
            if (task->state == TaskState::BLOCKED)
                task->state = TaskState::READY;
            else if (task->state != TaskState::READY)
                continue;
            enqueue_ready(*task);
            __atomic_store_n(&Scheduler::SwSlots::need_resched(), true,
                             __ATOMIC_RELEASE);
        }
        // Timerfd waiter wakes live in the PosixTime registry (slots are
        // not TCB fields): same lock position, leaf posix lock only.
        time::PosixTime::reapply_wakes(sched_cpu());

        // Accounting, WCET, alarms — common to both paths.  Issue #25 C1:
        // only tasks affine to this CPU (AP-affine tasks are not serviced).
        // Issue #154: executed_ticks is execution time — only the
        // interrupted (running) task is charged.  remaining_ticks,
        // deadline re-arm and alarms stay wall-clock for every affine
        // task (a parked READY task still consumes period and alarms).
        TaskControlBlock *const running = current_task();
        for (auto *task = all_tasks_.first_ptr(); task;
             task = all_tasks_.next_ptr(task)) {
            if (task->magic != TaskControlBlock::TCB_MAGIC)
                continue;
            if (task->state == TaskState::TERMINATED)
                continue;
            if (queue_target(*task) != sched_cpu())
                continue;

            if (task == running) {
                ++task->executed_ticks;
                charge_exec(*task); // issue #21: ns billing, running task only
            }

            if (task->state == TaskState::RUNNING ||
                task->state == TaskState::READY) {
                uint64_t prev_rem = task->remaining_ticks;
                if (task->remaining_ticks > 0)
                    --task->remaining_ticks;
                if (prev_rem == 0 && task->period_ticks > 0) {
                    task->remaining_ticks = task->period_ticks;
                    // Issue #21: new period, fresh budget clock (this
                    // tick was already billed into the old period above).
                    task->exec_period_ns = 0;
                    // Issue #19: the absolute deadline moved — re-sort a
                    // queued EDF task so dispatch order tracks it.
                    edf_resort(*task);
#if CONFIG_DEADLINE_MISS_DETECTION && !CONFIG_DEADLINE_MONITOR_TASK
                    task->deadline_ticks += task->period_ticks;
                    task->deadline_missed = false;
                    if (task->deadline_ticks > 0) {
                         deadline_list_.insert(*task);
                     }
#if CONFIG_WCET_OVERRUN_DETECTION
                    task->wcet_overrun_fired = false;
#endif
#endif
                }
            }

#if CONFIG_WCET_OVERRUN_DETECTION
            // Issue #154: a starved READY task must never trip the
            // overrun latch — only execution counts toward WCET.
            if (task == running && task->wcet_ticks > 0 &&
                !task->wcet_overrun_fired &&
                task->executed_ticks > task->wcet_ticks) {
                task->wcet_overrun_fired = true;
                wcet_overrun_handler(task,
                                     task->executed_ticks - task->wcet_ticks);
            }
#endif

            if (task->alarm_armed) {
                if (task->alarm_ticks > 0)
                    --task->alarm_ticks;
                if (task->alarm_ticks == 0) {
                    task->alarm_armed = false;
                    task->pending_signals |=
                        (1ULL << static_cast<uint64_t>(Signal::SIGALRM));
                }
            }
        }

        // Issue #172: load-average decay (top triplet).  Integer EMA of
        // the per-tick non-idle sample at LOADAVG_SCALE precision (the
        // ×1000 display scale alone truncates every step to zero).
        // Runs under the already-held scheduler_lock_; additive only, no
        // alloc/log.  Tickless windows skip decay (stale, bounded —
        // documented, not incorrect).
        {
            const uint64_t instant =
                (running && !is_idle_task(running)) ? LOADAVG_SCALE : 0;
            // N = ticks per window at CONFIG_TICK_HZ (guard: HZ is a
            // non-zero build constant; windows ordered 1m < 5m < 15m).
            constexpr uint64_t kTicks1m =
                60ULL * static_cast<uint64_t>(CONFIG_TICK_HZ);
            constexpr uint64_t kTicks5m = 5ULL * kTicks1m;
            constexpr uint64_t kTicks15m = 15ULL * kTicks1m;
            loadavg_1min_ =
                loadavg_step(loadavg_1min_, instant, kTicks1m,
                             LOADAVG_SCALE);
            loadavg_5min_ =
                loadavg_step(loadavg_5min_, instant, kTicks5m,
                             LOADAVG_SCALE);
            loadavg_15min_ =
                loadavg_step(loadavg_15min_, instant, kTicks15m,
                             LOADAVG_SCALE);
        }
#if !CONFIG_DEADLINE_MONITOR_TASK
        __atomic_fetch_add(&deadline_detection_integrity, 1, __ATOMIC_RELEASE);
#endif
    }

    // FIX(sched-race): The deferred-kill flush, sporadic budget management,
    // and zombie reap/flush all mutate scheduler state (ready-queue priority
    // buckets via move_priority, sporadic server state read by
    // effective_priority(), zombie list).  on_tick() normally runs from the
    // timer ISR where IRQs are off, but it is ALSO invoked from task context
    // (tests).  When the scheduler lock was not acquired (a task holds it
    // mid-mutation), these tail sections must not run concurrently with the
    // lock holder.  Gate them on lock_acquired and hold IrqGuard so they are
    // atomic against both nested IRQs and the lock holder's partial writes.
    if (lock_acquired) {
        arch::IrqGuard irq_guard{};

#if defined(CONFIG_SNAPSHOT_CANARY_WATCH)
        // Snapshot-buffer canary watchdog: a stray write into the snapshot
        // region (observed as canary corruption at test ~846 in the `all`
        // suite) is otherwise only detected at the next snapshot_restore —
        // long after the corrupting instruction ran.  Polling here (every
        // tick) narrows the window to one tick and lets us dump the current
        // tick + task at the moment of detection.
        // Gated behind CONFIG_SNAPSHOT_CANARY_WATCH (default off): per-tick
        // reads are unnecessary overhead for normal suites; enable with
        // -DCONFIG_SNAPSHOT_CANARY_WATCH for a targeted corruption run.
        if (kernel::test::snapshot_canary_corrupted()) {
            auto *cc = current_task();
            Logger::raw_write("[SNAP-CANARY] corrupted at tick=");
            Logger::print_dec(current_tick);
            Logger::raw_write(" cur=");
            Logger::print_dec(cc ? cc->id : 0u);
            Logger::raw_write(" st=");
            Logger::print_dec(cc ? static_cast<uint64_t>(cc->state) : 99u);
            Logger::raw_write(" nt=");
            Logger::print_dec(all_tasks_.size());
            Logger::raw_write("\n");
        }
        // Per-tick TCB-magic watchdog (DEBUG): a corrupted current-task TCB is
        // otherwise only caught at the next switch/remove_task.  Detect it
        // here so the corrupting test/tick is attributable.  Skip when
        // all_tasks_ is empty: snapshot_restore() transiently points
        // current_task() at a not-yet-restored TCB with magic==0 while it
        // rewinds the task list (nt==0), which is a false positive.
        if (current_task() && all_tasks_.size() > 0 &&
            current_task()->magic != TaskControlBlock::TCB_MAGIC) {
            Logger::raw_write("[TCB-MAGIC] corrupt current at tick=");
            Logger::print_dec(current_tick);
            Logger::raw_write(" ptr=0x");
            Logger::print_hex(reinterpret_cast<uint64_t>(current_task()));
            Logger::raw_write(" magic=0x");
            Logger::print_hex(current_task()->magic);
            Logger::raw_write(" nt=");
            Logger::print_dec(all_tasks_.size());
            Logger::raw_write("\n");
        }
#endif

        // Issue #92 canary relocation: sample the current user task's segment
        // canaries on a bounded tick cadence (CONFIG_CANARY_SAMPLE_TICKS).
        // Catches corruption for FAST-only tasks that never hit a FULL syscall.
        // Pure reads; test-mode latch; production panic.  This runs inside the
        // scheduler_lock_ + IrqGuard window of on_tick, exactly like the
        // snapshot/TCB-magic watchdogs above.
#if CONFIG_CANARY_GUARD
        {
            auto *cc = current_task();
            if (cc && cc->is_user_ &&
                cc->magic == TaskControlBlock::TCB_MAGIC &&
                (current_tick & (CONFIG_CANARY_SAMPLE_TICKS - 1)) == 0) {
                canary_check_in_scheduler_hooks(cc, 0);
            }
        }
#endif

        // Issue #107: external-pager fault watchdog — expire overdue delegated
        // faults (per-fault fail-closed, registration KEPT, VA poison latch).
        // Runs inside the scheduler_lock_ + IrqGuard window; records mid-map
        // (map_in_progress) are deferred this tick.
        kernel::ipc::PagerRegistry::watchdog_scan(current_tick);

        if (s_deferred_kill_count > 0)
            Scheduler::process_deferred_kills();

        // Sporadic Server budget management.  Issue #25 C1: only tasks
        // affine here (AP-affine sporadic tasks are not serviced).
        {
        auto *cur = current_task();
        uint64_t found = 0;
        for (auto *t = all_tasks_.first_ptr(); t; t = all_tasks_.next_ptr(t)) {
            if (found >= sporadic_task_count_)
                break;
            if (t->magic != TaskControlBlock::TCB_MAGIC)
                continue;
            if (queue_target(*t) != sched_cpu())
                continue;
            if (t->get_sporadic_server()) {
                found++;
                if (reinterpret_cast<uint64_t>(t->get_sporadic_server()) ==
                    0xFFFFFFFFFFFFFFFFULL) {
                    Logger::raw_write("[BUG] on_tick: sporadic_server=-1\n");
                    continue;
                }
// FIX(ss-race): is_poisoned_block checks for a freed TCB whose
                // sporadic_server field has been poisoned (0xDDDD...) by the
                // MemPool free path.  During daemon teardown the sporadic server
                // object may be destroyed while a terminated task still has a
                // stale pointer.  Without this guard the subsequent
                // process_replenishments() call would dereference freed memory.
                // This check is safe because no active task has a valid pointer
                // that matches the poison pattern — the poison value is chosen
                // to be outside any valid allocation.
                if (is_poisoned_block(t->get_sporadic_server())) {
                    continue;
                }
                // Ownership contract: the getter returns the O(1) read-cache;
                // teardown is serialized against on_tick by scheduler_lock_,
                // and effective_priority() guards the pointer with
                // is_poisoned_block() + the canonical-address check, so no
                // ScopedRef is needed on this hot path (it would add two
                // atomics per sporadic task per tick, perturbing the
                // timing-sensitive H2 race — ROADMAP §v0.3.9).
                task::SporadicServer *ss = t->get_sporadic_server();
                // FIX(rms-o1): Effective priority may change after replenishment
                // (e.g. budget restored → sporadic server priority rises).
                // The O(1) queue does not re-derive position from tcb.priority,
                // so we MUST call move_priority explicitly.  Skipping this leaves
                // the task at its depleted (lower) bucket, starving it until the
                // next reschedule, which may never come if the task is blocked.
                // Skip when t == cur — the current task is not in the ready
                // queue and move_priority on a non-enqueued task is undefined.
                {
                    uint64_t old_eff = effective_priority(t);
                    ss->process_replenishments(current_tick);
                    uint64_t new_eff = effective_priority(t);
                    // Issue #19: route through Scheduler::move_priority so
                    // EDF-queued tasks re-sort instead of bitmap-moving.
                    if (old_eff != new_eff && t != cur)
                        Scheduler::move_priority(*t, old_eff, new_eff);
                }
                if (t == cur && ss->is_active()) {
                    if (!ss->consume(current_tick)) {
#if CONFIG_SPORADIC_SERVER_EXHAUSTION_IS_DEADLINE
                        if (!t->deadline_missed) {
                            t->ss_state_on_deadline_miss =
                                static_cast<uint8_t>(ss->state());
                            t->ss_budget_on_deadline_miss =
                                ss->remaining_budget();
                            t->deadline_missed = true;
                            ++t->deadline_miss_count;
                            deadline_miss_handler(*t, 0);
                        }
#endif
                        uint64_t rsp{};
                        rsp = current_sp();
                        uint64_t base =
                            reinterpret_cast<uint64_t>(cur->kernel_stack);
                        if (cur->kernel_stack && cur->kernel_stack_top &&
                            rsp >= base && rsp < cur->kernel_stack_top) {
                            Scheduler::reschedule();
                        }
                    }
                }
            }
        }
    }

    static uint64_t tick_counter = 0;
    ++tick_counter;
// FIX(pret): reap_orphans runs every 100 ticks but is gated on
    // !s_test_active_ to prevent a UAF race with test ScopeGuards.  The test
    // harness owns its task lifecycle — it calls remove_task+cleanup+delete in
    // the ScopeGuard.  If the reaper frees the task first, the ScopeGuard's
    // delete double-frees the MemPool block.  Test tasks are short-lived and
    // few, so deferring reaping to the post-test snapshot_restore is safe and
    // avoids the race entirely.  The 100-tick batching ensures ammortised O(1)
    // cost in production while not being so infrequent that zombie count grows
    // unbounded (CONFIG_MAX_TASKS=64 caps the worst case).
        if (tick_counter % 100 == 0) {
            if (!is_test_active())
                reap_orphans();
            // ZombieList watchdog: force-flush ONLY when the zombie list has
            // outgrown the starvation limit (idle hasn't kept up).  Gating on
            // zcount > LIMIT keeps small zombie populations (e.g. 1-2 test
            // tasks still referenced by a test ScopeGuard) alive until the
            // post-test snapshot_restore drains them — flushing them early
            // frees the TCB while the ScopeGuard still holds the pointer,
            // producing a use-after-free (magic corruption) in tests like
            // preemption_under_syscall.
            uint64_t zcount = __atomic_load_n(&zombie_count_, __ATOMIC_RELAXED);
            if (zcount > CONFIG_ZOMBIE_STARVATION_LIMIT) {
                flush_zombies(CONFIG_ZOMBIE_STARVATION_LIMIT / 2);
            }
            daemon::restart_stale_daemons();
        }
    }  // end: gated tail sections (lock_acquired && IrqGuard)

    // Issue #61: RT load-balancer cadence (BSP tick only; production
    // only — tests drive balancer_tick() directly under IrqGuard so
    // placement asserts stay deterministic).
    static uint64_t balancer_cadence = 0;
    ++balancer_cadence;
    if (!is_test_active() &&
        (balancer_cadence % BALANCER_TICK_PERIOD) == 0) {
        balancer_tick();
    }

    // Issue #158: quarantine timeout poll (BSP tick; internally
    // try-locked + bounded, safe to call every tick; no-op when empty).
    TlbShootdown::on_tick(arch::Timer::ticks());

    rate_monotonic_schedule();
}

// ---------------------------------------------------------------------------
// reap_orphans
// ---------------------------------------------------------------------------

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
        if (t->magic != TaskControlBlock::TCB_MAGIC)
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
        if (!can_reap) {
            bool parent_found = false;
            for (TaskIter it(0);;) {
                auto *p = it.next();
                if (!p)
                    break;
                if (p->id == t->parent_id) {
                    parent_found = true;
                    bool terminated = (p->state == TaskState::TERMINATED);
                    bool waiting_for_this =
                        (p->waiting_child_pid == t->id);
                    bool waiting_for_any =
                        (p->waiting_child_pid ==
                         static_cast<uint64_t>(-1));
                    // Reap unless the (live) parent is blocked specifically in
                    // waitpid for *this* child (deferred reap) or for *any*
                    // child (sentinel -1).
                    can_reap =
                        terminated || (!waiting_for_this && !waiting_for_any);
                    break;
                }
            }
            if (!parent_found)
                can_reap = true;
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
        if (t == idle_task_) {
            auto *created = TaskControlBlock::create(
                kernel::integrity::idle_task_main, 0,
                TaskControlBlock::NO_PERIOD);
            if (created) {
                created->state = TaskState::READY;
                if (!suppress_terminated_log_)
                    Logger::info("Scheduler: task '%s' (ID=%u) terminated",
                                 t->name, t->id);
                t->cleanup();
                MemPool::free(t);
                new_idle = created;
            } else {
                if (!suppress_terminated_log_)
                    Logger::warn("Scheduler: idle recreate OOM — keeping old idle");
                all_tasks_.append(*t);
                ENSURE(id_table_insert(t->id, t) &&
                       "id_table full in reap (idle restore)");
            }
        } else {
            if (!suppress_terminated_log_)
                Logger::info("Scheduler: task '%s' (ID=%u) terminated", t->name,
                             t->id);
            t->cleanup();
            MemPool::free(t);
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
// Corruption / validation helpers
// ---------------------------------------------------------------------------

static void report_corruption(const char *label) {
    (void)label;
    __atomic_fetch_add(&scheduler_corruption_count, 1, __ATOMIC_RELEASE);
#ifdef CONFIG_DEBUG
    ENSURE(false && "scheduler corruption detected — see log above");
#endif
}

static bool rsp_in_stack_range(uint64_t rsp, const TaskControlBlock *t,
                               const char *label) {
    auto base = reinterpret_cast<uint64_t>(t->kernel_stack);
    auto top = t->kernel_stack_top;
    if (rsp >= base && rsp <= top)
        return true;
    if (rsp < base)
        return false;
    Logger::raw_write("[SCHED] ");
    Logger::raw_write(label);
    Logger::raw_write(": task id=");
    Logger::print_dec(t->id);
    Logger::raw_write(" rsp=0x");
    Logger::print_hex(rsp);
    Logger::raw_write(" above stack top 0x");
    Logger::print_hex(top);
    Logger::raw_write("\n");
    return false;
}

/// @brief Returns true when the live RSP belongs to the kernel boot stack
///        (kernel-image space), i.e. the physically-running harness.
static inline bool is_boot_stack_rsp(uint64_t rsp) noexcept {
    const uint64_t base = reinterpret_cast<uint64_t>(_stack_start);
    const uint64_t end = reinterpret_cast<uint64_t>(_stack_end);
    return rsp >= base && rsp < end;
}

/// @brief Restore scheduler state after a deferred-switch arm is cleared
///        WITHOUT being applied (CLR-RMS / CLR-SET / CLR-MISC symmetry).
///
/// switch_to_task() performs two side effects on the preempted current task
/// before publishing the arm (scheduler.cpp:2176-2178): it sets the current
/// task READY and enqueues it (when it was RUNNING or on the boot stack), and
/// sets the target RUNNING.  When the arm is later cleared instead of applied,
/// those side effects must be undone, otherwise the physically-running harness
/// (boot-stack current) is left `state=READY, in_ready_queue_=true` — INV-4 —
/// so next_task() skips it and falls through to idle (the H2 residual hang).
///
/// Caller must hold scheduler_lock_ or have IRQs disabled.  All runq ops used
/// here are lock-free and idempotent (TaskQueue::remove early-returns for a
/// node not physically in the queue; enqueue_ready refuses double-enqueues),
/// matching the drop_arm path (scheduler.cpp:3054-3074).
///
/// @param current        The preempted current task (the physical runner).
/// @param armed_target_id The `scheduler_next_task_id` captured BEFORE the
///                        atoms were cleared; UINT64_MAX if none.
static void restore_preempted_current(TaskControlBlock *current,
                                      uint64_t armed_target_id) noexcept {
    if (current && current->magic == TaskControlBlock::TCB_MAGIC) {
        if (current->in_ready_queue_) {
            Scheduler::dequeue_ready(*current);
        }
        // Only undo switch_to_task's publish side effect (READY) — never
        // resurrect a TERMINATED or BLOCKED current (self-terminated trampoline
        // task or a blocked-impersonated peer); forcing those to RUNNING would
        // re-enqueue them at the next switch_to_task and dispatch a zombie.
        if (current->state == TaskState::READY) {
#ifdef CONFIG_DEBUG
            if (current == Scheduler::get_harness_task()) {
                kernel::debug::trace("[H2-RESTORE] harness READY->RUNNING armed=");
                // trace() takes one uint64; format the id into a small buffer.
                char buf[24];
                int p = 0;
                kernel::debug::fmt_u64(buf, p, armed_target_id);
                buf[p] = 0;
                kernel::debug::trace(buf);
            }
#endif
            current->state = TaskState::RUNNING;
        }
    }
    // Re-enqueue the armed target (undo switch_to_task's RUNNING + the dequeue
    // next_task() performed when selecting it) so it is not stranded (INV-2).
    if (armed_target_id != UINT64_MAX && armed_target_id != 0) {
        auto *target = Scheduler::find_task(armed_target_id);
        if (target && target != current && !Scheduler::is_idle_task(target) &&
            target->magic == TaskControlBlock::TCB_MAGIC &&
            (target->state == TaskState::READY ||
             target->state == TaskState::RUNNING)) {
            Scheduler::set_task_ready(*target);
        }
    }
}

static bool validate_switch(TaskControlBlock *current, TaskControlBlock *next,
                            const char *label) {
    if (!current) {
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": current=null\n");
        return false;
    }
    if (!next) {
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": next=null\n");
        return false;
    }
    if (current->magic != TaskControlBlock::TCB_MAGIC) {
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": current magic invalid\n");
        return false;
    }
    auto caddr = reinterpret_cast<uint64_t>(current);
    auto naddr = reinterpret_cast<uint64_t>(next);
    if (caddr < 0xFFFF800000000000ULL) {
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": current low 0x");
        Logger::print_hex(caddr);
        Logger::raw_write("\n");
        return false;
    }
    if (naddr < 0xFFFF800000000000ULL) {
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": next low 0x");
        Logger::print_hex(naddr);
        Logger::raw_write("\n");
        return false;
    }
    if (current->magic != TaskControlBlock::TCB_MAGIC) {
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": current magic=0x");
        Logger::print_hex(current->magic);
        Logger::raw_write("\n");
        return false;
    }
    if (next->magic != TaskControlBlock::TCB_MAGIC) {
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": next magic=0x");
        Logger::print_hex(next->magic);
        Logger::raw_write("\n");
        return false;
    }
    if (!Scheduler::is_idle_task(next) &&
        !rsp_in_stack_range(task_stack_ptr(next), next, label)) {
        if (task_stack_ptr(next) <
            reinterpret_cast<uint64_t>(next->kernel_stack)) {
            return true;
        }
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": current id=");
        Logger::print_dec(current->id);
        Logger::raw_write(" rsp=0x");
        Logger::print_hex(task_stack_ptr(current));
        Logger::raw_write(" state=");
        Logger::print_dec(static_cast<uint64_t>(current->state));
        Logger::raw_write(" kstack=[0x");
        Logger::print_hex(reinterpret_cast<uint64_t>(current->kernel_stack));
        Logger::raw_write("-0x");
        Logger::print_hex(current->kernel_stack_top);
        Logger::raw_write("]\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// switch_to_task
// ---------------------------------------------------------------------------

#if defined(CONFIG_ARCH_X86_64) && CONFIG_PCID
/// @brief Lazily assign a user PCID at first CR3 publish (issue #156).
///        Single choke point covering create/clone/elf births uniformly
///        (deviation D1 from the creation-site plan: the elf::load path
///        also births user address spaces).  Runs under scheduler_lock_
///        at both publish sites; fires once per task lifetime (later
///        publishes see nonzero pcid_).  Returns 0 when unsupported so
///        the publish stays raw-phys (full flush, today's behavior).
static uint16_t publish_pcid_for(TaskControlBlock &next) noexcept {
    if (next.is_user_ && next.pcid_ == 0 && arch::pcid_supported())
        next.pcid_ = arch::pcid_alloc();
    return next.pcid_;
}
#endif

static void switch_to_task(TaskControlBlock *current, TaskControlBlock &next,
                           sync::SpinLock *held_lock = nullptr) {
    auto release_lock = [&]() {
        if (held_lock) {
            held_lock->unlock();
            held_lock = nullptr;
        }
    };

    if (!validate_switch(current, &next, "switch")) {
        report_corruption("switch");
        release_lock();
        return;
    }
    if (next.state != TaskState::READY && next.state != TaskState::RUNNING) {
        report_corruption("switch next state");
        release_lock();
        return;
    }
    if (current == &next) {
        release_lock();
        return;
    }
    // Issue #21: bill the outgoing task's quantum, stamp the incoming one.
    // Both run under the caller's scheduler_lock_/IRQ-off discipline, so the
    // add+restamp pairs are atomic — no double-charge, no lost quantum.
    charge_exec(*current);
    stamp_exec(next);

    uint64_t *save_target = &task_stack_ptr(current);
    bool cur_is_boot_stack = false;
    {
        uint64_t cur_rsp{};
        cur_rsp = current_sp();
        TaskControlBlock *owner = nullptr;
        const uint64_t cbase = reinterpret_cast<uint64_t>(current->kernel_stack);
        const bool cur_in_own_stack =
            current->kernel_stack && current->kernel_stack_top &&
            cur_rsp >= cbase && cur_rsp < current->kernel_stack_top;
        // Stack low-water mark: track the deepest RSP ever observed on this
        // task's kernel stack (grows down).  Sampled on every switch-out so
        // the `tasks` command can report high-water (used) / low-water (free).
        if (cur_in_own_stack &&
            (current->kstack_low_water_ == 0 ||
             cur_rsp < current->kstack_low_water_)) {
            current->kstack_low_water_ = cur_rsp;
        }
        if (is_boot_stack_rsp(cur_rsp)) {
            // H2 (docs/specs/ipc.md §4): the live RSP is on the kernel
            // boot stack — the harness (init/PID 1) runs there in test mode and
            // never switched onto its TCB kernel_stack, so NO TCB kernel_stack
            // covers this RSP.  Owner-resolution must NOT scan peers here: if
            // current_task() has drifted onto a peer TCB, saving into
            // `&task_stack_ptr(current)` would write the boot-stack RSP into the
            // peer's context.rsp (deferred-switch corruption).  Bind the save to
            // the harness TCB explicitly; the boot stack belongs to the harness,
            // not to any peer.
            auto *h = Scheduler::get_harness_task();
            owner = (h && h->magic == TaskControlBlock::TCB_MAGIC) ? h : current;
            cur_is_boot_stack = true;
        } else if (!cur_in_own_stack) {
            // The live RSP is not on the current task's own kernel stack.  Scan
            // all tasks for the real owner (drift correction: current_task()
            // may point at a peer TCB while the CPU actually runs on another
            // task's stack).
            for (uint64_t ti = 0; ti < Scheduler::task_count(); ++ti) {
                auto *tt = Scheduler::task_at(ti);
                if (!tt || tt->magic != TaskControlBlock::TCB_MAGIC)
                    continue;
                uint64_t tb = reinterpret_cast<uint64_t>(tt->kernel_stack);
                if (tt->kernel_stack && tt->kernel_stack_top &&
                    cur_rsp >= tb && cur_rsp < tt->kernel_stack_top) {
                    owner = tt;
                    break;
                }
            }
            if (owner == nullptr) {
                // The RSP sits on a foreign stack owned by NO TCB — the harness
                // (PID 1) running in test mode on a non-TCB boot stack (e.g.
                // outside the linker .boot_stack section).  Bind the save to the
                // harness and mark it for re-enqueue below so it is not stranded
                // (INV-2) when preempted.
                auto *h = Scheduler::get_harness_task();
                owner = (h && h->magic == TaskControlBlock::TCB_MAGIC) ? h
                                                                       : current;
                cur_is_boot_stack = true;
            }
        }
        if (owner && owner != current) {
            current = owner;
            Scheduler::set_current(*owner);
        }
        // H2 root fix (ROADMAP §v0.4.0): the owner-resolution above can
        // correct `current` to the PHYSICAL runner while `next` is that same
        // task — a condition that was already checked at entry (see above) but
        // can become true HERE, after the check.  Publishing a deferred switch
        // in this state is a SELF-switch: the ISR would save a fresh RSP into
        // the runner's context.rsp (save_target == &task_stack_ptr(next)) and
        // then iretq the PRE-SAVE (stale) load_rsp_from value, displacing the
        // runner onto a stale/fossil iret frame (docs/specs/ipc.md §4 H2).  No-op
        // instead: the runner keeps executing.  Mirror the bad-frame no-op's
        // queue-membership cleanup (state=RUNNING, out of the runq) so an
        // INV-4 leftover cannot strand it.
        if (current == &next) {
            next.state = TaskState::RUNNING;
            next.in_ready_queue_ = false;
            next.rq_priority_ = 0;
            release_lock();
            return;
        }
        // H2: keep the resolved owner's context.rsp LIVE.  The original
        // scratch-save (layer 2) sent the harness's RSP to
        // s_foreign_rsp_scratch whenever it was not detected on the linker
        // boot stack, permanently freezing context.rsp at the FIRST switch-out
        // frame (e.g. arch_hlt in the daemon wait).  A later deferred switch
        // TO the harness then iretq'd it onto that stale frame, freezing the
        // suite (observed at `all` test 78 / 477, priority_inheritance).  The
        // harness's live RSP IS its true context regardless of which region it
        // runs on — always save it into the owner's own context.rsp.  The
        // apply-side liveness + ownership re-check
        // (scheduler_validate_pending_switch) and the dispatch-guard both
        // reject a stale/foreign arm before it can iretq, so keeping
        // context.rsp current is safe.
        save_target = &task_stack_ptr(current);
#ifdef CONFIG_DEBUG
        // H2 residual-race recorder (debug-only, fires ONLY on the orphaned
        // displacement — the harness physically executing on a non-boot-stack,
        // non-TCB page — never on the normal linker-boot-stack phase).  The
        // iret frame at cur_rsp+136..168 is the CPU-pushed pre-interrupt frame:
        // rip/cs/rflags/rsp/ss of the displaced execution.
        if (cur_is_boot_stack && !is_boot_stack_rsp(cur_rsp)) {
            static bool s_h2w_fired = false;
            if (!s_h2w_fired) {
                s_h2w_fired = true;
                Logger::raw_write("[H2W] orphan-displaced tick=");
                Logger::print_dec(current_cpu().ticks);
                Logger::raw_write(" cur_rsp=0x");
                Logger::print_hex(cur_rsp);
                Logger::raw_write(" ctx_rsp=0x");
                Logger::print_hex(task_stack_ptr(current));
                Logger::raw_write(" callsite=0x");
                Logger::print_hex(
                    reinterpret_cast<uint64_t>(__builtin_return_address(0)));
                Logger::raw_write(" kst=0x");
                Logger::print_hex(
                    reinterpret_cast<uint64_t>(current->kernel_stack));
                Logger::raw_write("-0x");
                Logger::print_hex(current->kernel_stack_top);
                Logger::raw_write("\n");
                const uint64_t *s =
                    reinterpret_cast<const uint64_t *>(cur_rsp);
                for (unsigned i = 0; i < 56; ++i) {
                    Logger::raw_write("  [rsp+");
                    Logger::print_hex(static_cast<uint64_t>(i * 8));
                    Logger::raw_write("]=0x");
                    Logger::print_hex(s[i]);
                    Logger::raw_write("\n");
                }
                // The harness's stored kslot iret frame — the rsp field at
                // +160 is what the NEXT dispatch's iretq would load.
                const uint64_t *cf =
                    reinterpret_cast<const uint64_t *>(task_stack_ptr(current));
                Logger::raw_write("  ctx-frame: rip=0x");
                Logger::print_hex(cf[136 / 8]);
                Logger::raw_write(" cs=0x");
                Logger::print_hex(cf[144 / 8]);
                Logger::raw_write(" rflags=0x");
                Logger::print_hex(cf[152 / 8]);
                Logger::raw_write(" rsp=0x");
                Logger::print_hex(cf[160 / 8]);
                Logger::raw_write(" ss=0x");
                Logger::print_hex(cf[168 / 8]);
                Logger::raw_write("\n");
#if defined(CONFIG_ARCH_X86_64)
                // Walk the harness's kslot stack PTE chain (read via the
                // direct map) to see WHICH phys the kslot VA maps right now.
                // The CR3/PM4L walk is x86_64-specific.
                uint64_t cr3 = 0;
                asm volatile("mov %%cr3, %0" : "=r"(cr3));
                uint64_t kva = reinterpret_cast<uint64_t>(current->kernel_stack);
                const uint64_t HHDM = 0xFFFF800000000000ULL;
                uint64_t l4e = *reinterpret_cast<const uint64_t *>(
                    HHDM + (cr3 & 0xFFFFFFFFFF000ULL) +
                    ((kva >> 39) & 0x1FF) * 8);
                uint64_t l3e = *reinterpret_cast<const uint64_t *>(
                    HHDM + (l4e & 0xFFFFFFFFFF000ULL) +
                    ((kva >> 30) & 0x1FF) * 8);
                uint64_t l2e = *reinterpret_cast<const uint64_t *>(
                    HHDM + (l3e & 0xFFFFFFFFFF000ULL) +
                    ((kva >> 21) & 0x1FF) * 8);
                uint64_t l1e = *reinterpret_cast<const uint64_t *>(
                    HHDM + (l2e & 0xFFFFFFFFFF000ULL) +
                    ((kva >> 12) & 0x1FF) * 8);
                uint64_t kphys = l1e & 0xFFFFFFFFFF000ULL;
                Logger::raw_write("  kslot-kva=0x");
                Logger::print_hex(kva);
                Logger::raw_write(" maps-phys=0x");
                Logger::print_hex(kphys);
                Logger::raw_write(" orphan-phys=0x");
                Logger::print_hex(cur_rsp - HHDM);
                Logger::raw_write(" SAME=");
                Logger::print_dec(kphys == (cur_rsp & ~0xFFFULL) ? 1u : 0u);
                Logger::raw_write("\n");
#endif // CONFIG_ARCH_X86_64
            }
        }
#endif
    }

#ifdef CONFIG_DEBUG
    {
        auto &ring = current->debug_switch_ring;
        auto &idx = current->debug_switch_idx;
        auto &rec = ring[idx % TaskControlBlock::DEBUG_SWITCH_RING_SIZE];
        rec.entry_addr =
            reinterpret_cast<uint64_t>(__builtin_return_address(0));
#if defined(CONFIG_ARCH_X86_64)
        rec.exit_rip = current->context.rip;
#elif defined(CONFIG_ARCH_AARCH64)
        rec.exit_rip = current->context.elr_el1;
#endif
        rec.regs = current->context;
        rec.thread_id = current->id;
        rec.consumed_ticks = current->executed_ticks;
        ++idx;
    }
#endif
    {
        uint64_t nsp = task_stack_ptr(&next);
        uint64_t nbase = reinterpret_cast<uint64_t>(next.kernel_stack);
        uint64_t npg = reinterpret_cast<uint64_t>(next.page_table_);
        // The harness's context.rsp may be a LIVE boot-stack RSP (its genuine
        // test-mode stack, kept current by the boot-stack save in
        // switch_to_task) rather than a TCB kernel-stack address — that is
        // valid, not bad.  Exempt it from the initial nsp-vs-kernel_stack
        // check; frame_ok() below still validates the frame fields (with its
        // own harness boot-stack allowance).
        bool harness_boot_ctx =
            (&next == Scheduler::get_harness_task() && nsp != 0 &&
             nsp >= reinterpret_cast<uint64_t>(_stack_start) &&
             nsp < reinterpret_cast<uint64_t>(_stack_end));
        bool bad =
            (!nsp ||
             (!harness_boot_ctx &&
              (nsp < nbase || nsp >= next.kernel_stack_top)) ||
              (npg != 0 && (npg & 0xFFF) != 0));
        if (!bad) {
            // riscv64 has no frame-validation branch below (x86_64 iret /
            // aarch64 ELR+SPSR only) — the pointer is unused there.
            [[maybe_unused]] const uint64_t *f =
                reinterpret_cast<const uint64_t *>(nsp);
#if defined(CONFIG_ARCH_X86_64)
            uint64_t f_rflags = 0;
            // The iret frame sits above the saved register frame.  Two valid
            // layouts exist: a freshly-created task (task.cpp builds rip first)
            // and a task saved by isr_common (CPU order: ss first).  Both place
            // rflags at +152; rip/cs/rsp/ss are swapped between the two
            // orderings, so validate either one instead of assuming a single
            // fixed order (the wrong order made valid RUN-task frames look
            // corrupt and dropped their switch — wedging the `all` suite).
            uint64_t rip_a = f[IRET_RIP_IDX], cs_a = f[IRET_CS_IDX],
                     rsp_a = f[IRET_RSP_IDX],
                     ss_a = f[IRET_SS_IDX]; // created order (rip first)
            uint64_t rip_b = f[IRET_SS_IDX], cs_b = f[IRET_RSP_IDX],
                     rsp_b = f[IRET_CS_IDX],
                     ss_b = f[IRET_RIP_IDX]; // isr_common/CPU order (ss first)
            f_rflags = f[IRET_RFLAGS_IDX];
            if (!validate_iret_frame(next, nbase, f_rflags, rip_a, cs_a, rsp_a,
                                     ss_a) &&
                !validate_iret_frame(next, nbase, f_rflags, rip_b, cs_b, rsp_b,
                                     ss_b))
                bad = true;
#elif defined(CONFIG_ARCH_AARCH64)
            // AArch64 saved/initial context is the vectors.S save area:
            // x0-x30 (+0..240), sp_el0 (+248), elr_el1 (+256), spsr_el1
            // (+264).  Validate the exception-return state instead of an
            // x86 iret frame: ELR must be non-zero and SPSR must encode a
            // sane AArch64 exception-return mode (EL0t = 0b0000 or
            // EL1h = 0b0101) — a garbage frame (zeroed fresh stack) must
            // never be dispatched.
            uint64_t elr = f[256 / 8];
            uint64_t spsr = f[264 / 8];
            uint64_t mode = spsr & 0xFULL;
            if (elr == 0 || (mode != 0x0 && mode != 0x5)) {
                bad = true;
            }
#endif
        }
        if (bad) {
            // ---- Dispatch guard ----
            // Never iretq into a task whose iret frame is invalid.  This can
            // happen when `next` is the physically-running task but
            // current_task() has drifted onto a peer TCB (the running task
            // was never switched out, so its stack slot at context.rsp+136 is
            // live data, not a CPU-written iret frame).  In that case treat it
            // as a no-op self-switch: keep the physical runner going and correct
            // current_task().  Otherwise skip the dispatch and let the
            // current task continue (the bad task stays queued and is retried
            // once it has a real iret frame).
            uint64_t phys_rsp{};
            phys_rsp = current_sp();
            uint64_t nb = reinterpret_cast<uint64_t>(next.kernel_stack);
            bool next_is_runner =
                (&next == current) ||
                (next.kernel_stack && next.kernel_stack_top &&
                 phys_rsp >= nb && phys_rsp < next.kernel_stack_top);
            if (next_is_runner) {
                // `next` IS the physically-running task (current or a task whose
                // stack the live RSP sits on) but current_task() has drifted
                // onto a peer TCB.  Treat as a no-op self-switch: keep the
                // physical runner going and clear its queue membership so
                // next_task() does not exclude it as `current` and fall through
                // to idle (which would permanently starve the live runner).  A
                // RUNNING task has no valid iret frame (its stack slot at
                // context.rsp+136 is live data), so we must NOT iretq into it —
                // just keep it running.  The cache is NOT updated here (Rule 4):
                // the timer ISR sets it via set_current_task after the real swap
                // lands.
                next.state = TaskState::RUNNING;
                next.in_ready_queue_ = false;
                next.rq_priority_ = 0;
            } else {
                // D2 fix (INV-2 / VIOL-5): next_task() already dequeued `next`
                // from the runq.  The old comment claimed "the bad task stays
                // queued and is retried", but it is NOT queued anymore — it was
                // dequeued at scheduler.cpp:395.  Without re-enqueueing here it
                // becomes READY + in_ready_queue_=false + not in any bucket and
                // is stranded forever (the next_task() lazy rebuild only runs
                // when dequeue_highest returns null).  Put it back so it stays
                // eligible and is retried once it has a real iret frame.  Never
                // re-enqueue the idle task (it is the default fallback) or the
                // current physical runner.
                if (!Scheduler::is_idle_task(&next) && &next != current) {
                    Scheduler::set_task_ready(next);
                }
            }
            release_lock();
            return; // do not set scheduler_load_rsp_from -> no switch
        }
    }
    __atomic_store_n(&Scheduler::SwSlots::load_rsp_from(), task_stack_ptr(&next),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_kstack_base(),
                     reinterpret_cast<uint64_t>(next.kernel_stack),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_kstack_top(), next.kernel_stack_top,
                     __ATOMIC_RELEASE);
    if (next.page_table_) {
        // Issue #156: publish tagged (PCID 0 = raw phys, full flush).
        uint64_t cr3_value = next.page_table_;
#if defined(CONFIG_ARCH_X86_64) && CONFIG_PCID
        cr3_value = arch::pcid_tag(next.page_table_,
                                   publish_pcid_for(next));
#endif
        __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), cr3_value,
                         __ATOMIC_RELEASE);
        // Issue #74: publish the TLS base alongside CR3 (0 for unset;
        // kernel/harness tasks publish via the else branch below).
        __atomic_store_n(&Scheduler::SwSlots::load_tls_from(),
                         next.is_user_ ? next.tls_base_ : 0, __ATOMIC_RELEASE);
#if defined(CONFIG_DEBUG_IPC_SCHED)
        {
            auto *c = Scheduler::current_task();
            IPC_SCHED_TRACE("[SW]", "cur=", c ? c->id : 0u, "next=", next.id,
                            "rsp=", (uint64_t)task_stack_ptr(&next), "x=", 0u);
        }
#endif
#if defined(CONFIG_ARCH_X86_64)
        // Issue #25 C1: CPU0-gated (shared TSS; AP runs kernel tasks only).
        // (File-static context: arch::cpu_index() directly.)
        if (arch::cpu_index() == 0)
            arch::GDT::set_tss_rsp0(next.kernel_stack_top);
#endif
    } else {
        __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), VMM::get_kernel_pml4(),
                         __ATOMIC_RELEASE);
        // Issue #74: kernel/harness targets never carry TLS (apply loads
        // nothing); publish 0 so no stale user base survives.
        __atomic_store_n(&Scheduler::SwSlots::load_tls_from(), (uint64_t)0,
                         __ATOMIC_RELEASE);
#if defined(CONFIG_DEBUG_IPC_SCHED)
        {
            auto *c = Scheduler::current_task();
            IPC_SCHED_TRACE("[SW]", "cur=", c ? c->id : 0u, "next=", next.id,
                            "rsp=", (uint64_t)task_stack_ptr(&next), "x=", 0u);
        }
#endif
    }

    // Apply the target task's I/O permission bitmap (issue #3).  No-op for
    // kernel tasks and when the owner is unchanged.
    arch::iopb_switch_to(next);

    // Re-enqueue the current task if it must remain schedulable.  A task with
    // state RUNNING is always re-queued (normal preemption).  The boot-stack
    // harness is a special case: its TCB state can be READY while it physically
    // runs on the boot stack (it was never dispatched via a switch_to_task that
    // set RUNNING — snapshot restore leaves it READY).  If it is preempted
    // without re-enqueueing, it is stranded (INV-2: live but not in the runq
    // and not current after the switch) and next_task() falls through to idle,
    // freezing the `all` suite.  enqueue_ready() refuses double-enqueues, so
    // this is safe even if the harness is already queued.

#if CONFIG_CANARY_GUARD
    // v0.4.0 MP-3: verify the current task's kernel-stack canary on every
    // context switch — pure read at kernel_stack[0..8), already under the
    // scheduler lock.  A mismatch means the stack base was overwritten (deep
    // stack growth / corruption): controlled panic, never silent corruption.
    // Runs BEFORE the deferred-switch arm (H2) so the panic cannot be masked.
    if (!canary_verify_kernel_stack(current)) {
        kernel::Logger::fatal(
            "CANARY TRIP (kernel stack): task '%s' id=%u top=0x%lx",
            current->name, current->id,
            current->kernel_stack_top);
        panic("kernel-stack canary violated");
    }
#endif

    if (current->state == TaskState::RUNNING || cur_is_boot_stack) {
        current->state = TaskState::READY;
        Scheduler::enqueue_ready(*current);
    }
    next.state = TaskState::RUNNING;
    // Issue #172: snapshot dispatch placement for cpuinfo/top.  Single
    // writer (this CPU dispatching next), release-store; shell readers
    // use acquire loads.  Placed at the single real-dispatch commit
    // point (self-switch no-ops above return early and keep old value).
    __atomic_store_n(&next.running_on_cpu, Scheduler::sched_cpu(),
                     __ATOMIC_RELEASE);

    {
        arch::IrqGuard ig{};
        release_lock();
        // H2 ring: an ARM published while a previous arm is still pending
        // (save_rsp_to != 0) strands the previous arm's dequeued target; an
        // IDLE arm aimed at the harness strands the harness itself.
        H2_REC(H2_EV_ARM, next.id,
               __atomic_load_n(&Scheduler::SwSlots::load_rsp_from(), __ATOMIC_RELAXED),
               reinterpret_cast<uint64_t>(save_target));
        if (&next == Scheduler::own_idle() &&
            current == Scheduler::get_harness_task()) {
            H2_REC(H2_EV_IDLE_ARM, current->id, 0, 0);
        }
        __atomic_store_n(&Scheduler::SwSlots::next_task_id(), next.id, __ATOMIC_RELEASE);
#if defined(CONFIG_DEBUG_IPC_SCHED)
        // H2 arm-time liveness audit (cold): dump when a deferred-switch arm
        // is published to a target that is NOT a live id_table member — i.e.
        // the arm is stale at publish time (task already removed).
        {
            auto *tgt = Scheduler::find_task(next.id);
            if (!tgt || tgt != &next) {
                kernel::Logger::raw_write("[H2-ARMDEAD] cur=");
                kernel::Logger::print_dec(current->id);
                kernel::Logger::raw_write(" next=");
                kernel::Logger::print_dec(next.id);
                kernel::Logger::raw_write(" tgt=");
                kernel::Logger::print_hex(reinterpret_cast<uint64_t>(tgt));
                kernel::Logger::raw_write(" st=");
                kernel::Logger::print_dec(
                    static_cast<uint64_t>(next.state));
                kernel::Logger::raw_write(" tick=");
                kernel::Logger::print_dec(arch::Timer::ticks());
                kernel::Logger::raw_write("\n");
            }
        }
#endif
        // Generation-lock: commit the deferred-switch pair.  The generation is
        // bumped only after load_rsp_from / load_cr3_from / next_task_id are
        // visible (all written before this IRQ-guarded block), and the arm
        // (save_rsp_to) is published after the bump — so any ISR that observes
        // the arm also observes the complete pair and the current generation.
        // isr_stubs.asm re-verifies this generation before applying, so it
        // never applies a half-written / superseded pair.
        uint64_t gen =
            __atomic_load_n(&Scheduler::SwSlots::generation(), __ATOMIC_RELAXED);
        __atomic_store_n(&Scheduler::SwSlots::generation(), gen + 1,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::save_rsp_to(), save_target,
                         __ATOMIC_RELEASE);

        uint64_t cr0 = arch::read_cr0();
        cr0 |= (1ULL << 3);
        arch::write_cr0(cr0);
    }
}

/// @brief Corrects current_task() to the task physically executing on the
///        live kernel stack.  Some test helpers (e.g. yield_as) or context
///        switches can leave current_task() pointed at a peer TCB while the
///        CPU is actually running on another task's stack; saving the live
///        register state into the wrong TCB corrupts it and desyncs the
///        scheduler (the `all` suite wedging/hanging).  Call before selecting
///        the next task so saves land in the real running task.  The
///        set_current() invariant (the running task is never in the ready
// ---------------------------------------------------------------------------
// Rate-monotonic schedule / reschedule
// ---------------------------------------------------------------------------

void Scheduler::rate_monotonic_schedule() noexcept {
    // Issue #26: RMS never raises TPR itself.  It tolerates preemption
    // only from strictly higher classes (the tick at 0xE0 is above every
    // raised class, INV-TPR5); the try_lock + skip discipline below is
    // unchanged by TPR state.
    if (all_tasks_.size() <= 1)
        return;

    if (!scheduler_lock_.try_lock())
        return;
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_, adopt_lock);

    auto *current = current_task();
    if (!current || current->magic != TaskControlBlock::TCB_MAGIC)
        return;

#if CONFIG_CANARY_GUARD
    // v0.4.0 MP-3: per-tick kernel-stack canary check.  Pure read under the
    // scheduler lock; catches a corrupted stack base within one tick.  This
    // per-tick probe also serves as the documented H2-residual timing mask
    // (docs/_archive/ipc_blocking-analysis.md §H2: a single per-tick
    // instruction changed the boot interleaving and masked the residual
    // 25/25 runs).
    if (!canary_verify_kernel_stack(current)) {
        kernel::Logger::fatal(
            "CANARY TRIP (kernel stack, tick): task '%s' id=%u top=0x%lx",
            current->name, current->id,
            current->kernel_stack_top);
        panic("kernel-stack canary violated");
    }
#endif

    // BUGS.md#021: during the test cycle, do NOT preemptively switch away from
    // the harness (init_task, PID 1) while it is RUNNING.  The harness runs the
    // synchronous test bodies; being preempted by lower-priority test tasks
    // orphans it ... (see full comment in original).
    bool harness_nonpreempt =
        (is_test_active() && harness_task_ptr_ != nullptr &&
         current == harness_task_ptr_ &&
         current->state == TaskState::RUNNING);
    // Issue #19: EDF urgency overrides harness protection (same
    // switch_to_task machinery as priority preemption — the harness is
    // requeued to the bitmap and resumes when the EDF queue drains).
    // Without this, an EDF-ready task never preempts a spinning harness
    // once need_resched is clear (bitmap is blind to the EDF queue).
    if (harness_nonpreempt &&
        !__atomic_load_n(&Scheduler::SwSlots::need_resched(), __ATOMIC_ACQUIRE) &&
        edf_preempt_candidate() == nullptr) {
        uint64_t cur_prio = effective_priority(current);
        uint64_t highest_ready = rq_own().highest_ready_priority();
        if (highest_ready < cur_prio)
            return;
    }

    // Clear any pending deferred switch.  State symmetry: switch_to_task()
    // set the preempted current READY + enqueued it (and the target RUNNING);
    // clearing the arm without applying it must undo BOTH, or the
    // physically-running harness stays READY+queued (INV-4) and next_task()
    // skips it, falling through to idle (H2 residual hang).  This mirrors the
    // drop_arm restore in scheduler_validate_pending_switch (CLR-MISC).
    if (__atomic_load_n(&Scheduler::SwSlots::save_rsp_to(), __ATOMIC_ACQUIRE) != 0) {
        uint64_t armed = __atomic_load_n(&Scheduler::SwSlots::next_task_id(),
                                         __ATOMIC_RELAXED);
        H2_REC(H2_EV_CLR_RMS, armed, 0, 0);
        __atomic_store_n(&Scheduler::SwSlots::save_rsp_to(), (uint64_t *)nullptr,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::load_rsp_from(), (uint64_t)0,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), (uint64_t)0,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::load_tls_from(), (uint64_t)0,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::next_task_id(), (uint64_t)-1,
                         __ATOMIC_RELEASE);
        restore_preempted_current(current, armed);
    }

    auto *next = next_task();
#if defined(CONFIG_DEBUG_IPC_SCHED)
    if (all_tasks_.size() == 7) {
        auto *r6 = Scheduler::find_task(6);
        IPC_SCHED_TRACE("[RMS]", "cur=", current->id, "next=",
                        next ? next->id : 0u, "t6=",
                        r6 ? (uint64_t)r6->state : 9u, "q6=",
                        r6 ? (uint64_t)r6->in_ready_queue_ : 9u);
    }
#endif
    // Defense-in-depth (H2 residual): never iretq the physically-running test
    // harness into the idle loop.  If the harness were left READY (INV-4) and
    // skipped by next_task(), the guard below would fall through to idle and
    // strand it; the harness must continue as the current task instead.  The
    // state-restore symmetry in the CLR paths above keeps it RUNNING, so this
    // is belt-and-braces for any residual path that still leaves it READY.
    bool harness_current =
        (is_test_active() && harness_task_ptr_ != nullptr &&
         current == harness_task_ptr_);
    if (next && next != current &&
        !(next == own_idle() &&
          (current->state == TaskState::RUNNING || harness_current))) {
        switch_to_task(current, *next, nullptr);
    }

    __atomic_store_n(&Scheduler::SwSlots::need_resched(), false, __ATOMIC_RELEASE);
}

void Scheduler::reschedule() noexcept {
    // Single-core UP: read-only ready-queue peek needs only IRQ-safety, not
    // the full scheduler_lock_.  Holding the lock here prevents the timer
    // ISR's try_lock() from succeeding in rate_monotonic_schedule(),
    // blocking the deferred switch from being applied (the ipc_blocking
    // test harness spins in `while (state != TERMINATED) reschedule();`).
    arch::IrqGuard irq_guard{};

    if (all_tasks_.size() <= 1)
        return;

    auto *current = current_task();
    if (!current || current->magic != TaskControlBlock::TCB_MAGIC)
        return;

    // Peek the highest-priority ready task.  We do NOT dequeue here —
    // the switch is deferred (INV-4) and the actual dequeue happens in
    // rate_monotonic_schedule() -> next_task() on the next timer tick.
    auto *next = rq_own().peek_highest();
    // Issue #19: an EDF arrival preempts even when the bitmap peek shows
    // nothing better — the bitmap cannot see the EDF queue.  Arm
    // unconditionally: bitmap-head early returns below (idle/non-READY
    // head) must never swallow EDF urgency.  A spurious arm is harmless —
    // next_task() re-validates on the tick.
    bool edf_preempt = (edf_preempt_candidate() != nullptr);
#if defined(CONFIG_DEBUG_IPC_SCHED)
    {
        IPC_SCHED_TRACE("[RS]", "cur=", current->id, "next=",
                        next ? next->id : 0u, "hi=",
                        (uint64_t)ready_queue_.highest_ready_priority(),
                        "nt=", (uint64_t)all_tasks_.size());
    }
#endif
    if (edf_preempt) {
        __atomic_store_n(&Scheduler::SwSlots::need_resched(), true,
                         __ATOMIC_RELEASE);
        return;
    }

    if ((!next || next == current))
        return;

    if (next != nullptr) {
        if (next == own_idle() && current->state == TaskState::RUNNING)
            return;

        if (next->state != TaskState::READY &&
            next->state != TaskState::RUNNING)
            return;
    }

    // IrqGuard destructor re-enables IRQs here, allowing the timer ISR to
    // fire and acquire scheduler_lock_ for rate_monotonic_schedule().
    __atomic_store_n(&Scheduler::SwSlots::need_resched(), true, __ATOMIC_RELEASE);
}

void Scheduler::switch_away_from_terminating(TaskControlBlock &exiting) noexcept {
    TaskControlBlock *next;

    // Scope for scheduler_lock_ — released before the IRQ-guarded publish step.
    {
        SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);

        // A deferred switch is already published.  Do NOT publish a second.
        if (__atomic_load_n(&Scheduler::SwSlots::save_rsp_to(), __ATOMIC_ACQUIRE) != 0)
            return;

        next = next_task();
        if (!next || next == &exiting)
            // Issue #25 C1: fall back to the OWN CPU's idle (idle_task_ is
            // the BSP idle — parking an AP on it runs two CPUs on one
            // stack).
            next = own_idle();
        if (!next || next->magic != TaskControlBlock::TCB_MAGIC)
            return;

        // Validate `next`'s iret frame.
        {
            uint64_t nsp = task_stack_ptr(next);
            uint64_t nbase = reinterpret_cast<uint64_t>(next->kernel_stack);
            // Harness boot-stack context.rsp is valid (see switch_to_task).
            bool harness_boot_ctx =
                (next == Scheduler::get_harness_task() && nsp != 0 &&
                 nsp >= reinterpret_cast<uint64_t>(_stack_start) &&
                 nsp < reinterpret_cast<uint64_t>(_stack_end));
            bool bad =
                (!nsp ||
                 (!harness_boot_ctx &&
                  (nsp < nbase || nsp >= next->kernel_stack_top)) ||
                 (next->page_table_ != 0 &&
                  (next->page_table_ & 0xFFF) != 0));
            if (!bad) {
                // Unused on riscv64 (no validation branch below).
                [[maybe_unused]] const uint64_t *f =
                    reinterpret_cast<const uint64_t *>(nsp);
#if defined(CONFIG_ARCH_X86_64)
                uint64_t rip_a = f[IRET_RIP_IDX], cs_a = f[IRET_CS_IDX],
                         rsp_a = f[IRET_RSP_IDX], ss_a = f[IRET_SS_IDX];
                uint64_t rip_b = f[IRET_SS_IDX], cs_b = f[IRET_RSP_IDX],
                         rsp_b = f[IRET_CS_IDX], ss_b = f[IRET_RIP_IDX];
                uint64_t f_rflags = f[IRET_RFLAGS_IDX];
                if (!validate_iret_frame(*next, nbase, f_rflags, rip_a, cs_a,
                                         rsp_a, ss_a) &&
                    !validate_iret_frame(*next, nbase, f_rflags, rip_b, cs_b,
                                         rsp_b, ss_b))
                    bad = true;
#elif defined(CONFIG_ARCH_AARCH64)
                // AArch64 saved/initial context is the vectors.S save area:
                // x0-x30 (+0..240), sp_el0 (+248), elr_el1 (+256), spsr_el1
                // (+264).  Validate the exception-return state (mirrors the
                // switch_to_task dispatch guard): ELR must be non-zero and
                // SPSR must encode a sane AArch64 return mode (EL0t = 0b0000
                // or EL1h = 0b0101) — a garbage frame (zeroed fresh stack)
                // must never be dispatched.
                uint64_t elr = f[256 / 8];
                uint64_t spsr = f[264 / 8];
                uint64_t mode = spsr & 0xFULL;
                if (elr == 0 || (mode != 0x0 && mode != 0x5))
                    bad = true;
#endif
            }
            if (bad)
                next = idle_task_;
        }
        if (!next || next->magic != TaskControlBlock::TCB_MAGIC)
            return;

        // Publish the deferred switch globals (load side only under lock).
        __atomic_store_n(&Scheduler::SwSlots::load_rsp_from(), task_stack_ptr(next),
                         __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::load_kstack_base(),
                         reinterpret_cast<uint64_t>(next->kernel_stack),
                         __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::load_kstack_top(), next->kernel_stack_top,
                         __ATOMIC_RELEASE);
        if (next->page_table_) {
            // Issue #156: tagged publish (same contract as above).
            uint64_t cr3_value = next->page_table_;
#if defined(CONFIG_ARCH_X86_64) && CONFIG_PCID
            cr3_value = arch::pcid_tag(next->page_table_,
                                       publish_pcid_for(*next));
#endif
            __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), cr3_value,
                             __ATOMIC_RELEASE);
            // Issue #74: publish the TLS base alongside CR3 (second publish
            // site — switch_away_from_terminating tick path).
            __atomic_store_n(&Scheduler::SwSlots::load_tls_from(),
                             next->is_user_ ? next->tls_base_ : 0,
                             __ATOMIC_RELEASE);
        } else {
            __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), VMM::get_kernel_pml4(),
                             __ATOMIC_RELEASE);
            // Issue #74: kernel/harness targets never carry TLS.
            __atomic_store_n(&Scheduler::SwSlots::load_tls_from(), (uint64_t)0,
                             __ATOMIC_RELEASE);
        }

        if (exiting.state == TaskState::RUNNING) {
            exiting.state = TaskState::READY;
            Scheduler::enqueue_ready(exiting);
        }
        next->state = TaskState::RUNNING;

        // Issue #25 C1: CPU0-gated (shared TSS; see above).
        if (next->page_table_ && sched_cpu() == 0)
            arch::GDT::set_tss_rsp0(next->kernel_stack_top);

        // Apply the target task's I/O permission bitmap (issue #3).
        arch::iopb_switch_to(*next);
    }

    // IRQ-guarded publish step (lock released).
    {
        arch::IrqGuard ig{};
        __atomic_store_n(&Scheduler::SwSlots::next_task_id(), next->id, __ATOMIC_RELEASE);
        // Generation-lock: bump the generation after the load side is published
        // (written above under the lock) and before arming save_rsp_to.  The
        // ISR epilogue verifies the generation before applying the pair.
        uint64_t gen =
            __atomic_load_n(&Scheduler::SwSlots::generation(), __ATOMIC_RELAXED);
        __atomic_store_n(&Scheduler::SwSlots::generation(), gen + 1,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::save_rsp_to(),
                         &task_stack_ptr(&exiting), __ATOMIC_RELEASE);
#if defined(CONFIG_ARCH_X86_64)
        uint64_t cr0 = arch::read_cr0();
        cr0 |= (1ULL << 3);
        arch::write_cr0(cr0);
#endif
    }
}

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
        out[idx].runq_next = t->runq_next_;
        out[idx].runq_prev = t->runq_prev_;
        out[idx].in_ready_queue = t->in_ready_queue_;
        out[idx].rq_priority = t->rq_priority_;
        out[idx].edf_next = t->edf_next_;
        out[idx].edf_prev = t->edf_prev_;
        out[idx].in_edf_queue = t->in_edf_queue_;
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
            // Snapshot does not capture SporadicServer state nor PMM page-table
            // pools — clear the pointer and the intrusive object list so stale
            // UAF (0xDD-poisoned block) from a restored MemPool free-list cannot
            // crash effective_priority().  detach_all_objects() only unlinks
            // (bounded, never releases): the pool restore has already rewound
            // test-allocated blocks, so calling release() here would double-free.
            // Daemon ensure_running() recreates the SporadicServer if needed.
            t->detach_all_objects();
            t->runq_next_ = saved[j].runq_next;
            t->runq_prev_ = saved[j].runq_prev;
            t->in_ready_queue_ = saved[j].in_ready_queue;
            t->rq_priority_ = saved[j].rq_priority;
            t->edf_next_ = saved[j].edf_next;
            t->edf_prev_ = saved[j].edf_prev;
            t->in_edf_queue_ = saved[j].in_edf_queue;
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

// ---------------------------------------------------------------------------
// P5a: Deferred-kill helpers
// ---------------------------------------------------------------------------

void Scheduler::defer_kill(TaskControlBlock *task) noexcept {
    // Callers always hold scheduler_lock_ (deadline_miss_handler runs under
    // the on_tick/scan_deadlines guard) — see design §4.C VAR-08.
    if (s_deferred_kill_count < MAX_DEFERRED_KILLS) {
        s_deferred_kill_tasks[s_deferred_kill_count++] = task;
    } else {
        Logger::warn("[DMD] deferred-kill list full, task %lu not added",
                     task->id);
    }
}

void Scheduler::process_deferred_kills() noexcept {
    // Called from the on_tick tail under scheduler_lock_; also gated so it
    // only runs when the lock was acquired this tick (design §4.C VAR-08).
    for (uint64_t i = 0; i < s_deferred_kill_count; ++i) {
        auto *task = s_deferred_kill_tasks[i];
        if (!task || task->magic != TaskControlBlock::TCB_MAGIC)
            continue;
        // Issue #197: spare AP-live tasks (never free a live stack) —
        // skip this entry; a still-missed deadline re-fires defer_kill
        // on the next scan, so no kill intent is lost.
        if (is_current_on_any_cpu(task)) {
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
    s_deferred_kill_count = 0;
}

// ---------------------------------------------------------------------------
// P6b: Deadline scan (monitor path)
// ---------------------------------------------------------------------------

#if CONFIG_DEADLINE_MONITOR_TASK
void Scheduler::scan_deadlines() noexcept {
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
#if CONFIG_DEADLINE_MISS_DETECTION
    // Scan every live task rather than only deadline_list_.  A task's deadline
    // may be (re)configured after add_task() returned, in which case it is not
    // a member of deadline_list_ yet — walking only that list would miss its
    // deadline and silently suppress the miss detection.  Scanning all tasks
    // is O(n) but n is small and keeps detection correct for every periodic
    // task regardless of when its deadline was set.
    const uint64_t now = arch::Timer::ticks();
    for (auto *task = all_tasks_.first_ptr(); task;
         task = all_tasks_.next_ptr(task)) {
        if (task->magic != TaskControlBlock::TCB_MAGIC)
            continue;
        if (task->state == TaskState::TERMINATED)
            continue;
        if (task->period_ticks == 0 || task->deadline_ticks == 0)
            continue;
        if (task->deadline_missed)
            continue;
        if (task->deadline_ticks >= now)
            continue;
        if (task->get_sporadic_server()) {
            kernel::ScopedRef ss_ref{task->get_sporadic_server()};
            task->ss_state_on_deadline_miss = static_cast<uint8_t>(
                task->get_sporadic_server()->state());
            task->ss_budget_on_deadline_miss =
                task->get_sporadic_server()->remaining_budget();
        }
        task->deadline_missed = true;
        ++task->deadline_miss_count;
        deadline_miss_handler(*task, now - task->deadline_ticks);
        task->deadline_ticks += task->period_ticks;
        task->deadline_missed = false;
#if CONFIG_WCET_OVERRUN_DETECTION
        task->wcet_overrun_fired = false;
#endif
    }
#endif
    __atomic_fetch_add(&deadline_detection_integrity, 1, __ATOMIC_RELEASE);
}

void Scheduler::monitor_task_entry() noexcept {
    auto *me = Scheduler::current_task();
    while (true) {
        arch::pause();
        {
            arch::IrqGuard guard{};
            scheduler_lock_.lock();
            dequeue_ready(*me);
            // Set BLOCKED while STILL holding scheduler_lock_ so the on_tick
            // wake path (which also takes the lock before enqueueing) cannot
            // observe a half-blocked task and re-enqueue it -> BLOCKED+inrq
            // (the [WEDGE] INV-5 violation).  dequeue + state change must be
            // atomic with respect to the wake handshake.
            me->state = TaskState::BLOCKED;
            scheduler_lock_.unlock();
        }
        Scheduler::reschedule();

        if (__atomic_exchange_n(&s_scan_requested_, 0, __ATOMIC_ACQUIRE)) {
            scan_deadlines();
        }
    }
}

void Scheduler::ensure_monitor() noexcept {
    bool need_spawn = true;
    if (s_monitor_task_) {
        if (s_monitor_task_->magic == TaskControlBlock::TCB_MAGIC &&
            s_monitor_task_->state != TaskState::TERMINATED) {
            need_spawn = false;
        }
    }
    if (!need_spawn)
        return;

    Logger::info("[MON] Re-spawning deadline-monitor task");
    auto *tcb = TaskControlBlock::create(monitor_task_entry, 127,
                                         TaskControlBlock::NO_PERIOD);
    if (tcb) {
        __builtin_strncpy(tcb->name, "monitor", CONFIG_TASK_NAME_LEN - 1);
        tcb->name[CONFIG_TASK_NAME_LEN - 1] = '\0';
        s_monitor_task_ = tcb;
        __atomic_store_n(&s_scan_requested_, 0, __ATOMIC_RELEASE);
        add_task(*tcb);
        SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
        tcb->state = TaskState::BLOCKED;
        dequeue_ready(*tcb);
    }
}
#endif // CONFIG_DEADLINE_MONITOR_TASK

// ---------------------------------------------------------------------------
// Deadline-miss / WCET handlers (weak defaults)
// ---------------------------------------------------------------------------

#if CONFIG_DEADLINE_MISS_DETECTION
__attribute__((weak)) void
deadline_miss_handler(TaskControlBlock &task,
                      uint64_t missed_by_ticks) noexcept {
    bool budget_exhausted = (task.get_sporadic_server() != nullptr &&
                             static_cast<task::SporadicServer::State>(
                                 task.ss_state_on_deadline_miss) ==
                                 task::SporadicServer::State::EXHAUSTED);

#if CONFIG_DEADLINE_ACTION == 1
    if (budget_exhausted)
        Logger::error("[DMD] Task %lu (%s) budget exhausted (state=EXHAUSTED, "
                      "action=PANIC)",
                      task.id, task.name);
    else
        Logger::error("[DMD] Task %lu (%s) missed deadline by %lu ticks "
                      "(action=PANIC)",
                      task.id, task.name, missed_by_ticks);
    panic("[DMD] deadline miss (action=PANIC)");
#elif CONFIG_DEADLINE_ACTION == 2
    if (budget_exhausted)
        Logger::warn("[DMD] Task %lu (%s) budget exhausted (state=EXHAUSTED, "
                     "action=DEMOTE)",
                     task.id, task.name);
    else
        Logger::warn(
            "[DMD] Task %lu (%s) missed deadline by %lu ticks (action=DEMOTE)",
            task.id, task.name, missed_by_ticks);
    if (task.priority > 1) {
        uint64_t old_prio = effective_priority(&task);
        task.priority >>= 1;
        uint64_t new_prio = effective_priority(&task);
        if (old_prio != new_prio)
            rq_task(task).move_priority(task, old_prio, new_prio);
    }
#elif CONFIG_DEADLINE_ACTION == 3
    if (budget_exhausted)
        Logger::warn("[DMD] Task %lu (%s) budget exhausted (state=EXHAUSTED, "
                     "action=KILL)",
                     task.id, task.name);
    else
        Logger::warn(
            "[DMD] Task %lu (%s) missed deadline by %lu ticks (action=KILL)",
            task.id, task.name, missed_by_ticks);
    task.state = TaskState::TERMINATED;
    task.exit_code =
        static_cast<uint64_t>(-static_cast<int64_t>(Signal::SIGKILL));
    wake_waiting_parent(task);
    Scheduler::defer_kill(&task);
#elif CONFIG_DEADLINE_ACTION == 4
    if (budget_exhausted)
        Logger::info("[DMD] Task %lu (%s) budget exhausted (state=EXHAUSTED, "
                      "action=NOTIFY_MONITOR)",
                      task->id, task->name);
    else
        Logger::info("[DMD] Task %lu (%s) missed deadline by %lu ticks "
                      "(action=NOTIFY_MONITOR)",
                      task->id, task->name, missed_by_ticks);
    // Resolve the monitor PID: compile-time CONFIG_DEADLINE_MONITOR_PID when
    // set, otherwise the test-only override so the config matrix can target
    // the live [deadline-mon] task without a fixed compile-time PID.
    uint64_t monitor_pid = (CONFIG_DEADLINE_MONITOR_PID > 0)
                               ? static_cast<uint64_t>(CONFIG_DEADLINE_MONITOR_PID)
                               : (test_context_ ? test_context_->deadline_monitor_pid
                                                : 0);
    auto *monitor = Scheduler::find_task(monitor_pid);
    if (monitor && monitor->magic == TaskControlBlock::TCB_MAGIC &&
        monitor->state != TaskState::TERMINATED) {
        monitor->pending_signals |=
            (1ULL << static_cast<uint64_t>(Signal::SIGUSR1));
    }
#else
    if (budget_exhausted)
        Logger::info("[DMD] Task %lu (%s) budget exhausted (budget=%lu, "
                     "state=EXHAUSTED, action=LOG_ONLY)",
                     task.id, task.name, task.ss_budget_on_deadline_miss);
    else
        Logger::info(
            "[DMD] Task %lu (%s) missed deadline by %lu ticks (action=LOG_ONLY)",
            task.id, task.name, missed_by_ticks);
#endif
}
#endif

#if CONFIG_WCET_OVERRUN_DETECTION
__attribute__((weak)) void
wcet_overrun_handler(TaskControlBlock *task,
                     uint64_t overrun_by_ticks) noexcept {
    Logger::info(
        "[WCET] Task %lu (%s) exceeded WCET by %lu ticks (action=LOG_ONLY)",
        task->id, task->name, overrun_by_ticks);
}
#endif

} // namespace kernel

namespace kernel {
using namespace errors;

SchedulerError Scheduler::init_err(const SchedulerConfig &cfg) {
    init(cfg);
    return SCHED_ERR_OK;
}

SchedulerError Scheduler::add_task_err(TaskControlBlock &task) {
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    if (all_tasks_.size() >= MAX_TASKS)
        return SCHED_ERR_TABLE_FULL;
    if (id_table_find(task.id) != nullptr)
        return SCHED_ERR_DUPLICATE_ID;
    // Issue #20: enforced admission BEFORE any mutation — a denial leaves
    // tables, queues, and the ResourceTracker untouched (fail-closed); the
    // caller retries later (defer) or frees the TCB.  Logging stays outside
    // the helper (allocation-free path); one warn here is cold-path only.
    // Issue #23: partitioned gate — the candidate is checked against its
    // own target CPU's bound (queue_target), not the global sum.
    uint64_t util = 0;
    uint32_t bound = 0;
    uint64_t target = queue_target(task);
    SchedulerError adm =
        admission_check_cpu_locked(task, target, &util, &bound);
    if (adm != SCHED_ERR_OK) {
        Logger::warn("Scheduler: admission denied task %u (err=%u, "
                     "util=%u > bound=%u)",
                     task.id, static_cast<uint64_t>(adm), util, bound);
        return adm;
    }
    all_tasks_.append(task);
    if (task.period_ticks > 0 && task.deadline_ticks > 0) {
        deadline_list_.insert(task);
    }
    ENSURE(id_table_insert(task.id, &task) && "id_table full in add_task_err");
    // Issue #19/#20: single routing point via enqueue_ready (the old direct
    // bitmap enqueue was EDF-blind — every deadline task dispatched by
    // priority).  id_table_ membership precedes enqueue (H2 orphan guard).
    task.in_ready_queue_ = false;
    task.runq_next_ = nullptr;
    task.runq_prev_ = nullptr;
    task.in_edf_queue_ = false;
    task.edf_next_ = nullptr;
    task.edf_prev_ = nullptr;
    enqueue_ready(task);
    kernel::test::ResourceTracker::instance().track_task_add();
    return SCHED_ERR_OK;
}

SchedulerError Scheduler::remove_task_err(TaskControlBlock &task) {
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    if (id_table_find(task.id) == nullptr)
        return SCHED_ERR_NOT_FOUND;
    all_tasks_.remove(task);
    deadline_list_.remove(task);
    id_table_remove(&task);
    dequeue_ready(task);

    __atomic_store_n(&Scheduler::SwSlots::save_rsp_to(), nullptr, __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_rsp_from(), (uint64_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), (uint64_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_tls_from(), (uint64_t)0, __ATOMIC_RELEASE);

    // track_task_remove() lives in TaskControlBlock::cleanup() (shared teardown
    // point) — not here — to avoid double-counting.  See Scheduler::remove_task.
    return SCHED_ERR_OK;
}

SchedulerError Scheduler::alloc_id_err(uint64_t &out_id) {
    // H-1/H-4: same table-full contract as alloc_id(); error-returning form.
    if (all_tasks_.size() >= MAX_TASKS)
        return SCHED_ERR_TABLE_FULL;
    out_id = __atomic_fetch_add(&next_task_id_, 1UL, __ATOMIC_RELAXED);
    return SCHED_ERR_OK;
}

} // namespace kernel

/// @brief ISR-entry pre-save hook (isr_stubs.asm:217-220).  Fires on every ISR
/// entry, recording the interrupted task's RSP before any context switch.
/// Verifies the interrupted task's RSP lies within its kernel stack.  If the
/// check fails, logs a detailed report (current task, RSP, stack bounds, all
/// other tasks owning that RSP) — evidence for H2.  Runs with IF=0 (interrupt
/// gate); must not re-enable IRQs.
/// @brief ISR-epilogue RSP-owner abort hook (isr_stubs.asm:270-275).  Fires
///        when a pending deferred-switch apply is aborted because the loaded
/// RSP lies outside [scheduler_load_kstack_base, top) — the asm-side
/// stale/foreign-frame guard.  Cold path.  Dumps the rejected RSP, the
/// kstack range, the arm target, and the RFLAGS the interrupted task
/// will return to (H2 IF=0 freeze hypothesis).
/// @note Runs with IF=0 (interrupt gate); must not re-enable IRQs.
