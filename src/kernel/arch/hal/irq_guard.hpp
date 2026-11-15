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

/// @brief This CPU's IrqGuard nesting depth.  Returns 0 iff no IrqGuard
///        scope is live on this CPU (interrupts may or may not be enabled:
///
/// depth tracks scoping, not the IF bit — query arch::interrupts_enabled()
/// for the bit).
inline uint64_t irq_guard_depth_own() noexcept {
    return irq_guard_depth[arch::cpu_index() % CONFIG_MAX_CPUS];
}

/// @brief RAII guard that disables interrupts on construction and restores
///        the previous interrupt state on destruction.
/// @note Use this to protect short critical sections from being interrupted.
class [[nodiscard]] IrqGuard {
  public:
    /// @brief Disable interrupts, saving the previous enabled state.
    IrqGuard() noexcept : irq_was_(interrupts_enabled()) {
        ++irq_guard_depth[arch::cpu_index() % CONFIG_MAX_CPUS];
        cli();
    }

    /// @brief Restore the interrupt state that was saved at construction.
    ~IrqGuard() noexcept {
        --irq_guard_depth[arch::cpu_index() % CONFIG_MAX_CPUS];
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

} // namespace arch
