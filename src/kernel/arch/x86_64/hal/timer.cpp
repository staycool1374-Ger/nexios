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
#include <kernel/arch/x86_64/hal/percpu.hpp>
#include <kernel/arch/x86_64/hal/hpet.hpp>
#include <kernel/bootparams.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/profiling/sampler.hpp>

namespace arch {

namespace {
/// @brief Maximum deadline span (1 hour in ns): bounds all products.
static constexpr uint64_t k_max_deadline_ns = 3600000000000ULL;
/// @brief Calibration timeout floor (50 MHz in ticks per ms).
static constexpr uint64_t k_timeout_ticks_per_ms = 50000ULL;
/// @brief Plausible TSC range (50 MHz – 100 GHz).
static constexpr uint64_t k_tsc_min_hz = 50000000ULL;
static constexpr uint64_t k_tsc_max_hz = 100000000000ULL;
/// @brief Fail-closed TSC frequency when calibration finds nothing.
static constexpr uint64_t k_fallback_tsc_hz = 2000000000ULL;

/// @brief Convert coarse ticks to nanoseconds without 64-bit overflow.
/// @param tick_count Coarse tick count.
/// @param rate_hz Tick rate in Hz.
/// @return Nanoseconds clamped to k_max_deadline_ns (0 when rate is 0).
uint64_t ticks_to_ns(uint64_t tick_count, uint64_t rate_hz) {
    if (rate_hz == 0) {
        return 0;
    }
    const uint64_t cap_ticks = 3600ULL * rate_hz;
    if (tick_count > cap_ticks) {
        return k_max_deadline_ns;
    }
    const uint64_t whole = tick_count / rate_hz;
    const uint64_t part = tick_count % rate_hz;
    return whole * 1000000000ULL + (part * 1000000000ULL) / rate_hz;
}

/// @brief Convert nanoseconds to a TSC delta without 64-bit overflow.
/// @param delay_ns Delay in nanoseconds (clamped to k_max_deadline_ns).
/// @param freq_hz TSC frequency in Hz.
/// @return TSC delta (0 when freq is 0).
uint64_t hrt_ns_to_tsc(uint64_t delay_ns, uint64_t freq_hz) {
    if (freq_hz == 0) {
        return 0;
    }
    uint64_t bounded_ns = delay_ns;
    if (bounded_ns > k_max_deadline_ns) {
        bounded_ns = k_max_deadline_ns;
    }
    const uint64_t whole = bounded_ns / 1000000000ULL;
    const uint64_t part = bounded_ns % 1000000000ULL;
    const uint64_t part_hi = part / 1000ULL;
    const uint64_t part_lo = part % 1000ULL;
    return whole * freq_hz + part_hi * freq_hz / 1000000ULL +
           part_lo * freq_hz / 1000000000ULL;
}
} // namespace

/// @brief Monotonic tick counter, incremented on each timer IRQ.
constinit uint64_t Timer::ticks_ = 0;
/// @brief Calibrated TSC frequency in Hz.
constinit uint64_t Timer::tsc_freq_hz_ = 0;
/// @brief Active HRT tick source (set by resolve_source).
constinit TickSource Timer::active_source_ = TickSource::PIT;
/// @brief Last reported monotonic sample (atomics-only, see ns_monotonic).
constinit uint64_t Timer::last_ns_ = 0;
/// @brief Armed deadline in TSC units (0 = disarmed).
constinit uint64_t Timer::deadline_tsc_ = 0;
/// @brief Tick rate passed to init() (for tick/ns conversion).
constinit uint32_t Timer::last_freq_hz_ = 0;
/// @brief Calibration latch (calibrate() is idempotent).
constinit bool Timer::calibrated_ = false;
/// @brief Cached calibration result (true = preferred source).
constinit bool Timer::calibrate_ok_ = false;

/// @brief Initialise the system timer.
/// When CONFIG_USE_APIC_TIMER=1 and the APIC was successfully enabled, the
/// APIC timer drives the system tick; otherwise the legacy PIT is used.
/// @param frequency_hz Desired tick frequency in Hertz.
void Timer::init(uint32_t frequency_hz) {
    // Always calibrate TSC using the PIT (it's the most reliable method
    // across QEMU/hardware, and is independent of the tick source).
    last_freq_hz_ = frequency_hz;
    calibrate_tsc(frequency_hz);

    #if CONFIG_USE_APIC_TIMER
    if (arch::APIC::is_enabled()) {
        // Register the tick handler at the dedicated APIC timer vector.
        // The handler increments ticks and calls the scheduler.
        IDT::register_handler_raw(APIC::APIC_TIMER_VECTOR,
                                  [](uint64_t, uint64_t, uint64_t rip) {
                                      // Issue #26: the tick never runs
                                      // raised — heal any stale class on
                                      // entry (covers BSP + AP branches;
                                      // the ISR tail restores from the
                                      // shadow before return).
                                      arch::per_cpu_current()->tpr_shadow =
                                          APIC::TPR_CLASS_ACCEPT_ALL;
                                      APIC::tpr_restore(
                                          APIC::TPR_CLASS_ACCEPT_ALL);
                                      if (arch::cpu_index() != 0) {
                                          kernel::Scheduler::ap_tick();
                                          if (arch::APIC::is_timer_active())
                                              arch::APIC::timer_start();
                                          return;
                                      }
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
    if (frequency_hz == 0) {
        return;
    }
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
/// Performs up to CONFIG_HRT_CALIBRATION_RETRIES iterations with increasing
/// measurement windows and picks the first result that falls in a plausible
/// range (50 MHz – 100 GHz). Bounded in wall time by
/// CONFIG_HRT_CALIBRATION_TIMEOUT_MS at a 50 MHz floor; on failure the
/// fail-closed 2 GHz default applies. Resolves the HRT source label.
/// @param frequency_hz The PIT frequency used as the timing reference.
void Timer::calibrate_tsc(uint32_t frequency_hz) {
    uint32_t divisor = PIT_BASE_FREQ / frequency_hz;
    calibrated_ = true;
    const uint64_t cal_start = rdtsc();
    const uint64_t timeout_ticks =
        (uint64_t)CONFIG_HRT_CALIBRATION_TIMEOUT_MS *
        k_timeout_ticks_per_ms;

    for (int retry = 0; retry < CONFIG_HRT_CALIBRATION_RETRIES; ++retry) {
        if (rdtsc() - cal_start > timeout_ticks) {
            break;
        }
        outb(0x43, 0x00);
        uint16_t c0 = inb(0x40) | ((uint16_t)inb(0x40) << 8);
        uint64_t t0 = rdtsc();

        uint64_t tsc_target = t0 + 50000ULL * (1ULL << retry);
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
            if (tsc_freq_hz_ >= k_tsc_min_hz &&
                tsc_freq_hz_ <= k_tsc_max_hz) {
                resolve_source();
                return;
            }
        }
    }

    tsc_freq_hz_ = k_fallback_tsc_hz;
    resolve_source();
}

/// @brief Resolve active_source_/calibrate_ok_ from calibrated state.
/// Preference chain (CONFIG_HRT_SOURCE_PREFERENCE): prefer HPET (2) takes
/// a probed HPET; otherwise a valid TSC wins; a probed HPET is the auto
/// (0) fallback; anything else records PIT with the fail-closed frequency.
void Timer::resolve_source() {
    const bool hpet_ok =
        hpet::probe() && hpet::freq_hz() != 0;
    const bool tsc_ok = tsc_freq_hz_ >= k_tsc_min_hz &&
                        tsc_freq_hz_ <= k_tsc_max_hz;
    if (CONFIG_HRT_SOURCE_PREFERENCE == 2 && hpet_ok) {
        active_source_ = TickSource::HPET;
        calibrate_ok_ = true;
        return;
    }
    if (tsc_ok) {
        calibrate_ok_ = true;
        active_source_ = APIC::has_tsc_deadline() ? TickSource::TSC_DEADLINE
                                                  : TickSource::TSC;
        return;
    }
    if (hpet_ok) {
        active_source_ = TickSource::HPET;
        calibrate_ok_ = true;
        return;
    }
    active_source_ = TickSource::PIT;
    calibrate_ok_ = false;
}

TickSource Timer::active_source() {
    return active_source_;
}

uint64_t Timer::freq_hz() {
    return tsc_freq_hz_;
}

bool Timer::calibrate() {
    if (!calibrated_) {
        calibrated_ = true;
        uint32_t ref_hz = last_freq_hz_;
        if (ref_hz == 0) {
            ref_hz = CONFIG_TICK_HZ;
        }
        calibrate_tsc(ref_hz);
        return calibrate_ok_;
    }
    // Frequency is stable; refresh the label only (APIC CPUID caps are
    // probed after Timer::init, so a TSC label may promote to
    // TSC_DEADLINE once the APIC is up).
    if (active_source_ == TickSource::TSC && APIC::has_tsc_deadline()) {
        active_source_ = TickSource::TSC_DEADLINE;
    }
    return calibrate_ok_;
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

uint64_t Timer::ns_monotonic() {
    uint64_t sample = 0;
    if (active_source_ == TickSource::HPET) {
        const uint64_t hpet_freq = hpet::freq_hz();
        if (hpet_freq != 0) {
            const uint64_t hpet_cnt = hpet::counter();
            const uint64_t sec = hpet_cnt / hpet_freq;
            const uint64_t rem = hpet_cnt % hpet_freq;
            sample = sec * 1000000000ULL + (rem * 1000000000ULL) / hpet_freq;
        }
    } else {
        sample = ns();
    }
    return detail::monotonic_commit(&last_ns_, sample);
}

void Timer::oneshot(uint64_t ticks_from_now) {
    const uint64_t boot_hz = kernel::BootParams::instance().timer_hz;
    if (boot_hz == 0 || ticks_from_now == 0) {
        __atomic_store_n(&deadline_tsc_, 0ULL, __ATOMIC_RELEASE);
        return;
    }
    const uint64_t delay_ns = ticks_to_ns(ticks_from_now, boot_hz);
    if (arch::APIC::is_enabled() && arch::APIC::is_timer_active()) {
        arch::APIC::set_timer_oneshot(delay_ns);
    }
    // PIT-only configs keep the system tick: only the software deadline
    // is armed (the #17 wheel owns IRQ-backed bounded waits).
    const uint64_t freq = tsc_freq_hz();
    if (freq == 0) {
        __atomic_store_n(&deadline_tsc_, 0ULL, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&deadline_tsc_, rdtsc() + hrt_ns_to_tsc(delay_ns, freq),
                     __ATOMIC_RELEASE);
}

void Timer::periodic(uint64_t period_ticks) {
    const uint64_t boot_hz = kernel::BootParams::instance().timer_hz;
    if (boot_hz == 0 || period_ticks == 0) {
        __atomic_store_n(&deadline_tsc_, 0ULL, __ATOMIC_RELEASE);
        return;
    }
    const uint64_t period_ns = ticks_to_ns(period_ticks, boot_hz);
    if (arch::APIC::is_enabled() && arch::APIC::is_timer_active()) {
        arch::APIC::set_timer_periodic(period_ns);
    }
    const uint64_t freq = tsc_freq_hz();
    if (freq == 0) {
        __atomic_store_n(&deadline_tsc_, 0ULL, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&deadline_tsc_, rdtsc() + hrt_ns_to_tsc(period_ns, freq),
                     __ATOMIC_RELEASE);
}

uint64_t Timer::remaining() {
    const uint64_t rem_ns = remaining_ns();
    if (rem_ns == 0) {
        return 0;
    }
    const uint64_t boot_hz = kernel::BootParams::instance().timer_hz;
    if (boot_hz == 0) {
        return 0;
    }
    // Ceil to ticks so a live (nonzero ns) deadline never reads 0.
    const uint64_t whole = rem_ns / 1000000000ULL;
    const uint64_t part = rem_ns % 1000000000ULL;
    return whole * boot_hz + (part * boot_hz + 1000000000ULL - 1) / 1000000000ULL;
}

uint64_t Timer::remaining_ns() {
    const uint64_t deadline =
        __atomic_load_n(&deadline_tsc_, __ATOMIC_ACQUIRE);
    if (deadline == 0) {
        return 0;
    }
    const uint64_t freq = tsc_freq_hz();
    if (freq == 0) {
        return 0;
    }
    const uint64_t now = rdtsc();
    if (now >= deadline) {
        return 0;
    }
    const uint64_t delta = deadline - now;
    const uint64_t sec = delta / freq;
    const uint64_t rem = delta % freq;
    return sec * 1000000000ULL + (rem * 1000000000ULL) / freq;
}

/// @brief Override the tick counter (test support).
/// @param value Value to assign to the tick counter.
void Timer::set_ticks_for_test(uint64_t value) {
    __atomic_store_n(&ticks_, value, __ATOMIC_RELEASE);
}

} // namespace arch
