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

/// @file test_irq_early_std.cpp
/// @brief Early IRQ-controller + timer init conformance (issue #198).
///        Arch-neutral contract checks over arch::early_irq_init(),
///        IrqState snapshot/restore, and the Timer disarm/monotonic rules.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/early_init.hpp>
#include <kernel/arch/interrupt_controller.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/nexios_config.h>

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies early_irq_init() completes the ordered bring-up
// (IDT -> controller -> timer -> calibrate) with a known backing frequency.
// Input: early_irq_init(CONFIG_TICK_HZ)
// Expect: returns OK and Timer::freq_hz() != 0
// Depends: arch::early_irq_init, arch::Timer
JARVIS_TEST(early_init_returns_ok_and_freq_known, "PRE: iocd | POST: none") {
    const arch::EarlyIrqResult res =
        arch::early_irq_init(CONFIG_TICK_HZ);
    JARVIS_ASSERT_FMT(res == arch::EarlyIrqResult::OK,
                      "early_irq_init should return OK, got %u",
                      static_cast<unsigned>(res));
    JARVIS_ASSERT_FMT(arch::Timer::freq_hz() != 0,
                      "Timer backing frequency must be known after init");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies early_irq_init() is idempotent (BOOT_ONLY): a second
// call returns OK with no frequency change.
// Input: early_irq_init() twice
// Expect: both return OK, freq_hz() identical
// Depends: arch::early_irq_init
JARVIS_TEST(early_init_idempotent, "PRE: iocd | POST: none") {
    const arch::EarlyIrqResult first =
        arch::early_irq_init(CONFIG_TICK_HZ);
    const uint64_t freq_first = arch::Timer::freq_hz();
    const arch::EarlyIrqResult second =
        arch::early_irq_init(CONFIG_TICK_HZ);
    JARVIS_ASSERT(first == arch::EarlyIrqResult::OK);
    JARVIS_ASSERT(second == arch::EarlyIrqResult::OK);
    JARVIS_ASSERT_FMT(arch::Timer::freq_hz() == freq_first,
                      "second early_irq_init must not change frequency");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies early_irq_init(0) fails closed with BAD_FREQ.
// Input: early_irq_init(0)
// Expect: returns BAD_FREQ (never panic)
// Depends: arch::early_irq_init
JARVIS_TEST(early_init_rejects_zero_freq, "PRE: iocd | POST: none") {
    const arch::EarlyIrqResult res = arch::early_irq_init(0);
    JARVIS_ASSERT_FMT(res == arch::EarlyIrqResult::BAD_FREQ,
                      "early_irq_init(0) should return BAD_FREQ, got %u",
                      static_cast<unsigned>(res));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies IrqState snapshot/restore round-trips the controller
// mask state: mask one line, restore, state equals the original.
// Input: snapshot/mask/restore/snapshot
// Expect: restored snapshot equals the pre-mask snapshot
// Depends: arch::ArchInterruptController
JARVIS_TEST(irq_snapshot_restore_roundtrip, "PRE: iocd | POST: none") {
    arch::IrqGuard guard;
    const arch::IrqState before = arch::ArchInterruptController::snapshot();
    arch::ArchInterruptController::mask(1);
    arch::ArchInterruptController::restore(before);
    const arch::IrqState after = arch::ArchInterruptController::snapshot();
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_ASSERT_FMT(after.pic1_mask == before.pic1_mask,
                      "PIC1 mask not restored");
    JARVIS_ASSERT_FMT(after.pic2_mask == before.pic2_mask,
                      "PIC2 mask not restored");
#elif defined(CONFIG_ARCH_AARCH64)
    JARVIS_ASSERT_FMT(after.gic_mask == before.gic_mask,
                      "GIC mask not restored");
#elif defined(CONFIG_ARCH_RISCV64)
    JARVIS_ASSERT_FMT(after.plic_threshold == before.plic_threshold,
                      "PLIC threshold not restored");
    JARVIS_ASSERT_FMT(after.plic_enable_first == before.plic_enable_first,
                      "PLIC enable word not restored");
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies oneshot(0)/periodic(0) disarm the timer on every arch
// (0 = disarm per Timer contract).
// Input: oneshot(0), periodic(0)
// Expect: remaining() == 0 and remaining_ns() == 0 after each
// Depends: arch::Timer::oneshot/periodic/remaining/remaining_ns
JARVIS_TEST(timer_oneshot_periodic_disarm, "PRE: iocd | POST: none") {
    arch::Timer::oneshot(0);
    JARVIS_ASSERT_FMT(arch::Timer::remaining() == 0,
                      "oneshot(0) must disarm");
    JARVIS_ASSERT_FMT(arch::Timer::remaining_ns() == 0,
                      "oneshot(0) must clear sub-tick remainder");
    arch::Timer::periodic(0);
    JARVIS_ASSERT_FMT(arch::Timer::remaining() == 0,
                      "periodic(0) must disarm");
    JARVIS_ASSERT_FMT(arch::Timer::remaining_ns() == 0,
                      "periodic(0) must clear sub-tick remainder");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies ns_monotonic() never decreases across samples.
// Input: two ns_monotonic() samples
// Expect: second >= first
// Depends: arch::Timer::ns_monotonic
JARVIS_TEST(timer_monotonic_non_decreasing, "PRE: iocd | POST: none") {
    const uint64_t first = arch::Timer::ns_monotonic();
    const uint64_t second = arch::Timer::ns_monotonic();
    JARVIS_ASSERT_FMT(second >= first, "ns_monotonic decreased");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies handle_irq() increments the tick count by exactly one
// with no scheduler dependency (early bring-up rule INV-5).
// Input: set_ticks_for_test(0), handle_irq(0)
// Expect: ticks() == 1
// Depends: arch::Timer::handle_irq
JARVIS_TEST(timer_handle_irq_increments_ticks, "PRE: iocd | POST: none") {
    arch::IrqGuard guard;
    arch::Timer::set_ticks_for_test(0);
    arch::Timer::handle_irq(0);
    JARVIS_ASSERT_FMT(arch::Timer::ticks() == 1,
                      "handle_irq must increment ticks by 1, got %lu",
                      arch::Timer::ticks());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all early-IRQ-standardization conformance tests.
// Input: None
// Expect: All irq_early_std tests registered via JARVIS_REGISTER_TEST
// Depends: kernel test framework
void register_irq_early_std_tests() {
    Logger::info("Registering irq_early_std tests");
    JARVIS_REGISTER_TEST(early_init_returns_ok_and_freq_known);
    JARVIS_REGISTER_TEST(early_init_idempotent);
    JARVIS_REGISTER_TEST(early_init_rejects_zero_freq);
    JARVIS_REGISTER_TEST(irq_snapshot_restore_roundtrip);
    JARVIS_REGISTER_TEST(timer_oneshot_periodic_disarm);
    JARVIS_REGISTER_TEST(timer_monotonic_non_decreasing);
    JARVIS_REGISTER_TEST(timer_handle_irq_increments_ticks);
}
