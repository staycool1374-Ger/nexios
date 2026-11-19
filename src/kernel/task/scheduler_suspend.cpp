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

/// @file scheduler_suspend.cpp
/// @brief F4 admin stop mechanism (issue #557, INV-ADM-1..16): park,
///        resume (deadline + watchdog shift + re-arm), the latch, the
///        tick deferred-stop consumer, admin_absorb_wake.  Task-context
///        only (ENSURE depth 0 + IrqGuard + SchedulerLockGuard, the
///        debugger_resume precedent); never reschedules, blocks,
///        allocates or log-floods (INV-ADM-8).

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_integrity.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/log/dmesg_catalog.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <lib/logger.hpp>
#include <assert.hpp>

namespace kernel {

using sched_verify::validate_task_ptr;

// F4 admin-stop counters (see scheduler.hpp; same discipline as the
// #582 oracles).  Monotonic; rewound only by
// admin_stats_reset_for_test().
uint64_t Scheduler::adm_wake_absorbed_count_ = {};
uint64_t Scheduler::adm_wake_backstop_count_ = {};
uint64_t Scheduler::adm_stop_deferred_count_ = {};
uint64_t Scheduler::adm_resumed_count_ = {};
uint64_t Scheduler::adm_wdog_paused_count_ = {};
uint64_t Scheduler::adm_wdog_rearmed_count_ = {};
uint64_t Scheduler::adm_deadline_paused_count_ = {};

uint64_t Scheduler::adm_wake_absorbed_count() noexcept {
    return __atomic_load_n(&adm_wake_absorbed_count_, __ATOMIC_RELAXED);
}

uint64_t Scheduler::adm_wake_backstop_count() noexcept {
    return __atomic_load_n(&adm_wake_backstop_count_, __ATOMIC_RELAXED);
}

uint64_t Scheduler::adm_stop_deferred_count() noexcept {
    return __atomic_load_n(&adm_stop_deferred_count_, __ATOMIC_RELAXED);
}

uint64_t Scheduler::adm_resumed_count() noexcept {
    return __atomic_load_n(&adm_resumed_count_, __ATOMIC_RELAXED);
}

uint64_t Scheduler::adm_wdog_paused_count() noexcept {
    return __atomic_load_n(&adm_wdog_paused_count_, __ATOMIC_RELAXED);
}

uint64_t Scheduler::adm_wdog_rearmed_count() noexcept {
    return __atomic_load_n(&adm_wdog_rearmed_count_, __ATOMIC_RELAXED);
}

uint64_t Scheduler::adm_deadline_paused_count() noexcept {
    return __atomic_load_n(&adm_deadline_paused_count_, __ATOMIC_RELAXED);
}

void Scheduler::admin_stats_reset_for_test() noexcept {
    __atomic_store_n(&adm_wake_absorbed_count_, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&adm_wake_backstop_count_, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&adm_stop_deferred_count_, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&adm_resumed_count_, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&adm_wdog_paused_count_, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&adm_wdog_rearmed_count_, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&adm_deadline_paused_count_, 0, __ATOMIC_RELAXED);
}

bool Scheduler::admin_parked_of(const TaskControlBlock &task) noexcept {
    return __atomic_load_n(&task.admin_parked, __ATOMIC_ACQUIRE);
}

bool Scheduler::admin_absorb_wake(TaskControlBlock &task) noexcept {
#ifdef CONFIG_DEBUG
    // Discipline probe (warn-only, never fatal): production wake sites
    // run IF=0 or under scheduler_lock_; a task-context caller with
    // neither is a future desync.
    if (arch::interrupts_enabled() && !Scheduler::scheduler_lock_held()) {
        kernel::Logger::warn("sched: absorb outside IRQ-off/lock id=%u",
                             static_cast<unsigned>(task.id));
    }
#endif
    if (!__atomic_load_n(&task.admin_parked, __ATOMIC_ACQUIRE)) {
        return false;
    }
    // INV-ADM-1 layer a: latch the wake, never drop it, never write
    // READY here.  Resume consumes it exactly once.  The absorbed
    // counter lives here (single point — no call site can forget it).
    __atomic_store_n(&task.admin_wake_pending, true, __ATOMIC_RELEASE);
    __atomic_fetch_add(&adm_wake_absorbed_count_, 1ULL, __ATOMIC_RELAXED);
    return true;
}

namespace {

// Saturating add (arm_watchdog precedent): a suspension reaching past
// UINT64_MAX clamps instead of wrapping (a wrapped expiry would fire
// immediately — fail-open).
uint64_t admin_sat_add(uint64_t base, uint64_t delta) noexcept {
    const uint64_t slack = static_cast<uint64_t>(-1) - base;
    return (delta > slack) ? static_cast<uint64_t>(-1) : base + delta;
}

// Clamp a bumped wdog_gen off zero (arm_watchdog/disarm precedent):
// 0 reads as never-armed elsewhere, so the post-bump zero window must
// not exist even transiently (design-review N1 — stated, not omitted).
void admin_bump_wdog_gen(TaskControlBlock &target) noexcept {
    ++target.wdog_gen;
    if (target.wdog_gen == 0) {
        target.wdog_gen = 1;
    }
}

// Push the SCHED+21/+22 supervision records (WARN, catalogued).  Paused
// vs revoked stays observable without a format change: paused reads
// armed=0/expires=<non-zero>, disposed reads armed=0/expires=0.
void admin_note_wdog_paused(const TaskControlBlock &target) noexcept {
    log::dmesg_push_sev(log::ErrorSubsystem::SCHED,
                        log::kDmesgBase_SCHED + 21,
                        log::LogSeverity::WARN,
                        target.name, target.wdog_expiry_tick);
}

void admin_note_wdog_rearmed(const TaskControlBlock &target) noexcept {
    log::dmesg_push_sev(log::ErrorSubsystem::SCHED,
                        log::kDmesgBase_SCHED + 22,
                        log::LogSeverity::WARN,
                        target.name, target.wdog_expiry_tick);
}

} // namespace

// The park tail, shared by the direct suspend path and the tick
// deferred-stop consumer.  Caller holds scheduler_lock_ + IF=0 and has
// established the target is live, unparked and stoppable.
void Scheduler::admin_park_locked(TaskControlBlock &target,
                                  AdminPrevState prev) noexcept {
    const uint64_t now = arch::Timer::ticks();
    __atomic_store_n(&target.admin_prev_state, static_cast<uint8_t>(prev),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&target.admin_suspend_tick, now, __ATOMIC_RELEASE);
    __atomic_store_n(&target.admin_deadline_ticks, target.deadline_ticks,
                     __ATOMIC_RELEASE);
    // INV-ADM-11: latch the expiry, do NOT zero it — unlike
    // disarm_target (cap/wdog.cpp), which zeroes the expiry on revoke.
    __atomic_store_n(&target.admin_wdog_expiry_tick,
                     target.wdog_expiry_tick, __ATOMIC_RELEASE);
    __atomic_store_n(&target.admin_parked, true, __ATOMIC_RELEASE);
    __atomic_store_n(&target.admin_wake_pending, false, __ATOMIC_RELEASE);
    target.wdog_armed = false;
    // N2 note: the bump fires even for never-armed targets — harmless
    // (kick refuses via !armed, the idle seam takes the report-only
    // branch), and the resume edge pins no phantom arm.
    admin_bump_wdog_gen(target);
    Scheduler::dequeue_ready(target);
    target.state = TaskState::BLOCKED;
    deadline_list_.remove(target);
    if (target.period_ticks > 0 && target.admin_deadline_ticks != 0) {
        __atomic_fetch_add(&adm_deadline_paused_count_, 1ULL,
                           __ATOMIC_RELAXED);
    }
    if (target.admin_wdog_expiry_tick != 0) {
        __atomic_fetch_add(&adm_wdog_paused_count_, 1ULL,
                           __ATOMIC_RELAXED);
        admin_note_wdog_paused(target);
    }
}

errors::TcbCtlError
Scheduler::admin_suspend_err(TaskControlBlock &target) noexcept {
    ENSURE(validate_task_ptr(&target, "admin_suspend_err"));
    ENSURE(scheduler_lock_depth_own() == 0); // T2b: no reentrant take
    arch::IrqGuard irq_guard{};
    if (target.magic != TaskControlBlock::TCB_MAGIC) {
        return errors::TCBCTL_ERR_BAD_HANDLE;
    }
    if (Scheduler::find_task(target.id) != &target) {
        return errors::TCBCTL_ERR_BAD_HANDLE;
    }
    if (&target == Scheduler::current_task()) {
        return errors::TCBCTL_ERR_SELF;
    }
    if (target.state == TaskState::TERMINATED ||
        target.state == TaskState::REAPED) {
        return errors::TCBCTL_ERR_DEAD;
    }
    SchedulerLockGuard guard(scheduler_lock_);
    if (__atomic_load_n(&target.admin_parked, __ATOMIC_ACQUIRE)) {
        return errors::TCBCTL_ERR_ALREADY_SUSPENDED;
    }
    // Two park owners are mutually exclusive by construction (step 12):
    // suspend refuses a debugger-parked target ...
    if (__atomic_load_n(&target.debug_parked, __ATOMIC_ACQUIRE)) {
        return errors::TCBCTL_ERR_BUSY;
    }
    // ... and a #582 latch-armed BLOCKED runner is never silenced
    // (INV-ADM-9): the 8-CPU bounded scan names (id, generation), so a
    // recycled occupant cannot false-match.
    if (gs::lockpark_is_latched(target.id, target.generation)) {
        return errors::TCBCTL_ERR_BUSY;
    }
    if (target.state == TaskState::RUNNING) {
        // Deferred stop (INV-ADM-9): the tick parks the target at a
        // user-mode boundary; the wdog/deadline latch happens at park
        // time (never early — an early latch would hand back budget
        // the target legitimately consumes before it stops).
        if (Scheduler::queue_target(target) != Scheduler::sched_cpu()) {
            return errors::TCBCTL_ERR_BUSY;
        }
        __atomic_store_n(&target.admin_stop_requested, true,
                         __ATOMIC_RELEASE);
        __atomic_fetch_add(&adm_stop_deferred_count_, 1ULL,
                           __ATOMIC_RELAXED);
        return errors::TCBCTL_ERR_OK;
    }
    // WAITING is never assigned in-tree: fail closed, never silently
    // park an unknown disposition.
    if (target.state == TaskState::WAITING) {
        return errors::TCBCTL_ERR_BUSY;
    }
    const AdminPrevState prev =
        (target.state == TaskState::READY) ? AdminPrevState::READY
                                           : AdminPrevState::BLOCKED;
    Scheduler::admin_park_locked(target, prev);
    return errors::TCBCTL_ERR_OK;
}

errors::TcbCtlError
Scheduler::admin_resume_err(TaskControlBlock &target) noexcept {
    ENSURE(validate_task_ptr(&target, "admin_resume_err"));
    ENSURE(scheduler_lock_depth_own() == 0); // T2b: no reentrant take
    arch::IrqGuard irq_guard{};
    if (target.magic != TaskControlBlock::TCB_MAGIC) {
        return errors::TCBCTL_ERR_BAD_HANDLE;
    }
    if (Scheduler::find_task(target.id) != &target) {
        return errors::TCBCTL_ERR_BAD_HANDLE;
    }
    if (target.state == TaskState::TERMINATED ||
        target.state == TaskState::REAPED) {
        return errors::TCBCTL_ERR_DEAD;
    }
    SchedulerLockGuard guard(scheduler_lock_);
    if (!__atomic_load_n(&target.admin_parked, __ATOMIC_ACQUIRE)) {
        // Typed, no side effects (INV-ADM-2): a non-parked target is
        // byte-identical after this call.
        return errors::TCBCTL_ERR_NOT_SUSPENDED;
    }
    // Phase-preserving shift (INV-ADM-11/14/16): a target suspended
    // with N ticks of budget left resumes with N ticks — never a fresh
    // full period, never a permanent revocation, never free budget.
    const uint64_t now = arch::Timer::ticks();
    const uint64_t parked_at =
        __atomic_load_n(&target.admin_suspend_tick, __ATOMIC_ACQUIRE);
    const uint64_t elapsed = (now >= parked_at) ? now - parked_at : 0;
    const uint64_t latched_deadline =
        __atomic_load_n(&target.admin_deadline_ticks, __ATOMIC_ACQUIRE);
    const uint64_t latched_expiry =
        __atomic_load_n(&target.admin_wdog_expiry_tick, __ATOMIC_ACQUIRE);
    const bool wake_pending =
        __atomic_load_n(&target.admin_wake_pending, __ATOMIC_ACQUIRE);
    const auto prev = static_cast<AdminPrevState>(
        __atomic_load_n(&target.admin_prev_state, __ATOMIC_ACQUIRE));
    // WDOG re-arm before the latch clears: an already-overdue latched
    // expiry (reachable only through test-fabricated arms) resumes
    // already-due and fires on the next scan — fail-safe.
    if (latched_expiry != 0) {
        target.wdog_expiry_tick = admin_sat_add(latched_expiry, elapsed);
        target.wdog_last_kick_tick =
            admin_sat_add(target.wdog_last_kick_tick, elapsed);
        target.wdog_armed = true;
        admin_bump_wdog_gen(target);
        __atomic_fetch_add(&adm_wdog_rearmed_count_, 1ULL,
                           __ATOMIC_RELAXED);
        admin_note_wdog_rearmed(target);
    }
    // Deadline restore: only a latched (previously listed) deadline is
    // re-inserted — a zero latch means the node was never listed, so no
    // node is fabricated here.
    if (latched_deadline != 0) {
        target.deadline_ticks = admin_sat_add(latched_deadline, elapsed);
        if (target.period_ticks > 0 && target.deadline_ticks > 0) {
            deadline_list_.insert(target);
        }
    }
    // Clear the latch BEFORE any enqueue (INV-ADM-6/INV-ADM-1): from
    // here on the backstop sees an unparked task.
    __atomic_store_n(&target.admin_parked, false, __ATOMIC_RELEASE);
    __atomic_store_n(&target.admin_wake_pending, false, __ATOMIC_RELEASE);
    __atomic_store_n(&target.admin_stop_requested, false, __ATOMIC_RELEASE);
    __atomic_store_n(&target.admin_wdog_expiry_tick, 0, __ATOMIC_RELEASE);
    if (wake_pending || prev != AdminPrevState::BLOCKED) {
        target.state = TaskState::READY;
        Scheduler::enqueue_ready(target);
    }
    __atomic_fetch_add(&adm_resumed_count_, 1ULL, __ATOMIC_RELAXED);
    return errors::TCBCTL_ERR_OK;
}

// Tick deferred-stop consumer (INV-ADM-9): parks a RUNNING target whose
// suspend latched admin_stop_requested.  Runs under the RMS lock (the
// try_lock at the rate_monotonic_schedule head); the debugger park
// block keeps precedence (debug_parked refuses the park here, and
// suspend refused debug-parked targets at the edge).
void Scheduler::admin_consume_deferred_stop(TaskControlBlock &current,
                                            uint64_t tick_pc) noexcept {
    if (!__atomic_load_n(&current.admin_stop_requested,
                         __ATOMIC_ACQUIRE)) {
        return;
    }
    if (__atomic_load_n(&current.debug_parked, __ATOMIC_ACQUIRE)) {
        return;
    }
    if (current.state != TaskState::RUNNING) {
        return;
    }
    if (tick_pc >= CONFIG_USER_SPACE_LIMIT) {
        return;
    }
    __atomic_store_n(&current.admin_stop_requested, false,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&current.has_utrap_frame, true, __ATOMIC_RELEASE);
    Scheduler::admin_park_locked(current, AdminPrevState::RUNNING);
}

} // namespace kernel
