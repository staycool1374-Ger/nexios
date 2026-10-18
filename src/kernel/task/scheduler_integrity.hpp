#pragma once

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

/// @file scheduler_integrity.hpp
/// @brief Centralized scheduler context-switch validators (issue #323).
///        Single home for the kernel-stack pointer accessor and the
///        switch/iret-frame validation predicates that guard a deferred
///        context switch, so scheduler.cpp no longer owns them privately.

#include <types.hpp>
#include <kernel/task/task.hpp>

namespace kernel {

/// @brief Scheduler integrity / context-switch validation (issue #323).
///        Everything that answers "is this a safe pair/stack to publish a
///        switch onto" lives here and is shared by the scheduler.
namespace sched_verify {

/// @brief Architecture-aware stack pointer access.  Returns a reference so
///        the address-of usage `&task_stack_ptr(t)` (switch_to_task's save
///        target) keeps writing the live TCB field (audit-scheduler-tasks
///        LOW: macro → inline fn).
inline uint64_t &task_stack_ptr(TaskControlBlock *t) noexcept {
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

/// @brief True when @p rsp lies within @p t's kernel-stack range [base, top].
///        Returns false (without logging) when @p rsp is below the base;
///        logs the overshoot when @p rsp is above the top.  Diagnostic
///        helper for validate_switch().
[[nodiscard]] bool rsp_in_stack_range(uint64_t rsp, const TaskControlBlock *t,
                        const char *label);

/// @brief Validate a context-switch pair before the deferred-switch publish
///        (audit-scheduler-tasks LOW: the single home of the duplicated
///        current/next validation).  Accepts only non-null, higher-half
///        pointers carrying the TCB magic; for a non-idle @p next also
///        requires the pre-saved RSP to lie inside its kernel stack.  Logs
///        the first failure and returns false; the caller then reports the
///        corruption.
[[nodiscard]] bool validate_switch(TaskControlBlock *current, TaskControlBlock *next,
                     const char *label);

/// @brief Validate a task pointer at a Scheduler:: entry point (issue #369
///        T1a: the common non-null + higher-half + magic prefix of
///        validate_switch, factored for registry/queue admission paths).
///        Logs the first failure and returns false; callers ENSURE
///        (hard-fail everywhere per #369 policy).  References are validated
///        by address: the object may still be poisoned, which is exactly
///        what the magic check catches (spec §D5: magic holds until
///        remove_task/unregister_task).
[[nodiscard]] bool validate_task_ptr(const TaskControlBlock *t,
                       const char *label) noexcept;

/// @brief Bump @c scheduler_corruption_count and (in DEBUG) fail loudly.
///        Called by the scheduler when a validator above returns false.
void report_corruption(const char *label);

/// @brief True when @p rsp belongs to the kernel boot stack
///        (kernel-image space), i.e. the physically-running harness.
[[nodiscard]] bool is_boot_stack_rsp(uint64_t rsp) noexcept;

#if defined(CONFIG_ARCH_X86_64)
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
[[nodiscard]] bool validate_iret_frame(const TaskControlBlock &next, uint64_t nbase,
                         uint64_t f_rflags, uint64_t rip, uint64_t cs,
                         uint64_t rsp, uint64_t ss) noexcept;
#endif

} // namespace sched_verify

} // namespace kernel
