/*
 * NexIOS RTOS — SMP bring-up (Phase 5)
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

/// @file test_lapic.cpp
/// @brief Local APIC tests (issue #85, module 2): enablement contract,
///        LAPIC-ID stability, one-shot/periodic boundaries, bounded
///        one-shot delivery, EOI safety.  Only the public APIC surface is
///        exercised (version/MMIO-base/x2APIC-mode registers have no
///        accessors — documented gaps, need main-branch API).

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/apic.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/bootparams.hpp>
#include <kernel/nexios_config.h>

using namespace kernel;

namespace {

// Re-arm the periodic system tick at the boot rate.  One-shot tests
// disarm re-arm (periodic_ns_ = 0), so every one-shot/zero-boundary
// test must call this BEFORE asserting (cookbook Rule 5): an early
// assert failure must never leave the tick dead for later tests.
void restore_periodic_tick() {
    uint64_t boot_hz = BootParams::instance().timer_hz;
    if (boot_hz != 0) {
        arch::APIC::timer_init(static_cast<uint32_t>(boot_hz));
        arch::APIC::timer_start();
    }
}

// Busy-wait until Timer::ticks() exceeds tick_base or the TSC
// deadline passes.  Returns true when a tick landed in the window.
bool wait_for_tick(uint64_t tick_base, uint64_t timeout_ns) {
    uint64_t tsc_freq = arch::Timer::tsc_freq_hz();
    if (tsc_freq == 0) {
        return false;
    }
    uint64_t deadline =
        arch::rdtsc() + (tsc_freq * timeout_ns) / 1000000000ULL;
    while (arch::rdtsc() < deadline) {
        if (arch::Timer::ticks() > tick_base) {
            return true;
        }
        arch::pause();
    }
    return arch::Timer::ticks() > tick_base;
}

}  // namespace

// Runmode: kernel
// Testidea: The boot path leaves a live local APIC behind: the CPU
//           reports APIC support, the driver is enabled, and the system
//           tick runs on the APIC timer (not the legacy PIT).
// Input: Capability/enablement queries on the running machine.
// Expect: is_apic_supported() && is_enabled() && is_timer_active().
// Depends: arch::APIC boot init (APIC::init + Timer::init)
JARVIS_TEST(lapic_supported_and_enabled, "PRE: none | POST: none") {
    JARVIS_ASSERT(arch::APIC::is_apic_supported());
    JARVIS_ASSERT(arch::APIC::is_enabled());
    JARVIS_ASSERT(arch::APIC::is_timer_active());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The LAPIC ID register is stable firmware state: repeated
//           reads return the same ID (no read-side effects, no aliasing
//           with the timer countdown register next door).
// Input: Three consecutive APIC::lapic_id() reads.
// Expect: All three readings identical.  (MADT cross-check of the value
//         itself lives in smp_madt_scan_finds_bsp.)
// Depends: arch::APIC::lapic_id
JARVIS_TEST(lapic_id_stable, "PRE: none | POST: none") {
    uint32_t first = arch::APIC::lapic_id();
    uint32_t second = arch::APIC::lapic_id();
    uint32_t third = arch::APIC::lapic_id();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(first),
                     static_cast<uint64_t>(second));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(second),
                     static_cast<uint64_t>(third));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A zero-nanosecond one-shot is a documented no-op: it must
//           neither crash nor disturb the running periodic tick.
// Input: set_timer_oneshot(0), then observe the tick for ~20 ms.
// Expect: Ticks keep advancing (periodic mode untouched).
// Depends: arch::APIC::set_timer_oneshot, arch::Timer::ticks
JARVIS_TEST(lapic_oneshot_zero_noop, "PRE: isolate | POST: none") {
    uint64_t tick_base = arch::Timer::ticks();
    arch::APIC::set_timer_oneshot(0);
    bool ticked = wait_for_tick(tick_base, 20000000ULL);
    JARVIS_ASSERT(ticked);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A programmed one-shot fires exactly through the live tick
//           path: the deadline ISR runs at APIC_TIMER_VECTOR and
//           increments the tick counter within a bounded window.
// Input: set_timer_oneshot(2 ms); poll for a tick up to 100 ms.
// Expect: A tick lands in the window; periodic mode restored first.
// Depends: arch::APIC::set_timer_oneshot, tick ISR, Timer::ticks
JARVIS_TEST(lapic_oneshot_fires_bounded, "PRE: isolate | POST: none") {
    uint64_t tick_base = arch::Timer::ticks();
    arch::APIC::set_timer_oneshot(2000000ULL);
    bool fired = wait_for_tick(tick_base, 100000000ULL);
    restore_periodic_tick();
    JARVIS_ASSERT(fired);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Pin the real zero-period behaviour: set_timer_periodic(0)
//           early-returns (no-op) — the running tick is NOT stopped.
//           The header comment claims "0 = stop timer"; the code keeps
//           the current mode.  This test locks the code reality so a
//           future "fix" toward the comment cannot silently kill the
//           tick for every caller passing 0.
// Input: set_timer_periodic(0), then observe the tick for ~20 ms.
// Expect: Ticks keep advancing (mode untouched, still periodic).
// Depends: arch::APIC::set_timer_periodic, arch::Timer::ticks
JARVIS_TEST(lapic_periodic_zero_noop, "PRE: isolate | POST: none") {
    uint64_t tick_base = arch::Timer::ticks();
    arch::APIC::set_timer_periodic(0);
    bool ticked = wait_for_tick(tick_base, 20000000ULL);
    JARVIS_ASSERT(ticked);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A stray EOI with no in-service interrupt is harmless: the
//           write completes and the tick keeps running afterwards.
// Input: APIC::eoi() from task context, then observe ~20 ms of ticks.
// Expect: No fault; ticks keep advancing after the EOI.
// Depends: arch::APIC::eoi, arch::Timer::ticks
JARVIS_TEST(lapic_eoi_safe, "PRE: isolate | POST: none") {
    arch::APIC::eoi();
    uint64_t tick_base = arch::Timer::ticks();
    bool ticked = wait_for_tick(tick_base, 20000000ULL);
    JARVIS_ASSERT(ticked);
    JARVIS_TEST_PASS();
}

void register_lapic_tests() {
    Logger::info("Registering lapic tests");
    JARVIS_REGISTER_TEST(lapic_supported_and_enabled);
    JARVIS_REGISTER_TEST(lapic_id_stable);
    JARVIS_REGISTER_TEST(lapic_oneshot_zero_noop);
    JARVIS_REGISTER_TEST(lapic_oneshot_fires_bounded);
    JARVIS_REGISTER_TEST(lapic_periodic_zero_noop);
    JARVIS_REGISTER_TEST(lapic_eoi_safe);
}
#endif  // CONFIG_ARCH_X86_64
