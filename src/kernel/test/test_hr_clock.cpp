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

/// @file test_hr_clock.cpp
/// @brief High-resolution monotonic clock tests (issue #16, v0.4.7).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/nexios_config.h>
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/apic.hpp>
#include <kernel/bootparams.hpp>
#endif

using namespace kernel;

// Runmode: kernel
// Testidea: calibrate() leaves a nonzero unified frequency (fail-closed).
// Input: Call calibrate() twice (idempotency), read freq_hz()
// Expect: freq_hz() != 0 and stable across both calls
// Depends: arch::Timer::calibrate, arch::Timer::freq_hz
JARVIS_TEST(hrt_freq_nonzero_after_calibrate, "PRE: none | POST: none") {
    const bool first_ok = arch::Timer::calibrate();
    const uint64_t freq_first = arch::Timer::freq_hz();
    JARVIS_ASSERT(freq_first != 0);
    JARVIS_ASSERT(arch::Timer::tsc_freq_hz() != 0);
    const bool second_ok = arch::Timer::calibrate();
    JARVIS_ASSERT(first_ok == second_ok);
    JARVIS_ASSERT_EQ(freq_first, arch::Timer::freq_hz());
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_ASSERT_EQ(freq_first, arch::Timer::tsc_freq_hz());
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: ns_monotonic() never decreases across successive reads.
// Input: Read ns_monotonic() and ns() several times
// Expect: Each sample >= previous sample
// Depends: arch::Timer::ns_monotonic, arch::Timer::ns
JARVIS_TEST(hrt_ns_monotonic_non_decrease, "PRE: none | POST: none") {
    uint64_t prev_mono = arch::Timer::ns_monotonic();
    for (int sample_idx = 0; sample_idx < 4; ++sample_idx) {
        const uint64_t cur_mono = arch::Timer::ns_monotonic();
        JARVIS_ASSERT(cur_mono >= prev_mono);
        prev_mono = cur_mono;
    }
    const uint64_t ns_first = arch::Timer::ns();
    const uint64_t ns_second = arch::Timer::ns();
    JARVIS_ASSERT(ns_second >= ns_first);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: active_source() reports a configured, valid tick source.
// Input: Read active_source() after calibrate()
// Expect: Source matches build config (never HPET when disabled; x86_64
//         TSC_DEADLINE/TSC/PIT; aarch64 GENERIC_COUNTER; riscv64 MTIME)
// Depends: arch::Timer::active_source, CONFIG_HAS_HPET
JARVIS_TEST(hrt_active_source_valid, "PRE: none | POST: none") {
    (void)arch::Timer::calibrate();
    const arch::TickSource src = arch::Timer::active_source();
#if defined(CONFIG_ARCH_X86_64)
#if CONFIG_HAS_HPET
    JARVIS_ASSERT(src == arch::TickSource::HPET ||
                  src == arch::TickSource::TSC ||
                  src == arch::TickSource::TSC_DEADLINE ||
                  src == arch::TickSource::PIT);
#else
    JARVIS_ASSERT(src != arch::TickSource::HPET);
    if (arch::APIC::has_tsc_deadline()) {
        JARVIS_ASSERT(src == arch::TickSource::TSC_DEADLINE);
    } else {
        JARVIS_ASSERT(src == arch::TickSource::TSC ||
                      src == arch::TickSource::PIT);
    }
#endif
#elif defined(CONFIG_ARCH_AARCH64)
    JARVIS_ASSERT(src == arch::TickSource::GENERIC_COUNTER);
#elif defined(CONFIG_ARCH_RISCV64)
    JARVIS_ASSERT(src == arch::TickSource::MTIME);
#else
    JARVIS_ASSERT(src != arch::TickSource::TEST);
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: set_ticks_for_test() round-trips without disturbing ns_monotonic().
// Input: Override ticks, read back; sample ns_monotonic() across override
// Expect: ticks() returns test values; ns_monotonic() still non-decreasing
// Depends: arch::Timer::set_ticks_for_test, arch::Timer::ticks
JARVIS_TEST(hrt_test_ticks_determinism, "PRE: none | POST: none") {
    const uint64_t mono_before = arch::Timer::ns_monotonic();
    arch::Timer::set_ticks_for_test(0x1234ABCDULL);
    JARVIS_ASSERT_EQ((uint64_t)0x1234ABCDULL, arch::Timer::ticks());
    arch::Timer::set_ticks_for_test(0x0ULL);
    JARVIS_ASSERT_EQ((uint64_t)0x0ULL, arch::Timer::ticks());
    const uint64_t mono_after = arch::Timer::ns_monotonic();
    JARVIS_ASSERT(mono_after >= mono_before);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: oneshot() arms a sub-tick deadline visible via remaining_ns().
// Input: Arm oneshot(N ticks), read remaining()/remaining_ns() immediately
// Expect: 0 < remaining() <= N, 0 < remaining_ns() <= N * ns_per_tick;
//         system tick restored afterwards (no tick freeze for later tests)
// Depends: arch::Timer::oneshot/periodic/remaining/remaining_ns,
//         arch::APIC::timer_init/timer_start, BootParams::timer_hz
JARVIS_TEST(hrt_oneshot_remaining_ns, "PRE: none | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    if (!arch::APIC::is_enabled() || !arch::APIC::is_timer_active()) {
        Logger::warn("HRT oneshot skipped (APIC timer inactive)");
        JARVIS_TEST_PASS();
        return;
    }
    const uint64_t boot_hz = BootParams::instance().timer_hz;
    JARVIS_ASSERT(boot_hz != 0);
    constexpr uint64_t k_arm_ticks = 50;
    arch::Timer::oneshot(k_arm_ticks);
    const uint64_t rem_ticks = arch::Timer::remaining();
    JARVIS_ASSERT(rem_ticks > 0);
    JARVIS_ASSERT(rem_ticks <= k_arm_ticks);
    const uint64_t rem_ns = arch::Timer::remaining_ns();
    const uint64_t tick_ns = 1000000000ULL / boot_hz;
    JARVIS_ASSERT(rem_ns > 0);
    JARVIS_ASSERT(rem_ns <= k_arm_ticks * tick_ns);
    // Restore the periodic system tick BEFORE further asserts (cookbook
    // Rule 5): oneshot() disarms re-arm, so the tick would otherwise die
    // here for every later test in the class.
    arch::APIC::timer_init(static_cast<uint32_t>(boot_hz));
    arch::APIC::timer_start();
    // periodic() smoke at the running rate: re-arms the same 1-tick period
    // the tick already uses, so the tick keeps running unchanged.
    arch::Timer::periodic(1);
    const uint64_t per_ns = arch::Timer::remaining_ns();
    JARVIS_ASSERT(per_ns <= tick_ns);
    JARVIS_ASSERT(arch::Timer::remaining() <= 1);
#elif defined(CONFIG_ARCH_AARCH64)
    constexpr uint64_t k_arm_ms = 50;
    constexpr uint64_t k_ns_slack = 55000000ULL;
    arch::Timer::oneshot(k_arm_ms);
    JARVIS_ASSERT(arch::Timer::remaining() > 0);
    const uint64_t rem_ns = arch::Timer::remaining_ns();
    JARVIS_ASSERT(rem_ns > 0);
    JARVIS_ASSERT(rem_ns <= k_ns_slack);
#elif defined(CONFIG_ARCH_RISCV64)
    // mtimecmp is unreadable from S-mode: specified behavior is 0/0.
    JARVIS_ASSERT_EQ((uint64_t)0, arch::Timer::remaining());
    JARVIS_ASSERT_EQ((uint64_t)0, arch::Timer::remaining_ns());
#else
    JARVIS_ASSERT_EQ((uint64_t)0, arch::Timer::remaining_ns());
#endif
    JARVIS_TEST_PASS();
}

void register_hrt_monotonic_tests() {
    Logger::info("Registering hrt_monotonic tests");
    JARVIS_REGISTER_TEST(hrt_freq_nonzero_after_calibrate);
    JARVIS_REGISTER_TEST(hrt_ns_monotonic_non_decrease);
    JARVIS_REGISTER_TEST(hrt_active_source_valid);
    JARVIS_REGISTER_TEST(hrt_test_ticks_determinism);
    JARVIS_REGISTER_TEST(hrt_oneshot_remaining_ns);
}
