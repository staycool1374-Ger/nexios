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

/// @file test_tick_watchdog.cpp
/// @brief M2 (issue #546 §6.2 item 6) QE: TSC-domain tick watchdog tests
///        (core_sched).  Fire/quiet/one-shot/override-seam legs driven via
///        the test-active stand-down + IrqGuard-frozen ticks.  No production
///        code touched.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/tick_watchdog.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/hal/timer.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

using namespace kernel;

namespace {

// Grace in raw-counter cycles, mirroring try_enable() (grace ticks at
// CONFIG_TICK_HZ converted with the calibrated counter frequency).
uint64_t grace_cycles() {
    const uint64_t freq = arch::Timer::raw_counter_freq_hz();
    if (freq == 0 || CONFIG_TICK_HZ == 0)
        return 0;
    uint64_t grace = static_cast<uint64_t>(CONFIG_TICK_WATCHDOG_GRACE_TICKS);
    if (grace > ~0ULL / freq)
        grace = ~0ULL / freq;
    grace *= freq;
    grace /= static_cast<uint64_t>(CONFIG_TICK_HZ);
    return grace == 0 ? 1 : grace;
}

// Wall-spin until the raw counter advances past @p target cycles (IF=0
// caller freezes ticks meanwhile).  Fail-stop: false after 15 s wall.
bool spin_past(uint64_t base, uint64_t target) {
    const uint64_t freq = arch::Timer::raw_counter_freq_hz();
    const uint64_t limit = (freq == 0 ? 1000000000ULL : 15ULL * freq);
    for (;;) {
        arch::pause();
        const uint64_t now = arch::Timer::raw_counter();
        const uint64_t elapsed = now >= base ? now - base : 0;
        if (elapsed >= target)
            return true;
        if (elapsed >= limit)
            return false;
    }
}

// Stand-down wrapper: every leg restores test-active before returning so
// a drive failure can never strand the harness clock-ownership gate.
bool drive_quiet_before_first_tick() {
    tick_watchdog::reset_for_test();
    Logger::info("[TICKWD-TEST] entry depth=%lu",
                 arch::irq_guard_depth_own());
    Scheduler::set_test_active(false);
    tick_watchdog::checkpoint("test_quiet_first");
    const bool quiet =
        !tick_watchdog::fired() && tick_watchdog::fire_count() == 0;
    Scheduler::set_test_active(true);
    tick_watchdog::reset_for_test();
    return quiet;
}

bool drive_quiet_on_advance() {
    tick_watchdog::reset_for_test();
    tick_watchdog::note_tick();
    Scheduler::set_test_active(false);
    tick_watchdog::checkpoint("test_quiet_advance");
    const bool quiet =
        !tick_watchdog::fired() && tick_watchdog::fire_count() == 0;
    Scheduler::set_test_active(true);
    tick_watchdog::reset_for_test();
    return quiet;
}

bool drive_quiet_under_test_active() {
    tick_watchdog::reset_for_test();
    const uint64_t grace = grace_cycles();
    if (grace == 0)
        return false; // No calibrated counter: watchdog dead by design.
    bool quiet = false;
    {
        arch::IrqGuard irq_guard{}; // Freeze ticks; raw TSC keeps advancing.
        tick_watchdog::note_tick();
        const uint64_t base = arch::Timer::raw_counter();
        if (spin_past(base, 2 * grace + 1)) {
            // Test-active HELD: the harness owns the clock, no fire.
            tick_watchdog::checkpoint("test_quiet_active");
            quiet = !tick_watchdog::fired() &&
                    tick_watchdog::fire_count() == 0;
        }
    }
    tick_watchdog::note_tick_override(); // Resync post-freeze snapshot.
    tick_watchdog::reset_for_test();
    return quiet;
}

bool drive_fires_on_stall() {
    tick_watchdog::reset_for_test();
    const uint64_t grace = grace_cycles();
    if (grace == 0)
        return false;
    bool fired_once = false;
    Scheduler::set_test_active(false);
    {
        arch::IrqGuard irq_guard{};
        tick_watchdog::note_tick();
        const uint64_t base = arch::Timer::raw_counter();
        if (spin_past(base, 2 * grace + 1)) {
            tick_watchdog::checkpoint("test_stall");
            fired_once = tick_watchdog::fired() &&
                         tick_watchdog::fire_count() == 1 &&
                         tick_watchdog::last_evidence() > 0;
            // One-shot latch: a second checkpoint must not re-fire.
            tick_watchdog::checkpoint("test_stall_again");
            fired_once =
                fired_once && tick_watchdog::fire_count() == 1;
        }
    }
    Scheduler::set_test_active(true);
    tick_watchdog::reset_for_test(); // Unlatch for later tests.
    return fired_once;
}

// Frozen-tick setup: note the live tick, then rewind to exactly the noted
// value (retry-bounded: a tick advancing between the two reads breaks the
// exact match and the attempt is retried).  Caller holds IrqGuard (ticks
// frozen) with test-active stood down.  Returns the pre-spin raw counter,
// or 0 if the exact match never lined up.
uint64_t setup_rewound(bool apply_override) {
    for (uint64_t attempt = 0; attempt < 10; ++attempt) {
        const uint64_t t0 = arch::Timer::ticks();
        tick_watchdog::note_tick();
        if (arch::Timer::ticks() != t0)
            continue;
        arch::Timer::set_ticks_for_test(t0);
        if (apply_override)
            tick_watchdog::note_tick_override();
        return arch::Timer::raw_counter();
    }
    return 0;
}

// Leg (a) without override must fire (the pre-rewind elapsed is retained);
// leg (b) with override must stay quiet (the override forgives it).
bool drive_override_resync() {
    const uint64_t grace = grace_cycles();
    if (grace == 0)
        return false;
    const uint64_t half = grace * 6 / 10;
    bool leg_a_fired = false;
    bool leg_b_quiet = false;
    Scheduler::set_test_active(false);
    {
        arch::IrqGuard irq_guard{};
        tick_watchdog::reset_for_test();
        // Leg (a): note, stall past half grace, rewind, stall again.
        uint64_t base = setup_rewound(false);
        if (base != 0 && spin_past(base, half)) {
            base = arch::Timer::raw_counter();
            if (spin_past(base, half)) {
                tick_watchdog::checkpoint("test_override_plain");
                leg_a_fired = tick_watchdog::fired() &&
                              tick_watchdog::fire_count() == 1;
            }
        }
        tick_watchdog::reset_for_test();
        // Leg (b): same, but the override resyncs at the rewind point.
        base = setup_rewound(true);
        if (base != 0 && spin_past(base, half)) {
            tick_watchdog::checkpoint("test_override_seam");
            leg_b_quiet = !tick_watchdog::fired() &&
                          tick_watchdog::fire_count() == 0;
        }
    }
    Scheduler::set_test_active(true);
    tick_watchdog::note_tick_override();
    tick_watchdog::reset_for_test();
    return leg_a_fired && leg_b_quiet;
}

} // namespace

