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
/// @brief WATCHDOG_CREATE(91)/WATCHDOG_KICK(92) handlers (issue #41):
/// per-task software watchdog, self-only. Scalar stores into the caller's
/// own TCB — no user-pointer dereference — but stays out of
/// k_syscall_fast[] (issue #92 discipline: mutating path, full canary).
/// IrqGuard-serialized against the on_tick expiry scan; never blocks,
/// never reschedules, never allocates.

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_errors.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/nexios_config.h>

namespace kernel {

/// @brief Arm (or re-arm) the calling task's watchdog (issue #41).
///        arg0 = period in ticks (> 0); arg1 reserved (must be 0).
///        One-shot semantics live in the expiry scan; re-CREATE simply
///        re-arms with a fresh generation.
/// @return 0, -SYS_ERR_SCHED_NO_CURRENT, -SYS_ERR_SCHED_INVALID_ARGS.
uint64_t Syscall::sys_watchdog_create(uint64_t arg0, uint64_t arg1, uint64_t,
                                      uint64_t, uint64_t *) {
    auto *t = syscall_task();
    if (t == nullptr)
        return static_cast<uint64_t>(-errors::SYS_ERR_SCHED_NO_CURRENT);
    if (arg0 == 0 || arg1 != 0)
        return static_cast<uint64_t>(-errors::SYS_ERR_SCHED_INVALID_ARGS);
    const uint64_t now = arch::Timer::ticks();
    // Saturating expiry: a period reaching past UINT64_MAX clamps instead
    // of wrapping (a wrapped expiry would fire immediately — fail-open).
    const uint64_t slack = static_cast<uint64_t>(-1) - now;
    const uint64_t expiry = (arg0 > slack) ? static_cast<uint64_t>(-1)
                                           : now + arg0;
    {
        arch::IrqGuard guard{};
        t->wdog_period_ticks = arg0;
        t->wdog_last_kick_tick = now;
        t->wdog_expiry_tick = expiry;
        t->wdog_armed = true;
        ++t->wdog_gen;
        if (t->wdog_gen == 0)
            t->wdog_gen = 1;
    }
    return 0;
}

/// @brief Kick the calling task's watchdog (issue #41): restart the
///        period from now. All args reserved (must be 0).
/// @return 0, -SYS_ERR_SCHED_NO_CURRENT, -SYS_ERR_SCHED_INVALID_ARGS
///         (reserved args nonzero), -SYS_ERR_SCHED_INVALID_STATE (disarmed).
uint64_t Syscall::sys_watchdog_kick(uint64_t arg0, uint64_t arg1, uint64_t,
                                    uint64_t, uint64_t *) {
    auto *t = syscall_task();
    if (t == nullptr)
        return static_cast<uint64_t>(-errors::SYS_ERR_SCHED_NO_CURRENT);
    if (arg0 != 0 || arg1 != 0)
        return static_cast<uint64_t>(-errors::SYS_ERR_SCHED_INVALID_ARGS);
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
