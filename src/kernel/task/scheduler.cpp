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
#include <kernel/task/scheduler_integrity.hpp>      // issue #323 validators
#include <kernel/task/scheduler_queue_internal.hpp> // issue #323 queue/EDF
#include <kernel/task/scheduler_admission_internal.hpp> // issue #323 admission
#include <kernel/task/scheduler_debug_internal.hpp> // issue #323 debugger
#include <kernel/task/scheduler_lifecycle_internal.hpp> // issue #323 lifecycle
#include <kernel/task/scheduler_switch_internal.hpp> // issue #323 switch engine (shared decls)
#include <kernel/task/scheduler_path.hpp>      // issue #347 Slice B
#include <kernel/task/idle_monitor.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/elf/elf.hpp>
#include <crc32.hpp>
#include <services/terminal/terminal.hpp>
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
#include <kernel/debug/debug_stop.hpp>
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

#if defined(CONFIG_DEBUG)
// Fault-injection mask for dispatch_step() (issue #347 Slice B,
// declared in scheduler_path.hpp). Test-only; absent in release.
uint64_t kernel::g_test_skip_dispatch_step = 0;
#endif

// Issue #323 switch strand: current_sp() moved to the shared
// scheduler_switch_internal.hpp home (sched_switch::current_sp); the three
// remaining call sites below use it via the using-decl in namespace kernel.

// Boot stack (section .boot_stack, bounded by the linker's _stack_start /
// _stack_end symbols).  The harness (init, PID 1) physically runs on this
// stack in test mode — it is never switched onto its TCB kernel_stack, so no
// task's kernel_stack range covers the live RSP while the harness executes.
extern "C" {
extern char _stack_start[];
extern char _stack_end[];
}

// Issue #323: validate_iret_frame() lives in scheduler_integrity.cpp.

#include <kernel/task/sporadic_server.hpp>

// Issue #323 switch strand: the IRET_*_IDX constants moved to the shared
// scheduler_switch_internal.hpp home (sched_switch::); the switch engine in
// scheduler_switch.cpp is the sole user.
#include <signal.hpp>
#include <logger.hpp>

#if defined(CONFIG_DEBUG_IPC_SCHED)
// Wedge diagnostics for deferred-switch / ready-queue desync analysis.
// PfA-B: folded into per-CPU CpuContext debug state (design §4.A VAR-13).
#endif

