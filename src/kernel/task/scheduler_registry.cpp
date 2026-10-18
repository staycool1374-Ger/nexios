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

/// @file scheduler_registry.cpp
/// @brief Task registry + task-set administration for Scheduler (issue #323).
///        Pure move out of scheduler.cpp: no logic changes.

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_registry_internal.hpp>
#include <kernel/task/scheduler_admission_internal.hpp> // issue #323 LIU bounds/exempt
#include <kernel/task/scheduler_integrity.hpp>         // issue #369 T1a validate_task_ptr
#include <kernel/task/scheduler_queue_internal.hpp>    // issue #323 edf_remove
#include <kernel/task/scheduler_switch_internal.hpp>    // issue #323 SwSlots (remove_task_err)
#include <kernel/task/idle_monitor.hpp>                 // issue #323 idle_note_progress
#include <kernel/task/tcb_write_log.hpp>                // issue #323 dump_tcb_write_log
#include <kernel/log/dmesg.hpp>                         // issue #323 dmesg ring
#include <kernel/memory/integrity.hpp>                  // issue #323 integrity checks
#include <kernel/test/resource_tracker.hpp>             // issue #323 track_task_add
#include <kernel/arch/apic.hpp>                         // issue #323 arch::APIC (SCHED-IPI path)
#include <kernel/arch/timer.hpp>
#include <kernel/arch/gdt.hpp>
#include <kernel/arch/msr.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <lib/logger.hpp>
#include <assert.hpp>
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/smp.hpp> // issue #323 ap_count (sched_up_cpus)
#endif

namespace kernel {

using sched_admission::LIU_LEYLAND_BOUNDS;
using sched_admission::LIU_LEYLAND_LIMIT;
using sched_admission::LIU_LEYLAND_MAX_TASKS;
using sched_admission::server_budget_for_admission;
using sched_queue::edf_remove;
using sched_verify::validate_task_ptr;

TaskControlBlock *const Scheduler::ID_TOMBSTONE =
    reinterpret_cast<TaskControlBlock *>(static_cast<uintptr_t>(1));

AllTasksRegistry Scheduler::all_tasks_;

constinit TaskControlBlock *Scheduler::id_table_[ID_TABLE_SIZE] = {};

constinit uint64_t Scheduler::next_task_id_ = 0;
constinit uint64_t Scheduler::sporadic_task_count_ = 0;
constinit bool Scheduler::preempt_enabled_ = false;
constinit bool Scheduler::suppress_terminated_log_ = false;

// Issue #242: breaker observation starts unwatched (STARVABLE_NONE) with a
// zero count on every CPU; reset_starvation_breaker() re-establishes this on
// init and snapshot restore (policy-runtime, never preserved across tests).
uint64_t Scheduler::starve_level_[CONFIG_MAX_CPUS];
uint32_t Scheduler::starve_ticks_[CONFIG_MAX_CPUS];
    constinit TaskControlBlock *Scheduler::idle_task_ = nullptr;
TaskControlBlock *Scheduler::idle_tasks_[CONFIG_MAX_CPUS] = {};

    constinit TaskControlBlock *Scheduler::shell_task_ptr_ = nullptr;
    constinit TaskControlBlock *Scheduler::harness_task_ptr_ = nullptr;

uint64_t Scheduler::migration_count_[CONFIG_MAX_CPUS] = {};
uint64_t Scheduler::stuck_rescue_count_ = {};
uint64_t Scheduler::loadavg_1min_ = 0;
uint64_t Scheduler::loadavg_5min_ = 0;
uint64_t Scheduler::loadavg_15min_ = 0;

void Scheduler::set_priority(TaskControlBlock &task,
                             uint64_t new_prio) noexcept {

    ENSURE(scheduler_lock_.holder() == nullptr); // issue #369 T2b: no reentrant take (non-recursive)
    ENSURE(validate_task_ptr(&task, "set_priority"));
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

    ENSURE(scheduler_lock_.holder() == nullptr); // issue #369 T2b: no reentrant take (non-recursive)
    ENSURE(validate_task_ptr(&task, "set_affinity_err"));
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

    ENSURE(scheduler_lock_.holder() == nullptr); // issue #369 T2b: no reentrant take (non-recursive)
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
    ENSURE(validate_task_ptr(&task, "get_affinity"));
    return task.cpu_affinity;
}

bool Scheduler::is_rt_task(const TaskControlBlock &task) noexcept {
    ENSURE(validate_task_ptr(&task, "is_rt_task"));
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
    ENSURE(validate_task_ptr(&task, "set_task_ready"));
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
        // an affinity change under it).  Issue #369 T2b: assert no
        // reentrant take exactly where the deadlock would occur (callers
        // such as on_tick wake paths may hold the lock on other branches).
        ENSURE(scheduler_lock_.holder() == nullptr);
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
    ENSURE(scheduler_lock_.holder() == nullptr); // issue #369 T2b: no reentrant take (non-recursive)
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

    ENSURE(scheduler_lock_.holder() == nullptr); // issue #369 T2b: no reentrant take (non-recursive)
    ENSURE(validate_task_ptr(&task, "register_task"));
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

    ENSURE(scheduler_lock_.holder() == nullptr); // issue #369 T2b: no reentrant take (non-recursive)
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    ENSURE(validate_task_ptr(&task, "add_task"));
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

    ENSURE(scheduler_lock_.holder() == nullptr); // issue #369 T2b: no reentrant take (non-recursive)
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    if (task.magic != TaskControlBlock::TCB_MAGIC) [[unlikely]] {
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
    ENSURE(validate_task_ptr(&task, "unregister_task"));
    if (!scheduler_lock_.try_lock())
        return false;
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_, adopt_lock);
    ENSURE(scheduler_lock_.holder() != nullptr); // issue #369 T2a: adopted global held before any leaf take below

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

}  // namespace kernel

namespace kernel {
using namespace errors;

SchedulerError Scheduler::init_err(const SchedulerConfig &cfg) {
    init(cfg);
    return SCHED_ERR_OK;
}

SchedulerError Scheduler::add_task_err(TaskControlBlock &task) {

    ENSURE(scheduler_lock_.holder() == nullptr); // issue #369 T2b: no reentrant take (non-recursive)
    ENSURE(validate_task_ptr(&task, "add_task_err"));
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    if (all_tasks_.size() >= MAX_TASKS) [[unlikely]]
        return SCHED_ERR_TABLE_FULL;
    if (id_table_find(task.id) != nullptr) [[unlikely]]
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

    ENSURE(scheduler_lock_.holder() == nullptr); // issue #369 T2b: no reentrant take (non-recursive)
    ENSURE(validate_task_ptr(&task, "remove_task_err"));
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    if (id_table_find(task.id) == nullptr) [[unlikely]]
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
