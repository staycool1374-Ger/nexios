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

/// @file irq_guard.hpp
/// @brief RAII interrupt disable — disable IRQs on construction, restore on
/// destruction.

#include <kernel/arch/hal/io.hpp>
#include <kernel/arch/cpu_context.hpp>

namespace arch {

// M2 (issues #545/#546): per-CPU IrqGuard nesting depth, defined in
// core/global_state.cpp next to scheduler_lock_depth.  Always maintained
// (not DEBUG-gated): the lock-park predicate gates on it in release
// builds too.  Transitory by construction — an IrqGuard never spans a
// test boundary or quiesce.
extern "C" uint64_t irq_guard_depth[CONFIG_MAX_CPUS];

// M2 follow-up (#545 FIRE): per-CPU IrqGuard *owner* attribution + a
// 32-entry transition ring per CPU (evidence only, no behaviour change).
// Depth alone cannot distinguish an own scope from a foreign/leaked one:
// the owner is stored on the 0->1 transition (ISR sentinel when taken in
// ISR context, otherwise the current task pointer) and cleared on the
// 1->0 transition.  Owner 0 means unknown/boot (fail-closed: deny park).
extern "C" uint64_t irq_guard_owner[CONFIG_MAX_CPUS];
extern "C" uint64_t irq_guard_trace_idx[CONFIG_MAX_CPUS];
extern "C" uint64_t irq_guard_trace_depth[CONFIG_MAX_CPUS][32];
extern "C" uint64_t irq_guard_trace_owner[CONFIG_MAX_CPUS][32];
extern "C" uint64_t irq_guard_trace_cur[CONFIG_MAX_CPUS][32];
extern "C" uint64_t irq_guard_trace_site[CONFIG_MAX_CPUS][32];

/// @brief Syscall-trap marker (M2 follow-up #545 FIRE): set for the
///        duration of Syscall::handle so the lock-park predicate can tell a
///        syscall trap (task context, may park) from a hardware ISR (must
///        never park).  All three arches funnel through the shared C++
///        syscall_handler, so one guard covers x86_64/aarch64/riscv64.
///        ALSO records the ISR nesting depth at trap entry: a nested
///        hardware ISR inside the trap raises nesting above the entry
///        value, which closes the exemption (nested ISRs must never park).
///        Non-reentrant by construction (a trap never traps again on the
///        same hart; a discarded trap stack cannot dtor — the next trap
///        entry overwrites both slots, and a stale set is inert: task
///        context runs at nesting 0 which never equals a trap entry ≥ 1).
extern "C" uint64_t in_syscall_trap[CONFIG_MAX_CPUS];
extern "C" uint64_t syscall_trap_nesting[CONFIG_MAX_CPUS];

/// @brief True while the calling context is a syscall trap at its own
///        nesting level (nested hardware ISRs excluded by the entry-depth
///        match).  False in task context, in real ISRs, and under stale
///        nesting counts (rv64).
inline bool in_syscall_trap_level() noexcept {
    const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    return (in_syscall_trap[cpu] != 0) &&
           (kernel::isr_nesting_own() == syscall_trap_nesting[cpu]);
}

/// @brief Single IsrContext-park predicate shared by `deny_reason()` and
///        `resched_park_gate_base()` (#583: the two gates forked on this
///        definition and diverged on rv64).
///        Deny iff all three conjuncts hold: ISR nesting is nonzero, the
///        caller is NOT at a genuine syscall-trap level, and interrupts
///        are disabled (the caller-supplied single IF sample is 0).
///        IF==1 *is* the proof of not-a-hardware-ISR: x86 interrupt gates
///        clear IF on entry, aarch64 takes the exception with `daifset`
///        masking IRQs, and riscv64 hardware-clears SSTATUS.SIE with the
///        #206 `csrci sstatus, 2` entry rule — our ISRs never re-enable, so
///        a hardware ISR cannot observe IF==1.  A stale (unbalanced)
///        `isr_nesting` count (rv64 suite baseline) must therefore not deny
///        while IF==1; IF==0 keeps the deny (fail-closed direction).
///        Only callers: `sync::deny_reason()` (passes
///        `arch::interrupts_enabled()`) and `resched_park_gate_base()`
///        (passes the already-sampled entry IF, never re-read inside so
///        the two gates cannot sample different instants).
inline bool isr_context_park_denied(bool interrupts_on) noexcept {
    return kernel::isr_nesting_own() != 0 && !in_syscall_trap_level() &&
           !interrupts_on;
}

/// @brief 32-entry per-CPU transition ring length (power of two: idx & 31).
inline constexpr uint64_t kIrqGuardTraceEntries = 32;

/// @brief This CPU's IrqGuard nesting depth.  Returns 0 iff no IrqGuard
///        scope is live on this CPU (interrupts may or may not be enabled:
///
/// depth tracks scoping, not the IF bit — query arch::interrupts_enabled()
/// for the bit).
inline uint64_t irq_guard_depth_own() noexcept {
    return irq_guard_depth[arch::cpu_index() % CONFIG_MAX_CPUS];
}

/// @brief This CPU's IrqGuard owner token: the task that performed the
///        outermost take (0 when no scope is live).  Evidence only; the
///        predicate compares it against the calling task to detect foreign
///        (leaked or cross-task) scopes.
inline uint64_t irq_guard_owner_own() noexcept {
    return irq_guard_owner[arch::cpu_index() % CONFIG_MAX_CPUS];
}

/// @brief Owner token for the calling context: the current task pointer
///        (or 0 when there is none, e.g. early boot).  Deliberately
///        independent of isr_nesting/IF: the token is recorded at take time
///        and compared at check time, and IF always flips in between (that
///        is what a guard *does*), so any IF- or nesting-dependent token
///        mismatches itself across the take/check pair and fabricates
///        foreign owners (observed on rv64: take at IF=1 recorded the task,
///        check at IF=0 computed the ISR sentinel).  ISR takes attribute to
///        the interrupted task — harmless, as ISR scopes always unwind
///        before the ISR returns (balanced, never observed by the
///        predicate); the IsrContext arm guards real ISRs separately.
inline uint64_t irq_guard_owner_token() noexcept {
    return reinterpret_cast<uint64_t>(kernel::current_cpu().current);
}

/// @brief RAII guard that disables interrupts on construction and restores
///        the previous interrupt state on destruction.
/// @note Use this to protect short critical sections from being interrupted.
class [[nodiscard]] IrqGuard {
  public:
    /// @brief Disable interrupts, saving the previous enabled state.
    IrqGuard() noexcept : irq_was_(interrupts_enabled()) {
        const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
        if (irq_guard_depth[cpu] == 0)
            irq_guard_owner[cpu] = irq_guard_owner_token();
        ++irq_guard_depth[cpu];
        // Evidence ring (stores only; IF discipline unchanged).
        const uint64_t slot = irq_guard_trace_idx[cpu]++ & 31U;
        irq_guard_trace_depth[cpu][slot] = irq_guard_depth[cpu];
        irq_guard_trace_owner[cpu][slot] = irq_guard_owner[cpu];
        irq_guard_trace_cur[cpu][slot] =
            reinterpret_cast<uint64_t>(kernel::current_cpu().current);
        irq_guard_trace_site[cpu][slot] =
            reinterpret_cast<uint64_t>(__builtin_return_address(0));
        cli();
    }

