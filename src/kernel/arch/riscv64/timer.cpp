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
constinit uint64_t Timer::timer_interval_ = 0;
constinit TickSource Timer::active_source_ = TickSource::MTIME;
constinit uint64_t Timer::last_ns_ = 0;
constinit bool Timer::calibrated_ = false;

/// @brief QEMU virt mtime input frequency (issue #198: was a bare 10000000
///        literal in two places). Real hardware fills this from the
///        device tree / SBI environment (issue #29 follow-up).
inline constexpr uint64_t RISCV_MTIME_FREQ_HZ = 10000000;
/// @brief SBI legacy SET_TIMER extension id (ecall a7 = 0).
inline constexpr uint64_t SBI_LEGACY_SET_TIMER = 0;
/// @brief Fail-closed disarm deadline: mtimecmp parked at the maximum so no
///        supervisor timer interrupt can fire (mtime is unreadable-free and
///        mtimecmp unwritable-clear from S-mode).
inline constexpr uint64_t MTIMECMP_DISARMED = ~0ULL;

/// @brief Initialize the timer, set frequency, and register the IRQ handler.
/// @param frequency_hz Desired tick frequency in Hz.
void Timer::init(uint32_t frequency_hz) {
    set_frequency(frequency_hz);
    IDT::register_handler(InterruptVector::TIMER,
                          [](uint64_t, uint64_t, uint64_t sepc) {
                              handle_irq(0);
                              // Issue #225: record for the debugger park
                              // (user-mode boundary by sepc range).
                              kernel::Scheduler::note_debug_tick_pc(sepc);
                              kernel::Scheduler::on_tick();
                          });
}

void Timer::set_frequency(uint32_t frequency_hz) {
    if (frequency_hz == 0) {
        return;
    }
    // Use SBI to get timer frequency (mtime typically 10MHz on QEMU)
    timer_freq_hz_ = RISCV_MTIME_FREQ_HZ;
    const uint64_t interval = timer_freq_hz_ / frequency_hz;
    timer_interval_ = (interval == 0) ? 1 : interval;
    // SBI_SET_TIMER via ecall
    asm volatile("mv a0, %0; mv a7, %1; ecall"
                 :
                 : "r"(interval), "r"(SBI_LEGACY_SET_TIMER)
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
/// @param ip Instruction pointer from interrupt frame (unused on riscv64 —
///        no sampler hook; kept for the shared Timer interface).
void Timer::handle_irq([[maybe_unused]] uint64_t ip) {
    ticks_ = ticks_ + 1;
    // Re-arm relative (now + latched interval): self-correcting under handler
    // delay, unlike the old absolute ticks_ * interval product which drifted
    // and could arm a deadline already in the past.
    uint64_t interval = timer_interval_;
    if (interval == 0) {
        interval = 1;
    }
    uint64_t now{};
    asm volatile("csrr %0, time" : "=r"(now));
    const uint64_t next = now + interval;
    asm volatile("mv a0, %0; mv a7, %1; ecall"
                 :
                 : "r"(next), "r"(SBI_LEGACY_SET_TIMER)
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

TickSource Timer::active_source() {
    return active_source_;
}

uint64_t Timer::freq_hz() {
    return timer_freq_hz_;
}

bool Timer::calibrate() {
    if (!calibrated_) {
        calibrated_ = true;
        if (timer_freq_hz_ == 0) {
            timer_freq_hz_ = RISCV_MTIME_FREQ_HZ;
        }
        active_source_ = TickSource::MTIME;
    }
    return timer_freq_hz_ != 0;
}

uint64_t Timer::ns_monotonic() {
    return detail::monotonic_commit(&last_ns_, ns());
}

uint64_t Timer::remaining_ns() {
    // mtimecmp is not readable from S-mode: no sub-tick remainder.
    (void)remaining();
    return 0;
}

/// @brief Arm a one-shot timer via SBI.
/// @param ticks_from_now Number of milliseconds from now to fire (0 = disarm:
///        mtimecmp is parked at the maximum — S-mode cannot clear a pending
///        STIP, only push its deadline out).
void Timer::oneshot(uint64_t ticks_from_now) {
    uint64_t now{};
    asm volatile("csrr %0, time" : "=r"(now));
    uint64_t deadline = MTIMECMP_DISARMED;
    if (ticks_from_now != 0) {
        uint64_t interval = ticks_from_now * (timer_freq_hz_ / 1000);
        if (interval == 0) {
            interval = 1;
        }
        deadline = now + interval;
    }
    asm volatile("mv a0, %0; mv a7, %1; ecall"
                 :
                 : "r"(deadline), "r"(SBI_LEGACY_SET_TIMER)
                 : "a0", "a7", "memory");
}

/// @brief Arm a periodic timer via SBI.
/// @param period_ticks Period in ticks (Hz). 0 = disarm (deadline parked at
///        the maximum); otherwise the handle_irq re-arm interval is latched
///        so the period persists across ticks.
void Timer::periodic(uint64_t period_ticks) {
    uint64_t now{};
    asm volatile("csrr %0, time" : "=r"(now));
    uint64_t deadline = MTIMECMP_DISARMED;
    if (period_ticks != 0) {
        uint64_t interval = timer_freq_hz_ / period_ticks;
        if (interval == 0) {
            interval = 1;
        }
        timer_interval_ = interval;
        deadline = now + interval;
    }
    asm volatile("mv a0, %0; mv a7, %1; ecall"
                 :
                 : "r"(deadline), "r"(SBI_LEGACY_SET_TIMER)
                 : "a0", "a7", "memory");
}

/// @brief Return time remaining on the current timer.
/// @return Always 0 (mtimecmp is not readable from S-mode).
/// @note This is a known limitation of S-mode on RISC-V.
uint64_t Timer::remaining() {
    return 0;
}

} // namespace arch
