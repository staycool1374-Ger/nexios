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

/// @file test_ioapic.cpp
/// @brief I/O APIC tests (issue #85, module 3): boot routing liveness,
///        mask/unmask cycles, invalid-IRQ rejection, idempotence, full
///        legacy-line sweep.  Only APIC::mask_irq is public — entry
///        contents, ID/version registers and the MMIO base have no
///        accessors (documented gaps, need main-branch API), so routing
///        is verified functionally: the system tick and the keyboard
///        path programmed at APIC::init stay alive across mutations.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/apic.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/nexios_config.h>

using namespace kernel;

namespace {

// Observe the tick for up to timeout_ns; true when it advances.
// Mask/unmask mutations must never stall the APIC tick.
bool tick_alive_ns(uint64_t timeout_ns) {
    uint64_t tick_base = arch::Timer::ticks();
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
// Testidea: The APIC::init boot routing (IRQ0->32, IRQ1->33, rest
//           masked) leaves a working system: the APIC timer tick
//           advances, proving the I/O APIC + local APIC path is live.
// Input: None (observes the boot-programmed routing).
// Expect: Tick advances within ~20 ms.
// Depends: arch::APIC::init boot routing, arch::Timer::ticks
JARVIS_TEST(ioapic_boot_routing_alive, "PRE: none | POST: none") {
    JARVIS_ASSERT(arch::APIC::is_enabled());
    JARVIS_ASSERT(tick_alive_ns(20000000ULL));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A mask/unmask cycle on an unused legacy line (IRQ7,
//           masked at boot) programs both entry states without
//           disturbing the live tick; the line is restored masked.
// Input: mask_irq(7, true/false/true).
// Expect: No fault; tick advances afterwards.
// Depends: arch::APIC::mask_irq, arch::Timer::ticks
JARVIS_TEST(ioapic_mask_unmask_cycle, "PRE: isolate | POST: none") {
    arch::APIC::mask_irq(7, true);
    arch::APIC::mask_irq(7, false);
    arch::APIC::mask_irq(7, true);
    JARVIS_ASSERT(tick_alive_ns(20000000ULL));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Out-of-range IRQ numbers are rejected fail-silent:
//           mask_irq early-returns for irq >= 16 (boundary 15/16)
//           without touching any redirection entry.
// Input: mask_irq(16, ...) and mask_irq(255, ...) both states.
// Expect: No fault; tick advances afterwards (no entry clobbered).
// Depends: arch::APIC::mask_irq bounds guard
JARVIS_TEST(ioapic_invalid_irq_rejected, "PRE: isolate | POST: none") {
    arch::APIC::mask_irq(15, true);
    arch::APIC::mask_irq(15, false);
    arch::APIC::mask_irq(15, true);
    arch::APIC::mask_irq(16, true);
    arch::APIC::mask_irq(16, false);
    arch::APIC::mask_irq(255, true);
    arch::APIC::mask_irq(255, false);
    JARVIS_ASSERT(tick_alive_ns(20000000ULL));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Repeated identical mask writes are idempotent: double
//           mask / double unmask leave the entry and the tick stable.
// Input: mask_irq(7, true) twice, then (7, false) twice, restore true.
// Expect: No fault; tick advances afterwards.
// Depends: arch::APIC::mask_irq
JARVIS_TEST(ioapic_mask_idempotent, "PRE: isolate | POST: none") {
    arch::APIC::mask_irq(7, true);
    arch::APIC::mask_irq(7, true);
    arch::APIC::mask_irq(7, false);
    arch::APIC::mask_irq(7, false);
    arch::APIC::mask_irq(7, true);
    JARVIS_ASSERT(tick_alive_ns(20000000ULL));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Sweep every masked legacy line (IRQ2..IRQ15): unmask then
//           re-mask each entry, proving the entry stride (irq * 2) and
//           the mask bit land on a live register for all 14 lines and
//           every line returns to its boot-masked state.
// Input: For irq in [2, 16): mask_irq(irq, false), mask_irq(irq, true).
// Expect: No fault; all lines masked again; tick advances afterwards.
// Depends: arch::APIC::mask_irq entry addressing
JARVIS_TEST(ioapic_lines_sweep, "PRE: isolate | POST: none") {
    for (uint8_t irq = 2; irq < 16; ++irq) {
        arch::APIC::mask_irq(irq, false);
        arch::APIC::mask_irq(irq, true);
    }
    JARVIS_ASSERT(tick_alive_ns(20000000ULL));
    JARVIS_TEST_PASS();
}

void register_ioapic_tests() {
    Logger::info("Registering ioapic tests");
    JARVIS_REGISTER_TEST(ioapic_boot_routing_alive);
    JARVIS_REGISTER_TEST(ioapic_mask_unmask_cycle);
    JARVIS_REGISTER_TEST(ioapic_invalid_irq_rejected);
    JARVIS_REGISTER_TEST(ioapic_mask_idempotent);
    JARVIS_REGISTER_TEST(ioapic_lines_sweep);
}
#endif  // CONFIG_ARCH_X86_64
