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
#include <kernel/task/scheduler.hpp>
#include <kernel/nexios_config.h>

namespace arch {

volatile uint64_t Timer::ticks_ = 0;
uint64_t Timer::counter_freq_hz_ = 0;

/// @brief Initialise the timer with a given tick frequency and register the IRQ
/// handler.
/// @param[in] frequency_hz Desired timer interrupt frequency in Hz.
void Timer::init(uint32_t frequency_hz) {
    set_frequency(frequency_hz);
    IDT::register_handler(InterruptVector::TIMER,
                          [](uint64_t, uint64_t, uint64_t) {
                              handle_irq();
                              kernel::Scheduler::on_tick();
                          });
}

/// @brief Set the timer compare value to achieve a given frequency.
/// Reads CNTFRQ_EL0 to calculate the interval and writes it to CNTP_TVAL_EL0.
/// @param[in] frequency_hz Desired frequency in Hz.
void Timer::set_frequency(uint32_t frequency_hz) {
    uint64_t freq{};
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    counter_freq_hz_ = freq;
    uint64_t interval = freq / frequency_hz;
    asm volatile("msr cntp_tval_el0, %0" : : "r"(interval));
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
/// Re-writes CNTP_CTL_EL0 to clear the interrupt status.
void Timer::handle_irq() {
    ticks_ = ticks_ + 1;
    asm volatile("msr cntp_ctl_el0, %0" : : "r"(1UL));
    isb();
}

/// @brief Return the calibrated counter frequency in Hz (x86 TSC alias).
uint64_t Timer::tsc_freq_hz() {
    return counter_freq_hz_;
}

/// @brief Override the tick counter (used in test isolation).
/// @param[in] value Value to set ticks_ to.
void Timer::set_ticks_for_test(uint64_t value) {
    ticks_ = value;
}

/// @brief Arm the timer for a one-shot interrupt after a number of ticks.
/// @param[in] ticks_from_now Number of scheduler ticks (ms) from now.
void Timer::oneshot(uint64_t ticks_from_now) {
    uint64_t interval = ticks_from_now * (counter_freq_hz_ / 1000);
    if (interval == 0)
        interval = 1;
    asm volatile("msr cntp_tval_el0, %0" : : "r"(interval));
    asm volatile("msr cntp_ctl_el0, %0" : : "r"(1UL));
    isb();
}

/// @brief Arm the timer for periodic interrupts.
/// @param[in] period_ticks Interval in scheduler ticks (ms).
void Timer::periodic(uint64_t period_ticks) {
    uint64_t freq{};
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t interval = freq / period_ticks;
    if (interval == 0)
        interval = 1;
    asm volatile("msr cntp_tval_el0, %0" : : "r"(interval));
    asm volatile("msr cntp_ctl_el0, %0" : : "r"(1UL));
    isb();
}

/// @brief Read the remaining time on the timer (CNTP_TVAL_EL0).
/// @return Remaining timer value in counter ticks.
uint64_t Timer::remaining() {
    uint64_t val{};
    asm volatile("mrs %0, cntp_tval_el0" : "=r"(val));
    return val;
}

} // namespace arch