namespace kernel {

// Issue #323: the kernel-stack pointer accessor and the context-switch
// validators live in scheduler_integrity.{hpp,cpp} (single home).  Import
// them so the existing unqualified call sites in this TU stay unchanged.
using sched_verify::task_stack_ptr;
using sched_verify::validate_switch;
using sched_verify::report_corruption;
using sched_verify::is_boot_stack_rsp;
// Issue #323 switch strand: current_sp() moved to scheduler_switch_internal.hpp.
using sched_switch::current_sp;
// Issue #323 switch strand: H2_EV_CLR_RMS moved to the sched_switch:: shared
// home; the next_task drop-arm path below is the remaining scheduler.cpp user.
using sched_switch::H2_EV_CLR_RMS;
#if defined(CONFIG_ARCH_X86_64)
using sched_verify::validate_iret_frame;
#endif

// Issue #323: the ready-queue/EDF helpers moved to scheduler_queue.cpp;
// import them so the existing unqualified call sites in this TU stay
// unchanged.
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

// Issue #323: admission helpers moved to scheduler_admission.cpp; import the
// table + numerator helper so the legacy warn-only path in add_task() stays
// unchanged.  The admission_* member functions are declared in scheduler.hpp.
using sched_admission::LIU_LEYLAND_BOUNDS;
using sched_admission::LIU_LEYLAND_LIMIT;
using sched_admission::LIU_LEYLAND_MAX_TASKS;
using sched_admission::server_budget_for_admission;

// Issue #323: the per-CPU debugger park slot moved to scheduler_debug.cpp;
// import it so the rate_monotonic_schedule() park check stays unchanged.
using sched_debug::debug_tick_pc_;

// Issue #323: canary hook, poisoned-block probe, and dispatch counters
// moved to scheduler_debug.cpp; import them so the remaining tick/switch
// paths stay unchanged.
using sched_debug::is_poisoned_block;
#if defined(CONFIG_DEBUG)
using sched_debug::g_diag_rms_count;
using sched_debug::g_diag_switch_count;
using sched_debug::g_diag_tick_count;
#endif

// Issue #323: the deferred-kill worklist moved to scheduler_lifecycle.cpp;
// the on_tick tail observes only the count (drain happens there).
using sched_lifecycle::s_deferred_kill_count;

// H2_REC macro is now in global_state.hpp for cross-TU visibility

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
// Issue #25 C1: affinity (mailbox/quiesce/AP tick moved to scheduler_smp.cpp).
// ---------------------------------------------------------------------------

/// @brief Number of CPUs currently up (BSP + parked/running APs).
static uint64_t sched_up_cpus() noexcept {
#if defined(CONFIG_ARCH_X86_64)
    return 1 + kernel::smp::ap_count();
#else
    return 1;
#endif
}

// Allowed-CPU mask for `up` online CPUs (issue #255 H5: named helper for
// the nested ternary at the set_affinity_err site; cheap-compare-first
// order preserved; the up>=64 arm avoids the UB shift 1ULL<<64).
static uint64_t affinity_allowed_mask(uint64_t up) noexcept {
    if (up >= 64) {
        return ~0ULL;
    }
    if (up == 0) {
        return 0ULL;
    }
    return (1ULL << up) - 1;
}

// Issue #323 watchdog strand: budget/watchdog predicates live in scheduler_watchdog.cpp (file-static there).

errors::SchedulerError
Scheduler::set_affinity_err(TaskControlBlock &task, uint64_t mask) noexcept {
    // Quiesce window fences the lock-free AP current-apply path (spec
    // §3.4.5): with arms cancelled and AP dispatch parked, the running
    // check below is stable across the dequeue+enqueue.
    arch::IrqGuard irq_guard{};
    quiesce_enter();
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    uint64_t up = sched_up_cpus();
    uint64_t allowed = affinity_allowed_mask(up);
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
                // Issue #234: partition admission denial enters the ring.
                log::dmesg_push_sev(log::ErrorSubsystem::TIMING,
                                    log::kDmesgBase_TIMING + 3,
                                    log::LogSeverity::WARN, task.name,
                                    static_cast<uintptr_t>(adm));
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

void Scheduler::reset_starvation_breaker() noexcept {
    // Issue #242: unwatched (NONE) with zero count on every CPU.
    // Zero-init would fake-watch level 0 (the idle band); like
    // scheduler_next_task_id above, the explicit reset is the owner.
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c) {
        starve_level_[c] = PriorityMap::STARVABLE_NONE;
        starve_ticks_[c] = 0;
    }
}

uint64_t Scheduler::stuck_rescue_count() noexcept {
    return __atomic_load_n(&stuck_rescue_count_, __ATOMIC_RELAXED);
}

void Scheduler::reset_stuck_rescue_count_for_test() noexcept {
    __atomic_store_n(&stuck_rescue_count_, 0ULL, __ATOMIC_RELAXED);
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

void Scheduler::set_task_ready(TaskControlBlock &task) noexcept {
    arch::IrqGuard irq_guard{};
    // Issue #250 (audit): capture liveness BEFORE the READY write below.
    // The wake-while-current defer guard must observe the pre-wake state:
    // TERMINATED/REAPED tasks are never deferred (§12.1).
    const bool wake_live = (task.state != TaskState::TERMINATED &&
                            task.state != TaskState::REAPED);
    task.state = TaskState::READY;
    // Issue #285: wake = forward progress for the stall detector.
    idle_note_progress(task, arch::Timer::ticks());
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
        // Issue #250: same wake-while-current deferral as enqueue_ready
        // (a task affine elsewhere can still be the local current after
        // an affinity change under it).
        SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
        if (&task == Scheduler::current_task()) {
            // Mirror enqueue_ready's defensive unlink (audit): the local
            // current may still be linked on another CPU's queue after an
            // affinity change under it; clearing flags without unlinking
            // orphans the node. `target` is already user-clamped above.
            if (wake_live) {
                task.state = TaskState::READY;
                task.ready_deferred_ = true;
            }
            if (task.in_ready_queue_) {
                rq_for(target).remove(task, task.rq_priority_);
                task.in_ready_queue_ = false;
            }
            if (task.in_edf_queue_)
                edf_remove(task, target);
            return;
        }
        task.in_ready_queue_ = false;
        rq_for(target).enqueue(task, effective_priority(&task));
        return;
    }
    enqueue_ready(task);
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


TaskControlBlock *const Scheduler::ID_TOMBSTONE =
    reinterpret_cast<TaskControlBlock *>(static_cast<uintptr_t>(1));

AllTasksRegistry Scheduler::all_tasks_;

constinit TaskControlBlock *Scheduler::id_table_[ID_TABLE_SIZE] = {};

constinit uint64_t Scheduler::next_task_id_ = 0;
constinit uint64_t Scheduler::sporadic_task_count_ = 0;
constinit bool Scheduler::preempt_enabled_ = false;
constinit bool Scheduler::suppress_terminated_log_ = false;

// Issue #323 watchdog strand: test_context_/s_monitor_task_/s_scan_requested_ defs live in scheduler_watchdog.cpp.
ReadyQueueManager Scheduler::ready_queues_[CONFIG_MAX_CPUS];
DeadlineList Scheduler::deadline_list_;
// Issue #242: breaker observation starts unwatched (STARVABLE_NONE) with a
// zero count on every CPU; reset_starvation_breaker() re-establishes this on
// init and snapshot restore (policy-runtime, never preserved across tests).
uint64_t Scheduler::starve_level_[CONFIG_MAX_CPUS];
uint32_t Scheduler::starve_ticks_[CONFIG_MAX_CPUS];
    constinit TaskControlBlock *Scheduler::idle_task_ = nullptr;
TaskControlBlock *Scheduler::idle_tasks_[CONFIG_MAX_CPUS] = {};
Scheduler::MailboxEntry
    Scheduler::wake_mailbox_[CONFIG_MAX_CPUS][MAILBOX_SLOTS] = {};
constinit uint64_t Scheduler::wake_mailbox_count_[CONFIG_MAX_CPUS] = {};
sync::SpinLock Scheduler::wake_mailbox_lock_[CONFIG_MAX_CPUS];
constinit bool Scheduler::sched_quiesced_ = false;
constinit uint64_t Scheduler::sched_quiesce_depth_ = 0;
sync::SpinLock Scheduler::zombie_lock_;
    constinit TaskControlBlock *Scheduler::shell_task_ptr_ = nullptr;
    constinit TaskControlBlock *Scheduler::harness_task_ptr_ = nullptr;
constinit TaskControlBlock *Scheduler::zombie_head_ = nullptr;
constinit TaskControlBlock *Scheduler::zombie_tail_ = nullptr;
constinit uint64_t Scheduler::zombie_count_ = 0;
uint64_t Scheduler::migration_count_[CONFIG_MAX_CPUS] = {};
uint64_t Scheduler::stuck_rescue_count_ = {};
uint64_t Scheduler::loadavg_1min_ = 0;
uint64_t Scheduler::loadavg_5min_ = 0;
uint64_t Scheduler::loadavg_15min_ = 0;
sync::SpinLock Scheduler::scheduler_lock_;


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
    // Issue #242: breaker starts unwatched (zero-init would fake-watch the
    // idle band on every CPU).
    reset_starvation_breaker();

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
            // Issue #234: the bound check fires per admission (thousands of
            // times per test run) — the ring records the first exceed only;
            // every occurrence stays on the serial console above.
            static bool liu_exceed_recorded = false;
            if (!liu_exceed_recorded) {
                liu_exceed_recorded = true;
                log::dmesg_push_sev(log::ErrorSubsystem::TIMING,
                                    log::kDmesgBase_TIMING + 4,
                                    log::LogSeverity::WARN, task.name,
                                    total_util);
            }
        }
    }
}

