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
/// @brief PIT (Programmable Interval Timer) driver — generates periodic ticks
/// and calibrates the TSC.

#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/arch/apic.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/profiling/sampler.hpp>

namespace arch {

/// @brief Monotonic tick counter, incremented on each timer IRQ.
constinit uint64_t Timer::ticks_ = 0;
/// @brief Calibrated TSC frequency in Hz.
constinit uint64_t Timer::tsc_freq_hz_ = 0;

/// @brief Initialise the system timer.
/// When CONFIG_USE_APIC_TIMER=1 and the APIC was successfully enabled, the
/// APIC timer drives the system tick; otherwise the legacy PIT is used.
/// @param frequency_hz Desired tick frequency in Hertz.
void Timer::init(uint32_t frequency_hz) {
    // Always calibrate TSC using the PIT (it's the most reliable method
    // across QEMU/hardware, and is independent of the tick source).
    calibrate_tsc(frequency_hz);

    #if CONFIG_USE_APIC_TIMER
    if (arch::APIC::is_enabled()) {
        // Register the tick handler at the dedicated APIC timer vector.
        // The handler increments ticks and calls the scheduler.
        IDT::register_handler_raw(APIC::APIC_TIMER_VECTOR,
                                  [](uint64_t, uint64_t, uint64_t rip) {
                                      handle_irq(rip);
                                      kernel::Scheduler::on_tick();
                                      // Re-arm TSC-deadline for next tick
                                      if (arch::APIC::is_timer_active())
                                          arch::APIC::timer_start();
                                  });
        // Initialise and start the APIC timer (TSC-deadline or periodic).
        arch::APIC::timer_init(frequency_hz);
        arch::APIC::timer_start();
        // The PIT is no longer needed for ticks — stop programming it.
        // (The PIT counter continues to count but its output is ignored
        //  by the I/O APIC since IRQ0 is routed through the APIC.)
        return;
    }
    // Fall through to PIT if APIC is not enabled
#endif

    // Legacy PIT timer
    set_frequency(frequency_hz);
    IDT::register_handler(InterruptVector::TIMER,
                          [](uint64_t, uint64_t, uint64_t rip) {
                              handle_irq(rip);
                              kernel::Scheduler::on_tick();
                          });
}

/// @brief Program the PIT to fire at a given frequency.
/// @param frequency_hz Desired frequency; the PIT base frequency is divided by
/// this value.
void Timer::set_frequency(uint32_t frequency_hz) {
    uint32_t divisor = PIT_BASE_FREQ / frequency_hz;

    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

/// @brief Return the current tick count.
/// @return Number of PIT IRQs received since initialisation.
uint64_t Timer::ticks() {
    return __atomic_load_n(&ticks_, __ATOMIC_ACQUIRE);
}

/// @brief PIT IRQ handler — increments the tick counter.
/// @param ip Instruction pointer from interrupt frame (RIP on x86_64).
void Timer::handle_irq(uint64_t ip) {
    __atomic_fetch_add(&ticks_, 1UL, __ATOMIC_RELAXED);
    kernel::profiling::Sampler::record_sample(ip);
}

/// @brief Calibrate the TSC frequency using the PIT as a reference.
/// Performs up to 12 iterations with increasing measurement windows and picks
/// the first result that falls in a plausible range (50 MHz – 100 GHz).
/// @param frequency_hz The PIT frequency used as the timing reference.
void Timer::calibrate_tsc(uint32_t frequency_hz) {
    uint32_t divisor = PIT_BASE_FREQ / frequency_hz;

    for (int retry = 0; retry < 12; ++retry) {
        outb(0x43, 0x00);
        uint16_t c0 = inb(0x40) | ((uint16_t)inb(0x40) << 8);
        uint64_t t0 = rdtsc();

        uint64_t tsc_target = t0 + 50000ULL * (1 << retry);
        while (rdtsc() < tsc_target) {
            asm volatile("pause" : : : "memory");
        }

        outb(0x43, 0x00);
        uint16_t c1 = inb(0x40) | ((uint16_t)inb(0x40) << 8);
        uint64_t t1 = rdtsc();

        uint64_t tsc_delta = t1 - t0;
        uint32_t count_delta{};
        if (c1 <= c0) {
            count_delta = c0 - c1;
        } else {
            count_delta = c0 + (divisor - c1);
        }

        if (count_delta > 0 && tsc_delta > 0) {
            uint64_t pit_ticks = (uint64_t)count_delta * 2;
            tsc_freq_hz_ = (tsc_delta * PIT_BASE_FREQ) / pit_ticks;
            if (tsc_freq_hz_ >= 50000000ULL &&
                tsc_freq_hz_ <= 100000000000ULL) {
                return;
            }
        }
    }

    tsc_freq_hz_ = 2000000000ULL;
}

/// @brief Return the calibrated TSC frequency in Hz.
uint64_t Timer::tsc_freq_hz() {
    return tsc_freq_hz_;
}

/// @brief Return the time elapsed since boot in nanoseconds.
/// Uses the calibrated TSC frequency to convert TSC ticks to nanoseconds.
/// @return Nanoseconds since boot, or 0 if the TSC has not been calibrated.
uint64_t Timer::ns() {
    if (tsc_freq_hz_ == 0)
        return 0;
    uint64_t tsc = rdtsc();
    // 64-bit: use (tsc / freq) * 1e9 with remainder to avoid 128-bit libcall
    uint64_t sec = tsc / tsc_freq_hz_;
    uint64_t rem = tsc % tsc_freq_hz_;
    return sec * 1000000000ULL + (rem * 1000000000ULL) / tsc_freq_hz_;
}

/// @brief Override the tick counter (test support).
/// @param value Value to assign to the tick counter.
void Timer::set_ticks_for_test(uint64_t value) {
    __atomic_store_n(&ticks_, value, __ATOMIC_RELEASE);
}

} // namespace arch
