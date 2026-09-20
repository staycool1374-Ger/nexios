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

/// @brief Tick source backing the HRT monotonic clock (issue #16).
enum class TickSource : uint8_t {
    PIT = 0,            ///< Legacy PIT channel 0 (fallback).
    TSC = 1,            ///< Calibrated TSC over PIT/APIC tick.
    TSC_DEADLINE = 2,   ///< TSC with APIC TSC-deadline mode.
    HPET = 3,           ///< HPET main counter (when probed).
    GENERIC_COUNTER = 4,///< AArch64 generic counter (CNTFRQ/CNTPCT).
    MTIME = 5,          ///< RISC-V mtime.
    TEST = 6            ///< Test override (unused by production).
};

namespace detail {
/// @brief Lock-free monotonic commit: stores max(*cell, now).
/// @param cell Pointer to the last-reported value (atomics-only).
/// @param now New sample.
/// @return Monotonic value to report.
inline uint64_t monotonic_commit(uint64_t* cell, uint64_t now) {
    uint64_t prev = __atomic_load_n(cell, __ATOMIC_ACQUIRE);
    while (now > prev) {
        uint64_t expect = prev;
        const bool swapped = __atomic_compare_exchange_n(
            cell, &expect, now, false, __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE);
        if (swapped) {
            prev = now;
            break;
        }
        prev = expect;
    }
    return (now > prev) ? now : prev;
}
} // namespace detail

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
    /// @brief Monotonic nanoseconds since boot (issue #16, IRQ-safe).
    /// Never decreases on the calling CPU (lock-free last-value clamp).
    /// @return Monotonic elapsed nanoseconds.
    static uint64_t ns_monotonic();
    /// @brief Which source backs ns_monotonic() (set by calibrate()).
    /// @return Active tick source.
    static TickSource active_source();
    /// @brief One-time source calibration (BOOT_ONLY, idempotent).
    /// Fail-closed: falls back down TSC → HPET → PIT, frequency stays
    /// nonzero. Refreshes the source label on repeat calls (APIC caps
    /// are probed after Timer::init).
    /// @return true on the preferred source, false on fallback.
    static bool calibrate();
    /// @brief Unified backing frequency in Hz (0 before calibrate()).
    /// @return Backing frequency in Hz.
    static uint64_t freq_hz();
    /// @brief Change the tick frequency at runtime.
    /// @param frequency_hz New tick rate in Hz.
    static void set_frequency(uint32_t frequency_hz);
    /// @brief Handle a timer interrupt (increment tick count).
    /// @param ip Instruction pointer from interrupt frame (RIP on x86_64).
    static void handle_irq(uint64_t ip);
    /// @brief Override the tick count (test support).
    /// @param value New tick value.
    static void set_ticks_for_test(uint64_t value);

    /// @brief Schedule a one-shot timer interrupt.
    /// Delegates to the APIC timer when active; on PIT-only configs the
    /// IRQ is not reprogrammed (system tick preservation) and only the
    /// software deadline is armed for remaining()/remaining_ns().
    /// @param ticks_from_now Number of coarse ticks from now (0 = disarm).
    static void oneshot(uint64_t ticks_from_now);
    /// @brief Configure a periodic timer interrupt.
    /// @param period_ticks Period in coarse ticks (0 = disarm).
    static void periodic(uint64_t period_ticks);
    /// @brief Get the remaining ticks on the armed deadline.
    /// @return Remaining coarse ticks (0 when disarmed or expired).
    static uint64_t remaining();
    /// @brief Sub-tick nanoseconds remaining on the armed deadline.
    /// @return Remaining nanoseconds (0 when disarmed or expired).
    static uint64_t remaining_ns();
    /// @brief Return the calibrated TSC frequency in Hz.
    static uint64_t tsc_freq_hz();

  private:
    /// @brief Calibrate the TSC frequency using the PIT as a reference.
    /// Performs up to CONFIG_HRT_CALIBRATION_RETRIES iterations with
    /// increasing measurement windows and picks the first result that
    /// falls in a plausible range (50 MHz – 100 GHz), bounded in wall
    /// time by CONFIG_HRT_CALIBRATION_TIMEOUT_MS (50 MHz floor).
    /// @param frequency_hz The PIT frequency used as the timing reference.
    static void calibrate_tsc(uint32_t frequency_hz);
    /// @brief Resolve active_source_/calibrate_ok_ from calibrated state.
    static void resolve_source();
    /// @brief PIT base input frequency (1.193182 MHz).
    static constexpr uint32_t PIT_BASE_FREQ = 1193182;
    static constinit uint64_t ticks_;
    static constinit uint64_t tsc_freq_hz_;
    static constinit TickSource active_source_;
    static constinit uint64_t last_ns_;
    static constinit uint64_t deadline_tsc_;
    static constinit uint32_t last_freq_hz_;
    static constinit bool calibrated_;
    static constinit bool calibrate_ok_;
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
    /// @brief Monotonic nanoseconds since boot (issue #16, IRQ-safe).
    static uint64_t ns_monotonic();
    /// @brief Which source backs ns_monotonic() (set by calibrate()).
    static TickSource active_source();
    /// @brief One-time source calibration (BOOT_ONLY, idempotent).
    /// @return true on the preferred source, false on fallback.
    static bool calibrate();
    /// @brief Unified backing frequency in Hz (0 before calibrate()).
    static uint64_t freq_hz();
    /// @brief Change the tick frequency at runtime.
    static void set_frequency(uint32_t frequency_hz);
    /// @brief Handle a timer interrupt.
    /// @param ip Instruction pointer from interrupt frame.
    static void handle_irq(uint64_t ip);
    /// @brief Override the tick counter (test support).
    static void set_ticks_for_test(uint64_t value);

    /// @brief Schedule a one-shot timer interrupt.
    static void oneshot(uint64_t ticks_from_now);
    /// @brief Configure a periodic timer interrupt.
    static void periodic(uint64_t period_ticks);
    /// @brief Get the remaining ticks on the current timer.
    static uint64_t remaining();
    /// @brief Sub-tick nanoseconds remaining on the current timer.
    static uint64_t remaining_ns();
    /// @brief Return the calibrated counter frequency in Hz (x86 TSC alias).
    static uint64_t tsc_freq_hz();

  private:
    static constinit uint64_t ticks_;
    static constinit uint64_t counter_freq_hz_;
    /// @brief Current compare interval in counter ticks (re-arm value).
    static constinit uint32_t tick_interval_;
    static constinit TickSource active_source_;
    static constinit uint64_t last_ns_;
    static constinit bool calibrated_;
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
    /// @brief Monotonic nanoseconds since boot (issue #16, IRQ-safe).
    static uint64_t ns_monotonic();
    /// @brief Which source backs ns_monotonic() (set by calibrate()).
    static TickSource active_source();
    /// @brief One-time source calibration (BOOT_ONLY, idempotent).
    /// @return true on the preferred source, false on fallback.
    static bool calibrate();
    /// @brief Unified backing frequency in Hz (0 before calibrate()).
    static uint64_t freq_hz();
    /// @brief Change the tick frequency at runtime.
    static void set_frequency(uint32_t frequency_hz);
    /// @brief Handle a timer interrupt.
    /// @param ip Instruction pointer from interrupt frame.
    static void handle_irq(uint64_t ip);
    /// @brief Override the tick count (test support).
    static void set_ticks_for_test(uint64_t value);

    /// @brief Schedule a one-shot timer interrupt.
    static void oneshot(uint64_t ticks_from_now);
    /// @brief Configure a periodic timer interrupt.
    static void periodic(uint64_t period_ticks);
    /// @brief Get the remaining ticks on the current timer.
    static uint64_t remaining();
    /// @brief Sub-tick nanoseconds remaining (0: mtimecmp unreadable).
    static uint64_t remaining_ns();
    /// @brief Return the calibrated counter frequency in Hz (x86 TSC alias).
    static uint64_t tsc_freq_hz();

  private:
    static constinit uint64_t ticks_;
    static constinit uint64_t timer_freq_hz_;
    /// @brief Latched re-arm interval in mtime ticks (issue #198): written by
    ///        set_frequency()/periodic(), consumed by handle_irq() so the
    ///        deadline stays relative (now + interval) instead of drifting
    ///        on the absolute ticks_ * interval product.
    static constinit uint64_t timer_interval_;
    static constinit TickSource active_source_;
    static constinit uint64_t last_ns_;
    static constinit bool calibrated_;
};

/// @cond
#else
#error "HAL: no timer implementation for this architecture"
#endif
/// @endcond

/// @brief Architecture-independent timer type alias.
using ArchTimer = Timer;

} // namespace arch