void Scheduler::remove_task(TaskControlBlock &task) {
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    if (task.magic != TaskControlBlock::TCB_MAGIC) {
        Logger::error("remove_task: TCB %p magic=0x%lx (expected 0x%lx)",
                      &task, (uint64_t)task.magic,
                      (uint64_t)TaskControlBlock::TCB_MAGIC);
        // Issue #234: TCB corruption is an integrity violation — FATAL.
        log::dmesg_push_sev(log::ErrorSubsystem::TASK,
                            log::kDmesgBase_TASK + 9, log::LogSeverity::FATAL,
                            "TCB magic mismatch",
                            reinterpret_cast<uintptr_t>(&task));
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
    return 0; // Current not listed: index 0 (idle slot), not an error.
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

// ---------------------------------------------------------------------------
// on_tick — timer tick handler
// ---------------------------------------------------------------------------

void Scheduler::on_tick() noexcept {
#if defined(CONFIG_DEBUG)
    ++g_diag_tick_count;
#endif
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
        // Harness owns lifecycle (see harness_owns_lifecycle): the reaper
        // must not race test ScopeGuards (double-free UAF otherwise).
        if (!harness_owns_lifecycle()) {
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
            if (task->state == TaskState::BLOCKED) {
                task->state = TaskState::READY;
            } else if (task->state == TaskState::READY) {
                if (task->in_ready_queue_) {
                    // #345: already queued — duplicate per-tick re-wake
                    // would starve lower priorities (wdwake 1:1). Skip;
                    // stranded READY (!in_queue) still rescued below.
                    continue;
                }
                // Stranded READY (!in_queue) -> rescue via enqueue
            } else {
                // SIL 3 guard (audit issue #18): never queue RUNNING etc.
                // (current-never-queued invariant).
                continue;
            }
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

        // Issue #249: stranded-READY rescue. A non-current, non-idle task
        // that is READY but physically queued nowhere is undispatchable
        // (observed: shell id 6 wedged after an enqueue-while-current was
        // evicted by next_task's candidate==current branch, which pops and
        // clears in_ready_queue_; the unconditional dequeue_ready in
        // Notify::wait cannot repair a loss that happens after the last
        // dequeue, and the orphan detector above requires inrq==1 so it
        // is blind to this signature). Wake paths that strand a task do
        // not recur, so re-apply level-triggered every tick until the
        // task dispatches or dies. Detection-first: count + warn per
        // rescue (H2-ENQDEAD precedent) so a recurring wake-loss can never
        // hide behind the heal. EDF-eligible tasks are owned by
        // edf_peek_valid_own — never touch them here. Bounded by
        // STRANDED_RESCUE_SCAN_MAX; no foreign locks, no allocation,
        // never clobbers a non-READY state (H2 guard, copied from #18).
        uint64_t sweep_count = 0;
        for (auto *task = all_tasks_.first_ptr();
             task != nullptr && sweep_count < STRANDED_RESCUE_SCAN_MAX;
             task = all_tasks_.next_ptr(task)) {
            ++sweep_count;
            if (task->magic != TaskControlBlock::TCB_MAGIC)
                continue;
            if (is_idle_task(task) || task == current_task())
                continue;
            if (queue_target(*task) != sched_cpu())
                continue; // issue #25 C1: own-CPU tasks only
            if (edf_eligible(*task))
                continue; // EDF path owns these (edf_peek_valid_own)
            if (task->state != TaskState::READY)
                continue; // never clobber RUNNING/BLOCKED/etc (H2)
            if (task->in_ready_queue_ || task->in_edf_queue_)
                continue; // only the wedge signature
            bool phys = false;
            for (uint64_t prio_idx = 0;
                 prio_idx <= CONFIG_PRIORITY_CEILING && !phys; ++prio_idx) {
                if (rq_task(*task).queue(prio_idx).contains(*task))
                    phys = true;
            }
            if (phys)
                continue; // flag/link desync the other way — not ours
            // Issue #250: a legitimately-deferred task is always current
            // while flagged (wake-side mirror), and the sweep already
            // skips current — so non-current + flagged is a lost-consume
            // bug, not a valid deferral. Clear and rescue anyway (never
            // skip: skipping on flag would reintroduce the wedge on flag
            // loss).
            task->ready_deferred_ = false;
            uint64_t rescue_no = ++stuck_rescue_count_;
            if (rescue_no == 1 ||
                (rescue_no % STRANDED_RESCUE_WARN_EVERY) == 0) {
                Logger::warn(
                    "sched: stranded READY rescued #%lx id=%lx eff=%lx",
                    rescue_no, task->id, effective_priority(task));
            }
            enqueue_ready(*task);
            __atomic_store_n(&Scheduler::SwSlots::need_resched(), true,
                             __ATOMIC_RELEASE);
        }

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
                // Issue #283: lifetime observed-maximum + exceed log live
                // in the idle_monitor seam (M5: unit-testable); the tick
                // path only invokes it for the running task.
                (void)idle_note_exec_sample(*task);
            }

            if (task->state == TaskState::RUNNING ||
                task->state == TaskState::READY) {
                uint64_t prev_rem = task->remaining_ticks;
                if (task->remaining_ticks > 0)
                    --task->remaining_ticks;
                if (prev_rem == 0 && task->period_ticks > 0) {
                    task->remaining_ticks = task->period_ticks;
                    // Issue #283: clean-boundary meet accounting lives in
                    // the idle_monitor seam (M5: unit-testable). Caveat:
                    // under CONFIG_DEADLINE_MONITOR_TASK the miss flag is
                    // transient (scan_deadlines sets then clears it in one
                    // pass), so meets there count reloads while misses stay
                    // exact in deadline_miss_count — the ratio still trends
                    // correctly; meets are a monitoring aid, never a
                    // safety invariant.
                    idle_note_period_reload(*task);
                    // Issue #21: new period, fresh budget clock (this
                    // tick was already billed into the old period above).
                    task->exec_period_ns = 0;
                    // Issue #331: stamp the new window phase (the
                    // blocked-branch advance below measures from here).
                    task->exec_window_start_tick = current_tick;
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

            // Issue #331: advance the PD_USE window for periodic tasks
            // that are neither RUNNING nor READY (e.g. BLOCKED in
            // sys_receive): their window never reloads above, so
            // exec_period_ns would accumulate lifetime exec and read
            // 100% forever. Observability only — no remaining_ticks /
            // deadline / EDF / queue changes (dispatch frozen).
            if (task->state != TaskState::RUNNING &&
                task->state != TaskState::READY &&
                task->period_ticks > 0 &&
                task->period_ticks != TaskControlBlock::NO_PERIOD) {
                const uint64_t elapsed =
                    current_tick - task->exec_window_start_tick;
                if (elapsed >= task->period_ticks) {
                    // Sample-then-reset (#154 binding): the monotonic-max
                    // WCET seam must see the closing window first.
                    (void)idle_note_exec_sample(*task);
                    task->exec_period_ns = 0;
                    idle_note_period_reload(*task);
                    task->exec_window_start_tick +=
                        (elapsed / task->period_ticks) *
                        task->period_ticks;
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
                    // Issue #278: same lost-wakeup hole as sys_kill — a
                    // wheel-blocked sleeper would sit until wheel expiry.
                    // Sleepers only; set_task_ready is deferral-safe.
                    if (task->state == TaskState::BLOCKED &&
                        task->sleep_armed)
                        Scheduler::set_task_ready(*task);
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

        // Issue #41: per-task software watchdog expiry scan — single
        // bounded pass, disarmed/generation-mismatched tasks skipped.
        // Runs under the already-held scheduler_lock_ (same window as
        // the pager scan above); KILL defers via defer_kill, never
        // inline.
        scan_watchdogs_locked(current_tick);

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

    static constinit uint64_t tick_counter = 0;
    ++tick_counter;
    // Harness owns lifecycle (see harness_owns_lifecycle): the reaper must
    // not race test ScopeGuards (double-free UAF otherwise).  Deferring to
    // snapshot_restore is safe: test tasks are short-lived and few.
        if (tick_counter % 100 == 0) {
            if (!harness_owns_lifecycle())
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

    // Harness owns lifecycle (see harness_owns_lifecycle): tests drive
    // balancer_tick() directly so placement asserts stay deterministic.
    static constinit uint64_t balancer_cadence = 0;
    ++balancer_cadence;
    if (!harness_owns_lifecycle() &&
        (balancer_cadence % BALANCER_TICK_PERIOD) == 0) {
        balancer_tick();
    }

    // Issue #158: quarantine timeout poll (BSP tick; internally
    // try-locked + bounded, safe to call every tick; no-op when empty).
    TlbShootdown::on_tick(arch::Timer::ticks());

    rate_monotonic_schedule();
}

// ---------------------------------------------------------------------------
// Corruption / validation helpers
// ---------------------------------------------------------------------------

// Issue #323: report_corruption(), is_boot_stack_rsp(), rsp_in_stack_range(),
// and validate_iret_frame() live in scheduler_integrity.cpp (sched_verify).

// Issue #323: validate_switch() lives in scheduler_integrity.cpp.

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
#if defined(CONFIG_DEBUG)
    ++g_diag_rms_count;
#endif
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

    // Issue #225: debugger deferred-stop park. Runs inside the normal
    // tick decision (lock held) so the battle-tested pick/switch flow
    // below handles the parked task (including empty-queue/idle) exactly
    // like any blocking path — no bespoke ISR switch dance (whose
    // reschedule() arm can legitimately decide there is nothing to
    // switch to yet, stranding a BLOCKED-marked task on CPU). The stop
    // is taken only at a user-mode boundary: stop requested + RUNNING +
    // recorded interrupted PC below the user limit (nested ticks record
    // kernel PCs and skip for a later tick). Consumes the request; marks
    // has_utrap_frame (the tick wrote the U-frame slot). The park is
    // notified via debug_note_requested_park (event + poke, mirroring
    // the trap path): without it a pre-fetch park strands the target —
    // the debugger never learns the stop and sel2 polls empty forever
    // (issue #295). Detached tasks (id 0) never park: nobody could
    // resume them.
    {
        uint64_t cpu = sched_cpu();
        uint64_t tick_pc = (cpu < CONFIG_MAX_CPUS)
                               ? __atomic_load_n(&debug_tick_pc_[cpu],
                                                 __ATOMIC_RELAXED)
                               : CONFIG_USER_SPACE_LIMIT;
        if (cpu < CONFIG_MAX_CPUS &&
            __atomic_load_n(&current->debug_stop_requested,
                            __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&current->debugger_id, __ATOMIC_ACQUIRE) !=
                0 &&
            current->state == TaskState::RUNNING &&
            tick_pc < CONFIG_USER_SPACE_LIMIT) {
            __atomic_store_n(&current->debug_stop_requested, false,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&current->debug_parked, true, __ATOMIC_RELEASE);
            __atomic_store_n(&current->has_utrap_frame, true, __ATOMIC_RELEASE);
            current->state = TaskState::BLOCKED;
            dequeue_ready(*current);
            kernel::debug::debug_note_requested_park(*current, tick_pc);
        }
    }

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
        // Issue #221 (audit): a cleared arm must not leave a standing
        // force-apply behind (see cancel_pending_switch_cpu) — otherwise a
        // terminate-era force survives onto the next published normal arm.
        __atomic_store_n(&Scheduler::SwSlots::force_apply(), (uint64_t)0,
                         __ATOMIC_RELEASE);
        restore_preempted_current(current, armed);
    }

    // Issue #242: bounded starvation-breaker.  Strict RMS above starves
    // every lower level forever when the top READY level never drains
    // (convicted: two prio-11 never-blocking spinners alternate and the
    // prio-10 harness gets zero CPU over 3001 ticks).  When the top level H
    // is picked while lower non-idle READY levels wait for
    // kStarvationBreakerTicks consecutive ticks, force exactly one dispatch
    // below H, then reset to strict RMS.  Counter-only (task priorities are
    // never mutated: PI/admission untouched); O(1) bitmap ops; identical
    // debug/release flow; EDF keeps precedence (next_task checks EDF first,
    // and the breaker only overrides a strict bitmap pick, never an EDF one
    // — see below).  Worst-case interference is exactly one forced tick per
    // N = 20 backlogged ticks (docs/specs/scheduler.md).
    TaskControlBlock *breaker_next = nullptr;
    {
        uint64_t cpu = sched_cpu();
        uint64_t top = rq_own().highest_ready_priority();
        if (top == 0 ||
            rq_own().bitmap().highest_starvable_below(top) ==
                PriorityMap::STARVABLE_NONE) {
            starve_level_[cpu] = top;
            starve_ticks_[cpu] = 0;
        } else if (top == starve_level_[cpu]) {
            ++starve_ticks_[cpu];
        } else {
            starve_level_[cpu] = top;
            starve_ticks_[cpu] = 1;
        }
        if (starve_ticks_[cpu] >= kStarvationBreakerTicks) {
            // EDF keeps precedence: an EDF-pending tick is a non-strict
            // dispatch that breaks the starvation pattern, so the breaker
            // stands down (counter resets below) and next_task() serves the
            // EDF candidate as today.  edf_preempt_candidate() is safe here:
            // next_task() evaluates it in this same lock/IRQ context.
            if (edf_preempt_candidate() == nullptr) {
                breaker_next = next_task_below(top);
            }
            starve_level_[cpu] = PriorityMap::STARVABLE_NONE;
            starve_ticks_[cpu] = 0;
        }
    }
    auto *next = breaker_next ? breaker_next : next_task();
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
                        (uint64_t)rq_own().highest_ready_priority(),
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
    kernel::dispatch_step(kernel::SchedDispatchSteps::Arm); // issue #347
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
            t->detach_all_objects(daemon::is_managed(t->id));
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

// ---------------------------------------------------------------------------
// P6b: Deadline scan (monitor path)
// ---------------------------------------------------------------------------


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
        // Issue #234: by-design refusal, system healthy — WARN, not ERROR.
        log::dmesg_push_sev(log::ErrorSubsystem::TIMING,
                            log::kDmesgBase_TIMING + 3, log::LogSeverity::WARN,
                            task.name, static_cast<uintptr_t>(adm));
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
