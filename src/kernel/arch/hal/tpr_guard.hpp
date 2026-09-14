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

/// @file tpr_guard.hpp
/// @brief RAII TPR-class raise — mask an interrupt priority band on
/// construction, restore the saved class on destruction (issue #26).
/// Own header (NOT inside irq_guard.hpp): needs the APIC + PerCpu
/// declarations, which irq_guard.hpp must not depend on.
/// See docs/specs/apic-tpr.md §3.5.

#pragma once

#include <types.hpp>
#include <assert.hpp>
#include <kernel/arch/hal/io.hpp>
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/apic.hpp>
#include <kernel/arch/x86_64/hal/percpu.hpp>
#endif

namespace arch {

/// @brief RAII guard that raises the calling CPU's TPR to a priority
///        class on construction and restores the saved class on
///        destruction.  Never replaces IrqGuard: callers keep their
///        existing scheduler_lock_ + IF=0 discipline (INV-TPR2); TPR
///        only narrows which vectors may preempt.  No-op when the
///        LAPIC is off (accessors are fail-closed).
class [[nodiscard]] TprGuard {
  public:
    /// @brief Raise the own-CPU TPR class, saving the previous class.
    /// @param cls Target class (only bits 7:4 used; > MAX is rejected
    ///        and leaves the class unchanged).
    explicit TprGuard(uint8_t cls) noexcept : saved_(current_class()) {
#if defined(CONFIG_ARCH_X86_64)
        // INV-TPR2 enforced: the raise/shadow pair is only atomic vs
        // interrupts under IF=0.  TPR narrows which vectors may preempt;
        // it never replaces the caller's lock + cli discipline.
        ENSURE(!interrupts_enabled());
#endif
        raise(cls);
    }

    /// @brief Restore the class saved at construction.
    ~TprGuard() noexcept {
        restore(saved_);
    }

    TprGuard(const TprGuard &) = delete;
    TprGuard &operator=(const TprGuard &) = delete;
    TprGuard(TprGuard &&) = delete;
    TprGuard &operator=(TprGuard &&) = delete;

  private:
    /// @brief Saved own-CPU class from before the guard.
    uint8_t saved_;

    /// @brief Read the own-CPU intended class (shadow on x86_64).
    static uint8_t current_class() noexcept {
#if defined(CONFIG_ARCH_X86_64)
        return static_cast<uint8_t>(
            per_cpu_current()->tpr_shadow & 0xF0U);
#else
        return 0;
#endif
    }

    /// @brief Raise HW + shadow; records the new class in the shadow.
    static void raise(uint8_t cls) noexcept {
#if defined(CONFIG_ARCH_X86_64)
        APIC::tpr_raise(cls);
        per_cpu_current()->tpr_shadow =
            static_cast<uint64_t>(APIC::get_tpr_class());
#else
        (void)cls;
#endif
    }

    /// @brief Restore HW + shadow to a previously saved class.
    static void restore(uint8_t cls) noexcept {
#if defined(CONFIG_ARCH_X86_64)
        APIC::tpr_restore(cls);
        per_cpu_current()->tpr_shadow =
            static_cast<uint64_t>(APIC::get_tpr_class());
#else
        (void)cls;
#endif
    }
};

} // namespace arch
