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
/// @brief RISC-V64 timer driver — SBI-based timer with mtime/mtimecmp.

#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/nexios_config.h>

namespace arch {

constinit uint64_t Timer::ticks_ = 0;
uint64_t Timer::timer_freq_hz_ = 0;

/// @brief Initialize the timer, set frequency, and register the IRQ handler.
/// @param frequency_hz Desired tick frequency in Hz.
void Timer::init(uint32_t frequency_hz) {
    set_frequency(frequency_hz);
    IDT::register_handler(InterruptVector::TIMER,
                          [](uint64_t, uint64_t, uint64_t) {
                              handle_irq();
                              kernel::Scheduler::on_tick();
                          });
}

void Timer::set_frequency(uint32_t frequency_hz) {
    // Use SBI to get timer frequency (mtime typically 10MHz on QEMU)
    timer_freq_hz_ = 10000000;
    uint64_t interval = timer_freq_hz_ / frequency_hz;
    // SBI_SET_TIMER via ecall
    asm volatile("mv a0, %0; li a7, 0; ecall"
                 :
                 : "r"(interval)
                 : "a0", "a7", "memory");
    // Enable STIP in sie
    uint64_t sie{};
    asm volatile("csrr %0, sie" : "=r"(sie));
    sie |= (1ULL << 5); // STIE
    asm volatile("csrw sie, %0" : : "r"(sie) : "memory");
}

/// @brief Return the number of timer ticks since boot.
/// @return Tick counter value.
uint64_t Timer::ticks() {
    return ticks_;
}

/// @brief Return monotonic time in nanoseconds since boot.
/// @return Nanoseconds, or 0 if timer frequency is unset.
uint64_t Timer::ns() {
    if (timer_freq_hz_ == 0)
        return 0;
    uint64_t cnt{};
    asm volatile("csrr %0, time" : "=r"(cnt));
    uint64_t sec = cnt / timer_freq_hz_;
    uint64_t rem = cnt % timer_freq_hz_;
    return sec * 1000000000ULL + (rem * 1000000000ULL) / timer_freq_hz_;
}

/// @brief Handle a timer interrupt: increment tick count and re-arm via SBI.
void Timer::handle_irq() {
    ticks_ = ticks_ + 1;
    // Re-arm timer via SBI
    uint64_t next = ticks_ * (timer_freq_hz_ / CONFIG_TICK_HZ);
    asm volatile("mv a0, %0; li a7, 0; ecall"
                 :
                 : "r"(next)
                 : "a0", "a7", "memory");
}

/// @brief Override the tick counter for testing.
/// @param value New tick counter value.
void Timer::set_ticks_for_test(uint64_t value) {
    ticks_ = value;
}

/// @brief Return the calibrated timer counter frequency in Hz (x86 TSC alias).
/// @return Timer frequency in Hz.
uint64_t Timer::tsc_freq_hz() {
    return timer_freq_hz_;
}

/// @brief Arm a one-shot timer via SBI.
/// @param ticks_from_now Number of milliseconds from now to fire.
void Timer::oneshot(uint64_t ticks_from_now) {
    uint64_t interval = ticks_from_now * (timer_freq_hz_ / 1000);
    if (interval == 0)
        interval = 1;
    uint64_t now{};
    asm volatile("csrr %0, time" : "=r"(now));
    asm volatile("mv a0, %0; li a7, 0; ecall"
                 :
                 : "r"(now + interval)
                 : "a0", "a7", "memory");
}

/// @brief Arm a periodic timer via SBI.
/// @param period_ticks Period in ticks (Hz).
void Timer::periodic(uint64_t period_ticks) {
    uint64_t interval = timer_freq_hz_ / period_ticks;
    if (interval == 0)
        interval = 1;
    uint64_t now{};
    asm volatile("csrr %0, time" : "=r"(now));
    asm volatile("mv a0, %0; li a7, 0; ecall"
                 :
                 : "r"(now + interval)
                 : "a0", "a7", "memory");
}

/// @brief Return time remaining on the current timer.
/// @return Always 0 (mtimecmp is not readable from S-mode).
/// @note This is a known limitation of S-mode on RISC-V.
uint64_t Timer::remaining() {
    return 0;
}

} // namespace arch
