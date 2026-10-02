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

/// @file syscall_handlers_watchdog.cpp
/// @brief WATCHDOG_CREATE(91)/WATCHDOG_KICK(92) handlers (issues #41/#277):
/// per-task software watchdog. pid arg selects the target: 0 = self (no
/// cap needed, exact #41 path), otherwise the caller must present a
/// WdogCap for that pid with WRITE right (issue #277, daemon supervision).
/// Scalar stores into TCBs — no user-pointer dereference — but stays out
/// of k_syscall_fast[] (issue #92 discipline: mutating path, full canary).
/// IrqGuard-serialized against the on_tick expiry scan; never blocks,
/// never reschedules, never allocates.

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_errors.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/wdog.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/nexios_config.h>

namespace kernel {

// Shared with syscall_handlers_cap.cpp (same TU-visible helper).
cap::CNode *current_cspace();

namespace {

/// @brief Resolve the watchdog target: pid 0 = caller, else the named
///        task gated by a WdogCap with WRITE right, single-owner match and
///        generation match (stale-slot guard). The looked-up cap reference
///        is released before returning the raw target (the TCB lifetime is
///        covered by the caller's IrqGuard window + scheduler state, same
///        as the self path — no blocking or reschedule happens inside).
/// @return Target TCB, or nullptr when validation fails.
TaskControlBlock *resolve_watchdog_target(uint64_t pid, uint64_t cap_handle) {
    auto *cur = syscall_task();
    if (cur == nullptr)
        return nullptr;
    if (pid == 0)
        return cur;
    cap::CNode *src = current_cspace();
    if (src == nullptr)
        return nullptr;
    KernelObject *obj = cap::lookup(src, cap_handle, cap::CapType::Wdog,
                                    cap::CAP_RIGHT_WRITE);
    if (obj == nullptr)
        return nullptr;
    auto *wdog = static_cast<cap::WdogCap *>(obj);
    bool ok = (wdog->target_pid == pid);
    TaskControlBlock *target =
        ok ? Scheduler::find_task(pid) : nullptr;
    if (target == nullptr || target->magic != TaskControlBlock::TCB_MAGIC ||
        target->generation != wdog->target_gen)
        ok = false;
    obj->release();
    return ok ? target : nullptr;
}

/// @brief Arm (or re-arm) a watchdog (issues #41/#277).
///        arg0 = period in ticks (> 0); arg1 = pid (0 = self, else needs
///        WdogCap); arg2 = cap handle for cross-task (0 = unused/self).
///        One-shot semantics live in the expiry scan; re-CREATE simply
///        re-arms with a fresh generation.
/// @return 0, -SYS_ERR_SCHED_NO_CURRENT, -SYS_ERR_SCHED_INVALID_ARGS.
uint64_t arm_watchdog(uint64_t period, TaskControlBlock *t) {
    const uint64_t now = arch::Timer::ticks();
    // Saturating expiry: a period reaching past UINT64_MAX clamps instead
    // of wrapping (a wrapped expiry would fire immediately — fail-open).
    const uint64_t slack = static_cast<uint64_t>(-1) - now;
    const uint64_t expiry =
        (period > slack) ? static_cast<uint64_t>(-1) : now + period;
    {
        arch::IrqGuard guard{};
        t->wdog_period_ticks = period;
        t->wdog_last_kick_tick = now;
        t->wdog_expiry_tick = expiry;
        t->wdog_armed = true;
        ++t->wdog_gen;
        if (t->wdog_gen == 0)
            t->wdog_gen = 1;
    }
    return 0;
}

} // namespace

/// @brief Arm (or re-arm) the watchdog (issues #41/#277).
///        arg0 = period in ticks, arg1 = pid (0 = self), arg2 = cap handle.
uint64_t Syscall::sys_watchdog_create(uint64_t arg0, uint64_t arg1,
                                      uint64_t arg2, uint64_t, uint64_t *) {
    if (arg0 == 0)
        return static_cast<uint64_t>(-errors::SYS_ERR_SCHED_INVALID_ARGS);
    TaskControlBlock *t = resolve_watchdog_target(arg1, arg2);
    if (t == nullptr) {
        if (syscall_task() == nullptr)
            return static_cast<uint64_t>(-errors::SYS_ERR_SCHED_NO_CURRENT);
        return static_cast<uint64_t>(-errors::SYS_ERR_SCHED_INVALID_ARGS);
    }
    return arm_watchdog(arg0, t);
}

/// @brief Kick the watchdog (issues #41/#277): restart the period from
///        now. arg0 = pid (0 = self), arg1 = cap handle, arg2 reserved 0.
/// @return 0, -SYS_ERR_SCHED_NO_CURRENT, -SYS_ERR_SCHED_INVALID_ARGS
///         (reserved args nonzero / no authority), -SYS_ERR_SCHED_INVALID_STATE
///         (disarmed).
uint64_t Syscall::sys_watchdog_kick(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                    uint64_t, uint64_t *) {
    if (arg2 != 0)
        return static_cast<uint64_t>(-errors::SYS_ERR_SCHED_INVALID_ARGS);
    TaskControlBlock *t = resolve_watchdog_target(arg0, arg1);
    if (t == nullptr) {
        if (syscall_task() == nullptr)
            return static_cast<uint64_t>(-errors::SYS_ERR_SCHED_NO_CURRENT);
        return static_cast<uint64_t>(-errors::SYS_ERR_SCHED_INVALID_ARGS);
    }
    const uint64_t now = arch::Timer::ticks();
    {
        arch::IrqGuard guard{};
        if (!t->wdog_armed || t->wdog_gen == 0)
            return static_cast<uint64_t>(-errors::SYS_ERR_SCHED_INVALID_STATE);
        const uint64_t slack = static_cast<uint64_t>(-1) - now;
        t->wdog_expiry_tick = (t->wdog_period_ticks > slack)
                                  ? static_cast<uint64_t>(-1)
                                  : now + t->wdog_period_ticks;
        t->wdog_last_kick_tick = now;
    }
    return 0;
}

} // namespace kernel