    /// @brief Restore the interrupt state that was saved at construction.
    /// @note Saturating: a dtor observing depth 0 means its matching take
    ///        was dropped by ghost containment (1b seam) or an abandoned
    ///        stack; wrapping to UINT64_MAX would permanently deny every
    ///        park predicate, so the count holds at 0 instead.  Monotonic
    ///        by construction otherwise — balanced scopes never touch the
    ///        floor, so a saturation always pairs with a ghost event that
    ///        the watchdog/counters already diagnose.
    ~IrqGuard() noexcept {
        const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
        if (irq_guard_depth[cpu] <= 1)
            irq_guard_owner[cpu] = 0;
        // Evidence ring (stores only; IF discipline unchanged).
        const uint64_t slot = irq_guard_trace_idx[cpu]++ & 31U;
        irq_guard_trace_depth[cpu][slot] =
            irq_guard_depth[cpu] != 0 ? irq_guard_depth[cpu] - 1 : 0;
        irq_guard_trace_owner[cpu][slot] = irq_guard_owner[cpu];
        irq_guard_trace_cur[cpu][slot] =
            reinterpret_cast<uint64_t>(kernel::current_cpu().current);
        irq_guard_trace_site[cpu][slot] =
            reinterpret_cast<uint64_t>(__builtin_return_address(0));
        if (irq_guard_depth[cpu] != 0)
            --irq_guard_depth[cpu];
        if (irq_was_)
            sti();
    }

    IrqGuard(const IrqGuard &) = delete;
    IrqGuard &operator=(const IrqGuard &) = delete;
    IrqGuard(IrqGuard &&) = delete;
    IrqGuard &operator=(IrqGuard &&) = delete;

  private:
    /// @brief Saved interrupt-enabled flag from before the guard.
    bool irq_was_;
};

/// @brief RAII syscall-trap marker (M2 follow-up #545 FIRE).  Marks the
///        calling CPU as inside a syscall trap at the current ISR nesting
///        depth; the lock-park predicate uses it to exempt genuine trap
///        context from the IsrContext deny (a trap runs on the calling
///        task's stack with a task to deschedule, and the park path's own
///        sti/hlt owns the IRQ state exactly like sys_receive's wait loop).
///        Instantiated once per syscall_handler invocation (all arches).
class [[nodiscard]] SyscallTrapScope {
  public:
    SyscallTrapScope() noexcept {
        const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
        in_syscall_trap[cpu] = 1;
        syscall_trap_nesting[cpu] = kernel::isr_nesting_own();
    }
    ~SyscallTrapScope() noexcept {
        const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
        in_syscall_trap[cpu] = 0;
    }

    SyscallTrapScope(const SyscallTrapScope &) = delete;
    SyscallTrapScope &operator=(const SyscallTrapScope &) = delete;
    SyscallTrapScope(SyscallTrapScope &&) = delete;
    SyscallTrapScope &operator=(SyscallTrapScope &&) = delete;
};

} // namespace arch
