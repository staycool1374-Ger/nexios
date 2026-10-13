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

namespace arch {

/// @brief RAII guard that disables interrupts on construction and restores
///        the previous interrupt state on destruction.
/// @note Use this to protect short critical sections from being interrupted.
class [[nodiscard]] IrqGuard {
  public:
    /// @brief Disable interrupts, saving the previous enabled state.
    IrqGuard() noexcept : irq_was_(interrupts_enabled()) {
        cli();
    }

    /// @brief Restore the interrupt state that was saved at construction.
    ~IrqGuard() noexcept {
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
