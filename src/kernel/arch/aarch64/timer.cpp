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

/// @file timer.cpp
/// @brief AArch64 generic timer driver using CNTP_EL0 (physical timer).

#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/bootparams.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/nexios_config.h>

namespace arch {

/// @brief Counter ticks remaining until the armed deadline (0 when the
/// timer is disabled or expired). Shared by remaining()/remaining_ns().
/// @return Remaining counter ticks (clamped, never wraps).
static uint64_t counter_remaining();

constinit uint64_t Timer::ticks_ = 0;
uint64_t Timer::counter_freq_hz_ = 0;
uint32_t Timer::tick_interval_ = 0;
constinit TickSource Timer::active_source_ = TickSource::GENERIC_COUNTER;
constinit uint64_t Timer::last_ns_ = 0;
constinit bool Timer::calibrated_ = false;

/// @brief Initialise the timer with a given tick frequency and register the IRQ
/// handler.
/// @param[in] frequency_hz Desired timer interrupt frequency in Hz.
void Timer::init(uint32_t frequency_hz) {
    set_frequency(frequency_hz);
    IDT::register_handler(InterruptVector::TIMER,
                          [](uint64_t, uint64_t, uint64_t elr) {
                              handle_irq(elr);
                              // Issue #225: record for the debugger park
                              // (user-mode boundary by elr range).
                              kernel::Scheduler::note_debug_tick_pc(elr);
                              kernel::Scheduler::on_tick();
                          });
}

/// @brief Set the timer compare value to achieve a given frequency.
/// Programs an absolute CNTP_CVAL_EL0 deadline (now + interval) so a late
/// re-arm cannot land in the past and storm. Re-arming uses CVAL, not the
/// relative TVAL window.
/// @param[in] frequency_hz Desired frequency in Hz.
void Timer::set_frequency(uint32_t frequency_hz) {
    if (frequency_hz == 0) {
        return;
    }
    uint64_t freq{};
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    counter_freq_hz_ = freq;
    uint64_t interval = freq / frequency_hz;
    tick_interval_ = static_cast<uint32_t>(interval == 0 ? 1 : interval);
    uint64_t now{};
    asm volatile("mrs %0, cntpct_el0" : "=r"(now));
    const uint64_t deadline =
        (now > ~0ULL - tick_interval_) ? ~0ULL : now + tick_interval_;
    asm volatile("msr cntp_cval_el0, %0" : : "r"(deadline));
    asm volatile("msr cntp_ctl_el0, %0" : : "r"(1UL));
    isb();
}

/// @brief Return the total number of timer ticks since initialisation.
/// @return Tick count.
uint64_t Timer::ticks() {
    return ticks_;
}

/// @brief Return elapsed time in nanoseconds since boot.
/// @return Nanoseconds, or 0 if counter frequency is not yet known.
uint64_t Timer::ns() {
    if (counter_freq_hz_ == 0)
        return 0;
    uint64_t cnt{};
    asm volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    uint64_t sec = cnt / counter_freq_hz_;
    uint64_t rem = cnt % counter_freq_hz_;
    return sec * 1000000000ULL + (rem * 1000000000ULL) / counter_freq_hz_;
}

/// @brief Handle a timer interrupt: increment tick count and re-arm the timer.
/// Re-arms via an absolute CVAL deadline (now + latched interval) so a
/// delayed handler cannot program the past and storm on a level-asserted
/// line. Saturating tick count (no wrap).
/// @param ip Instruction pointer from interrupt frame.
void Timer::handle_irq(uint64_t ip) {
    (void)ip;
    ticks_ = ticks_ + 1;
    uint64_t interval = tick_interval_;
    if (interval == 0) {
        interval = 1;
    }
    uint64_t now{};
    asm volatile("mrs %0, cntpct_el0" : "=r"(now));
    const uint64_t deadline =
        (now > ~0ULL - interval) ? ~0ULL : now + interval;
    asm volatile("msr cntp_cval_el0, %0" : : "r"(deadline));
    asm volatile("msr cntp_ctl_el0, %0" : : "r"(1UL));
    isb();
}

/// @brief Return the calibrated counter frequency in Hz (x86 TSC alias).
uint64_t Timer::tsc_freq_hz() {
    return counter_freq_hz_;
}

uint64_t Timer::raw_counter() {
    uint64_t cnt{};
    asm volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    return cnt;
}

uint64_t Timer::raw_counter_freq_hz() {
    uint64_t freq{};
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq;
}

TickSource Timer::active_source() {
    return active_source_;
}

uint64_t Timer::freq_hz() {
    return counter_freq_hz_;
}

bool Timer::calibrate() {
    if (!calibrated_) {
        calibrated_ = true;
        if (counter_freq_hz_ == 0) {
            uint64_t freq{};
            asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
            counter_freq_hz_ = freq;
        }
        active_source_ = TickSource::GENERIC_COUNTER;
    }
    return counter_freq_hz_ != 0;
}

uint64_t Timer::ns_monotonic() {
    return detail::monotonic_commit(&last_ns_, ns());
}

uint64_t Timer::remaining_ns() {
    const uint64_t freq = counter_freq_hz_;
    if (freq == 0) {
        return 0;
    }
    const uint64_t rem = counter_remaining();
    return (rem / freq) * 1000000000ULL +
           ((rem % freq) * 1000000000ULL) / freq;
}

/// @brief Override the tick counter (used in test isolation).
/// @param[in] value Value to set ticks_ to.
void Timer::set_ticks_for_test(uint64_t value) {
    ticks_ = value;
}

/// @brief Arm the timer for a one-shot interrupt after a number of ticks.
/// @param[in] ticks_from_now Number of scheduler ticks from now.
/// Converts coarse ticks (BootParams::timer_hz, fallback CONFIG_TICK_HZ)
/// to counter ticks and arms an absolute CVAL deadline. Saturating math:
/// oversized requests clamp to the maximum deadline, never wrap.
/// 0 disarms (CTL=0); remaining() then reads 0.
void Timer::oneshot(uint64_t ticks_from_now) {
    if (ticks_from_now == 0) {
        // Disarm: clear the countdown and disable the physical timer so
        // remaining()/remaining_ns() observe 0 (0 = disarm per Timer
        // contract). CTL-gated remaining() makes the readback exact
        // (TVAL is count-minus-CVAL derived, not a stored zero).
        asm volatile("msr cntp_tval_el0, %0" : : "r"(0UL));
        asm volatile("msr cntp_ctl_el0, %0" : : "r"(0UL));
        isb();
        return;
    }
    const uint64_t boot_hz = kernel::BootParams::instance().timer_hz != 0
                                 ? kernel::BootParams::instance().timer_hz
                                 : CONFIG_TICK_HZ;
    const uint64_t freq = counter_freq_hz_;
    const uint64_t per_coarse = (freq == 0 || boot_hz == 0)
                                    ? 1ULL
                                    : freq / boot_hz == 0 ? 1ULL
                                                          : freq / boot_hz;
    uint64_t interval = (ticks_from_now > ~0ULL / per_coarse)
                            ? ~0ULL
                            : ticks_from_now * per_coarse;
    if (interval == 0) {
        interval = 1;
    }
    uint64_t now{};
    asm volatile("mrs %0, cntpct_el0" : "=r"(now));
    const uint64_t deadline =
        (now > ~0ULL - interval) ? ~0ULL : now + interval;
    asm volatile("msr cntp_cval_el0, %0" : : "r"(deadline));
    asm volatile("msr cntp_ctl_el0, %0" : : "r"(1UL));
    isb();
}

/// @brief Arm the timer for periodic interrupts.
/// @param[in] period_ticks Period in scheduler ticks (not Hz). Converts to
/// counter ticks at BootParams::timer_hz and latches the interval so the
/// ISR re-arms the same period. 0 disarms (CTL=0).
void Timer::periodic(uint64_t period_ticks) {
    if (period_ticks == 0) {
        // Disarm: clear the countdown and disable the physical timer so
        // remaining()/remaining_ns() observe 0 (0 = disarm per Timer
        // contract).
        asm volatile("msr cntp_tval_el0, %0" : : "r"(0UL));
        asm volatile("msr cntp_ctl_el0, %0" : : "r"(0UL));
        isb();
        return;
    }
    const uint64_t boot_hz = kernel::BootParams::instance().timer_hz != 0
                                 ? kernel::BootParams::instance().timer_hz
                                 : CONFIG_TICK_HZ;
    const uint64_t freq = counter_freq_hz_;
    const uint64_t per_coarse = (freq == 0 || boot_hz == 0)
                                    ? 1ULL
                                    : freq / boot_hz == 0 ? 1ULL
                                                          : freq / boot_hz;
    uint64_t interval = (period_ticks > ~0ULL / per_coarse)
                            ? ~0ULL
                            : period_ticks * per_coarse;
    if (interval == 0) {
        interval = 1;
    }
    tick_interval_ = (interval > 0xFFFFFFFFULL)
                         ? 0xFFFFFFFFU
                         : static_cast<uint32_t>(interval);
    uint64_t now{};
    asm volatile("mrs %0, cntpct_el0" : "=r"(now));
    const uint64_t deadline =
        (now > ~0ULL - interval) ? ~0ULL : now + interval;
    asm volatile("msr cntp_cval_el0, %0" : : "r"(deadline));
    asm volatile("msr cntp_ctl_el0, %0" : : "r"(1UL));
    isb();
}

/// @brief Counter ticks remaining until the armed deadline (0 when the
/// timer is disabled or expired). Shared by remaining()/remaining_ns().
/// @return Remaining counter ticks (clamped, never wraps).
static uint64_t counter_remaining() {
    uint64_t ctl{};
    asm volatile("mrs %0, cntp_ctl_el0" : "=r"(ctl));
    if ((ctl & 1ULL) == 0) {
        return 0;
    }
    uint64_t cval{};
    asm volatile("mrs %0, cntp_cval_el0" : "=r"(cval));
    uint64_t now{};
    asm volatile("mrs %0, cntpct_el0" : "=r"(now));
    return (cval > now) ? (cval - now) : 0;
}

/// @brief Read the remaining time on the timer in coarse scheduler ticks.
/// @return Remaining whole+partial ticks (ceil), 0 when disarmed/expired.
uint64_t Timer::remaining() {
    const uint64_t rem = counter_remaining();
    if (rem == 0) {
        return 0;
    }
    const uint64_t boot_hz = kernel::BootParams::instance().timer_hz != 0
                                 ? kernel::BootParams::instance().timer_hz
                                 : CONFIG_TICK_HZ;
    const uint64_t freq = counter_freq_hz_;
    if (freq == 0 || boot_hz == 0) {
        return rem;
    }
    const uint64_t per_coarse = freq / boot_hz == 0 ? 1 : freq / boot_hz;
    // Ceil without add-overflow: q + (r != 0), valid for saturated ~0ULL
    // deadlines where (rem + per_coarse - 1) would wrap (S3 hardening).
    return rem / per_coarse + (rem % per_coarse != 0 ? 1 : 0);
}

} // namespace arch
