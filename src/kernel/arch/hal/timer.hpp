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

/// @file timer.hpp
/// @brief Timer abstraction — PIT+TSC (x86_64), generic timer (AArch64), timer
/// (RISC-V).

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace arch {

/// @cond
#if defined(CONFIG_ARCH_X86_64)
/// @endcond

/// @brief x86-64 timer driver — PIT + TSC calibration for tick and nanosecond
/// timing.
class Timer {
  public:
    /// @brief Initialise the timer with a target tick frequency.
    /// @param frequency_hz Desired tick rate in Hz.
    static void init(uint32_t frequency_hz);
    /// @brief Get the current tick count.
    /// @return Monotonic tick count.
    static uint64_t ticks();
    /// @brief Estimate nanoseconds since boot.
    /// @return Approximate elapsed nanoseconds.
    static uint64_t ns();
    /// @brief Change the tick frequency at runtime.
    /// @param frequency_hz New tick rate in Hz.
    static void set_frequency(uint32_t frequency_hz);
    /// @brief Handle a timer interrupt (increment tick count).
    static void handle_irq();
    /// @brief Override the tick count (test support).
    /// @param value New tick value.
    static void set_ticks_for_test(uint64_t value);

    /// @brief Schedule a one-shot timer interrupt.
    /// @param ticks_from_now Number of ticks from now.
    static void oneshot(uint64_t ticks_from_now);
    /// @brief Configure a periodic timer interrupt.
    /// @param period_ticks Period in ticks.
    static void periodic(uint64_t period_ticks);
    /// @brief Get the remaining ticks on the current timer.
    /// @return Remaining ticks.
    static uint64_t remaining();
    /// @brief Return the calibrated TSC frequency in Hz.
    static uint64_t tsc_freq_hz();

  private:
    /// @brief Calibrate the TSC frequency using the PIT.
    /// @param frequency_hz Target PIT frequency.
    static void calibrate_tsc(uint32_t frequency_hz);
    /// @brief PIT base input frequency (1.193182 MHz).
    static constexpr uint32_t PIT_BASE_FREQ = 1193182;
    static constinit uint64_t ticks_;
    static constinit uint64_t tsc_freq_hz_;
};

/// @cond
#elif defined(CONFIG_ARCH_AARCH64)
/// @endcond

/// @brief AArch64 timer driver — generic system timer.
class Timer {
  public:
    /// @brief Initialise the timer with a target tick frequency.
    static void init(uint32_t frequency_hz);
    /// @brief Get the current tick count.
    static uint64_t ticks();
    /// @brief Estimate nanoseconds since boot.
    static uint64_t ns();
    /// @brief Change the tick frequency at runtime.
    static void set_frequency(uint32_t frequency_hz);
    /// @brief Handle a timer interrupt.
    static void handle_irq();
    /// @brief Override the tick count (test support).
    static void set_ticks_for_test(uint64_t value);

    /// @brief Schedule a one-shot timer interrupt.
    static void oneshot(uint64_t ticks_from_now);
    /// @brief Configure a periodic timer interrupt.
    static void periodic(uint64_t period_ticks);
    /// @brief Get the remaining ticks on the current timer.
    static uint64_t remaining();
    /// @brief Return the calibrated counter frequency in Hz (x86 TSC alias).
    static uint64_t tsc_freq_hz();

  private:
    static constinit uint64_t ticks_;
    static constinit uint64_t counter_freq_hz_;
    /// @brief Current compare interval in counter ticks (re-arm value).
    static constinit uint32_t tick_interval_;
};

/// @cond
#elif defined(CONFIG_ARCH_RISCV64)
/// @endcond

/// @brief RISC-V 64 timer driver — machine/supervisor timer (mtime).
class Timer {
  public:
    /// @brief Initialise the timer with a target tick frequency.
    static void init(uint32_t frequency_hz);
    /// @brief Get the current tick count.
    static uint64_t ticks();
    /// @brief Estimate nanoseconds since boot.
    static uint64_t ns();
    /// @brief Change the tick frequency at runtime.
    static void set_frequency(uint32_t frequency_hz);
    /// @brief Handle a timer interrupt.
    static void handle_irq();
    /// @brief Override the tick count (test support).
    static void set_ticks_for_test(uint64_t value);

    /// @brief Schedule a one-shot timer interrupt.
    static void oneshot(uint64_t ticks_from_now);
    /// @brief Configure a periodic timer interrupt.
    static void periodic(uint64_t period_ticks);
    /// @brief Get the remaining ticks on the current timer.
    static uint64_t remaining();
    /// @brief Return the calibrated counter frequency in Hz (x86 TSC alias).
    static uint64_t tsc_freq_hz();

  private:
    static constinit uint64_t ticks_;
    static constinit uint64_t timer_freq_hz_;
};

/// @cond
#else
#error "HAL: no timer implementation for this architecture"
#endif
/// @endcond

/// @brief Architecture-independent timer type alias.
using ArchTimer = Timer;

} // namespace arch