// Runmode: kernel
// Testidea: No fire before the first tick is observed.
// Input: reset_for_test(), then checkpoint with test-active stood down.
// Expect: fired()==false, fire_count()==0 (g_last_counter==0 path).
// Depends: tick_watchdog reset/checkpoint/fire_count, set_test_active
JARVIS_TEST(tick_watchdog_quiet_before_first_tick, "PRE: none | POST: none") {
    JARVIS_ASSERT(drive_quiet_before_first_tick());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A fresh tick snapshot never fires (elapsed < grace).
// Input: reset, note_tick, immediate checkpoint with test-active down.
// Expect: No fire (both tick-advanced and elapsed~0 paths are quiet).
// Depends: note_tick + checkpoint
JARVIS_TEST(tick_watchdog_quiet_on_advance, "PRE: none | POST: none") {
    JARVIS_ASSERT(drive_quiet_on_advance());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The harness clock-ownership stand-down holds (no fire while
// the test harness owns the clock, even across a grace-length stall).
// Input: reset, note_tick, IrqGuard-frozen ticks + wall spin past grace,
//        checkpoint with test-active HELD.
// Expect: No fire (is_test_active path).
// Depends: checkpoint test-active gate
JARVIS_TEST(tick_watchdog_quiet_under_test_active, "PRE: none | POST: none") {
    JARVIS_ASSERT(drive_quiet_under_test_active());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A frozen tick with advancing TSC fires exactly once.
// Input: reset, note_tick, IrqGuard-frozen ticks + wall spin past grace,
//        checkpoint with test-active stood down, checkpoint again.
// Expect: fired(), fire_count()==1, last_evidence()>0; second checkpoint
//         does not re-fire (one-shot latch).
// Depends: checkpoint fire path, fired/fire_count/last_evidence
JARVIS_TEST(tick_watchdog_fires_on_stall, "PRE: none | POST: none") {
    JARVIS_ASSERT(drive_fires_on_stall());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: note_tick_override() resyncs a deliberate tick rewind so it
// is not misread as a stall (test seam for set_ticks_for_test users).
// Input: Frozen-tick setup rewound to the noted value; leg (a) without
//        override fires; leg (b) with override stays quiet.
// Expect: (a) fires (seam necessity proven); (b) quiet after override.
// Depends: note_tick_override, set_ticks_for_test
JARVIS_TEST(tick_watchdog_override_resync, "PRE: none | POST: none") {
    JARVIS_ASSERT(drive_override_resync());
    JARVIS_TEST_PASS();
}

void register_tick_watchdog_tests() {
    JARVIS_REGISTER_TEST(tick_watchdog_quiet_before_first_tick);
    JARVIS_REGISTER_TEST(tick_watchdog_quiet_on_advance);
    JARVIS_REGISTER_TEST(tick_watchdog_quiet_under_test_active);
    JARVIS_REGISTER_TEST(tick_watchdog_fires_on_stall);
    JARVIS_REGISTER_TEST(tick_watchdog_override_resync);
}
