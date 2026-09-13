/*
 * NexIOS RTOS — SMP bring-up (Phase B2)
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

/// @file test_smp_ipi.cpp
/// @brief APIC IPI tests (issue #25, Phase B2).
///        send_ipi() is exercised without waking an AP: the INIT/SIPI
///        sequence targets an absent LAPIC (ICR acceptance needs no
///        recipient — delivery-status clears once the local APIC sends),
///        and FIXED delivery is proven by a self-IPI to the BSP on a
///        dedicated test vector.  The self-IPI handler stays installed
///        after the test (benign flag-setter + EOI, never fires again
///        unless re-triggered).

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/apic.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>

using namespace kernel;

namespace {

// Dedicated self-IPI test vector (no kernel/timer/keyboard/syscall use).
constexpr uint8_t kSelfIpiVector = 0xEF;

// Set by the self-IPI handler; polled by the delivery test.
volatile uint64_t g_self_ipi_seen = 0;

} // namespace

// Runmode: kernel
// Testidea: The INIT/SIPI command path executes cleanly against an absent
// LAPIC: ICR acceptance (delivery-status clear) requires no recipient, so
// the full INIT-assert/deassert + SIPI sequence can run on single-CPU
// builds without disturbing the BSP.
// Input: send_ipi(0xFE, ...) with INIT_ASSERT, INIT_DEASSERT, SIPI.
// Expect: All three return true (absent under both the default and the
//         smp2 variant — BSP is 0, the AP (if any) is 1).
// Depends: arch::APIC::send_ipi, APIC::is_enabled (boot init)
JARVIS_TEST(smp_ipi_absent_target_accepted, "PRE: iocd | POST: none") {
    JARVIS_ASSERT(arch::APIC::is_enabled());
    constexpr uint32_t kAbsentLapic = 0xFE;
    JARVIS_ASSERT(
        arch::APIC::send_ipi(kAbsentLapic, 0, arch::APIC::IpiMode::INIT_ASSERT));
    JARVIS_ASSERT(arch::APIC::send_ipi(kAbsentLapic, 0,
                                       arch::APIC::IpiMode::INIT_DEASSERT));
    JARVIS_ASSERT(
        arch::APIC::send_ipi(kAbsentLapic, 0x70, arch::APIC::IpiMode::SIPI));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: FIXED delivery works end to end: a self-IPI to the BSP LAPIC
// on the dedicated test vector runs the installed IDT handler (flag +
// EOI) within a TSC-bounded window.
// Input: register_handler_raw(0xEF, flag-setter); send_ipi(bsp, 0xEF,
//        FIXED); poll the flag for <= 100 ms of TSC time.
// Expect: send accepted; flag observed (delivery proven, not assumed).
// Depends: arch::APIC::send_ipi, IDT::register_handler_raw,
//         Timer::tsc_freq_hz (calibrated at boot)
JARVIS_TEST(smp_ipi_self_fixed_delivered, "PRE: iocd | POST: none") {
    JARVIS_ASSERT(arch::APIC::is_enabled());
    uint64_t freq = arch::Timer::tsc_freq_hz();
    JARVIS_ASSERT(freq > 0);
    arch::IDT::register_handler_raw(kSelfIpiVector,
                                    [](uint64_t, uint64_t, uint64_t) {
                                        g_self_ipi_seen = 1;
                                        arch::APIC::eoi();
                                    });
    g_self_ipi_seen = 0;
    uint32_t bsp = arch::APIC::lapic_id();
    JARVIS_ASSERT(arch::APIC::send_ipi(bsp, kSelfIpiVector,
                                       arch::APIC::IpiMode::FIXED));
    uint64_t start = arch::rdtsc();
    uint64_t limit = freq / 10; // 100 ms of TSC ticks
    while (g_self_ipi_seen == 0) {
        if (arch::rdtsc() - start > limit)
            break;
        asm volatile("pause");
    }
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), g_self_ipi_seen);
    JARVIS_TEST_PASS();
}

void register_smp_ipi_tests() {
    Logger::info("Registering smp ipi tests");
    JARVIS_REGISTER_TEST(smp_ipi_absent_target_accepted);
    JARVIS_REGISTER_TEST(smp_ipi_self_fixed_delivered);
}
#endif // CONFIG_ARCH_X86_64
