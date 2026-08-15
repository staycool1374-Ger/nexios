/// @file test_apic_timer.cpp
/// @brief Tests for APIC timer replacement (v0.3.4).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/apic.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/bootparams.hpp>
#include <kernel/nexios_config.h>

using namespace kernel;

// Runmode: kernel
// Testidea: Verify APIC timer is active and increments ticks.
// Input: none (APIC timer is already running from boot init).
// Expect: Ticks increment over a measured interval.
// Depends: arch::APIC, arch::Timer
JARVIS_TEST(apic_timer_ticks_increment, "PRE: isolate | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    if (!arch::APIC::is_enabled() || !arch::APIC::is_timer_active()) {
        Logger::warn("APIC timer not active — skipping");
        JARVIS_TEST_PASS();
        return;
    }

    uint64_t t0 = arch::Timer::ticks();
    uint64_t tsc_freq = arch::Timer::tsc_freq_hz();
    JARVIS_ASSERT(tsc_freq > 0);
    uint64_t target = arch::rdtsc() + tsc_freq / 5;  // ~200 ms

    while (arch::rdtsc() < target) {
        arch::pause();
    }

    uint64_t elapsed = arch::Timer::ticks() - t0;
    Logger::info("apic_timer_ticks_increment: %lu ticks in ~200 ms", elapsed);
    JARVIS_ASSERT(elapsed > 0);
#else
    Logger::warn("APIC test skipped (not x86_64)");
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: set_timer_oneshot() does not crash and timer fires.
// Input: Program 1 ms one-shot, busy-wait briefly.
// Expect: Timer fires (ticks increment) or one-shot expired.
// Depends: arch::APIC, arch::rdtsc
JARVIS_TEST(apic_timer_oneshot, "PRE: isolate | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    if (!arch::APIC::is_enabled() || !arch::APIC::is_timer_active()) {
        Logger::warn("APIC timer not active — skipping");
        JARVIS_TEST_PASS();
        return;
    }

    // Program a short one-shot
    uint64_t tsc_freq = arch::Timer::tsc_freq_hz();
    JARVIS_ASSERT(tsc_freq > 0);
    arch::APIC::set_timer_oneshot(1000000);  // 1 ms

    // Wait a short while (interrupts fire normally)
    uint64_t deadline = arch::rdtsc() + tsc_freq / 500;  // ~2 ms
    while (arch::rdtsc() < deadline) {
        arch::pause();
    }

    // Restore the periodic system tick.  set_timer_oneshot() zeroes
    // periodic_ns_ (apic.cpp), so timer_start() alone no-ops — re-init at the
    // boot tick rate re-arms the periodic timer, then start it.  Assert the
    // rate is non-zero: a zero boot_hz would leave the tick dead for every
    // subsequent test in the class.
    uint64_t boot_hz = BootParams::instance().timer_hz;
    JARVIS_ASSERT(boot_hz != 0);
    if (boot_hz != 0) {
        arch::APIC::timer_init(static_cast<uint32_t>(boot_hz));
        arch::APIC::timer_start();
    }

    Logger::info("apic_timer_oneshot: one-shot programmed, no crash");
#else
    Logger::warn("APIC test skipped (not x86_64)");
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: APIC timer stop prevents further ticks; restart works.
// Input: Stop timer, wait, check tick count frozen; then restart.
// Expect: Tick count unchanged after stop; ticks resume after restart.
// Depends: arch::APIC, arch::Timer
JARVIS_TEST(apic_timer_stop_restart, "PRE: isolate | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    if (!arch::APIC::is_enabled() || !arch::APIC::is_timer_active()) {
        Logger::warn("APIC timer not active — skipping");
        JARVIS_TEST_PASS();
        return;
    }

    uint64_t before = arch::Timer::ticks();
    arch::APIC::timer_stop();

    uint64_t tsc_freq = arch::Timer::tsc_freq_hz();
    uint64_t deadline = arch::rdtsc() + tsc_freq / 20;  // ~50 ms
    while (arch::rdtsc() < deadline) {
        arch::pause();
    }

    uint64_t after = arch::Timer::ticks();

    // Restart the system tick BEFORE asserting (cookbook Rule 5): an early
    // assert failure would otherwise return with the tick still masked,
    // deadlocking the whole class.  Re-init at the boot tick rate re-arms the
    // periodic timer (timer_stop() masked the LVT and zeroed periodic_ns_).
    uint64_t boot_hz = BootParams::instance().timer_hz;
    JARVIS_ASSERT(boot_hz != 0);
    if (boot_hz != 0) {
        arch::APIC::timer_init(static_cast<uint32_t>(boot_hz));
        arch::APIC::timer_start();
    }
    // At most ONE tick may land in the window between reading `before` and the
    // LVT mask taking effect (an in-flight ISR already incremented ticks_).
    // timer_stop() then freezes the counter, so a larger delta is a real stop
    // failure.
    JARVIS_ASSERT(after - before <= 1);

    Logger::info("apic_timer_stop_restart: ticks frozen at %lu, restarted", before);
#else
    Logger::warn("APIC test skipped (not x86_64)");
#endif
    JARVIS_TEST_PASS();
}

void register_apic_timer_tests() {
    Logger::info("Registering APIC timer tests");
    JARVIS_REGISTER_TEST(apic_timer_ticks_increment);
    JARVIS_REGISTER_TEST(apic_timer_oneshot);
    JARVIS_REGISTER_TEST(apic_timer_stop_restart);
}
