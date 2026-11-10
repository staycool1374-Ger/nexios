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

/// @brief Trap attribution verdict (issue #513 M1 closure). Answers "whose
///        fault is this" from frame placement + live page-table root,
///        instead of trusting the published current task (which the
///        RSP-owner scan can misresolve to a boot-stack runner).
struct SchedTrapOwner {
    /// @brief Task whose page-table root is live (the faulting address
    ///        space), or nullptr when no live user task matches.
    TaskControlBlock *trapped;
    /// @brief Task whose kstack holds @p frame_base (or the harness for
    ///        the boot stack), or nullptr when no task owns it.
    TaskControlBlock *frame_owner;
    /// @brief True when acting on `current_task()` would blame the wrong
    ///        task (or a non-task context).
    bool foreign;
    /// @brief True when no live task could be resolved (fail-closed).
    bool unresolved;
};

/// @brief Live page-table root in publish-tagged form (issue #513).
///        x86: CR3 (PCID-tagged); rv64: SATP (MODE|ASID|PPN);
///        aarch64: TTBR0_EL1 (ASID-tagged). Compares equal against
///        arch::context_id_tag(task root, task id) for the live task —
///        no masks, no untag: both sides carry the same encoding.
[[nodiscard]] uint64_t live_root_tagged() noexcept;

/// @brief Resolve the task whose tagged root equals @p root.
///        User tasks only (kernel context has no per-task root).
///        Returns nullptr when nothing live matches.
[[nodiscard]] TaskControlBlock *resolve_task_by_root(uint64_t root) noexcept;

/// @brief Resolve the task whose kstack range holds @p frame_base.
///        Falls back to the harness task for the boot stack.
///        Returns nullptr when no task owns the address.
[[nodiscard]] TaskControlBlock *resolve_stack_owner(uint64_t frame_base) noexcept;

/// @brief Validate a U-trap frame's attribution before any kill/timeout/
///        bookkeeping decision (issue #513). Fast path (no scan): current
///        is valid, frame lies in current's kstack, and — for user tasks
///        with a page table — the live root equals current's tagged root.
///        A U-trap attributed to a kernel current is always foreign.
///        Slow path: resolve trapped by live root, frame_owner by range.
///        Never logs, locks, or allocates (trap-entry safe); callers own
///        the trip disposition (counter + DEBUG halt / release repair).
/// @param frame_base Base address of the trap save area.
/// @param site      Short caller tag for the trip latch (no logging here).
[[nodiscard]] SchedTrapOwner validate_trap_frame(uint64_t frame_base,
                                                 const char *site) noexcept;

/// @brief Validate the current task for a syscall dispatch (issue #513,
///        3.1 choke point). True only for a valid user current whose live
///        RSP sits inside its own kstack (or the harness on the boot
///        stack). Two compares on the fast path; no scan, no logging.
[[nodiscard]] bool validate_syscall_frame(const TaskControlBlock *cur,
                                          const uint64_t *regs) noexcept;

/// @brief Trip disposition (issue #513 2.11): PROCEED means the faulting
///        task was repaired into current and the caller must continue the
///        normal path (kill for faults, dispatch for syscalls); PARK means
///        no live task could be attributed — the caller must switch-or-park
///        with no arm (the Item-1 epilogue gate parks instead of resuming).
enum class TrapOwnerDisposition : uint8_t { PROCEED = 0, PARK = 1 };

/// @brief Test hook consulted on a trap-attribution trip (issue #513,
///        same shape as stack_overflow_hook). Weak default returns false.
///        A strong test override returns true after latching: the trip
///        then takes the release path without halting, so tests can
///        assert the outcome instead of halting.
bool trap_owner_hook(uint64_t expected_id, uint64_t trapped_id,
                     uint64_t site) noexcept;

/// @brief Latch a trap-attribution trip and apply the disposition
///        (issue #513 2.11). Latches expected/trapped/site/frame-owner/
///        slot-top/live-root in FaultState, consults trap_owner_hook,
///        then: hook-true → repair + PROCEED; benign shape (frame owned
///        by the resolved task AND live root matches its tagged root —
///        only the published current label lagged) → repair + PROCEED
///        (no halt: proceeding is exact, and halting would wedge green
///        runs on a self-healing shape); otherwise DEBUG builds halt via
///        first_chance_halt (TRAP_OWNER, raw serial — no Logger), release
///        builds repair-if-possible else PARK. Repair = set_current_task
///        (trapped) so the caller kills/dispatches the right task; the
///        armed switch-away (fault paths) uses the discard save target.
///        Never resumes the faulting frame itself.
TrapOwnerDisposition trap_owner_trip(const SchedTrapOwner &o, uint64_t sepc,
                                     uint64_t scause, uint64_t stval,
                                     uint64_t frame_base,
                                     uint64_t site) noexcept;

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
