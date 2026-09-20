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

/// @file x86_64/early_init.cpp
/// @brief x86_64 early IRQ bring-up (issue #198): IDT → legacy PIC →
///        local/APIC (+ I/O APIC) → PIT/APIC timer → TSC calibration.
///        Mirrors the production order in kernel.cpp so the boot path and
///        the conformance tests exercise the same sequence.

#include <kernel/arch/early_init.hpp>
#include <kernel/arch/interrupt_controller.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/x86_64/hal/apic.hpp>

namespace arch {

// BOOT_ONLY latch (issue #198, INV-2): Timer::init re-measures the TSC on
// every call, so a repeat entry must return the prior result instead of
// re-running the sequence. Latched on success only — a failed first call
// may be retried.
bool g_early_irq_done = false;

EarlyIrqResult early_irq_init(uint32_t frequency_hz) {
    if (frequency_hz == 0) {
        return EarlyIrqResult::BAD_FREQ;
    }
    if (g_early_irq_done) {
        return EarlyIrqResult::OK;
    }
    IDT::init();
    IDT::load();
    // Legacy 8259A cascade first: a working controller exists even when the
    // APIC is absent or its MMIO window fails to map (fail-open to PIC).
    ArchInterruptController::init();
    if (APIC::is_apic_supported()) {
        if (!APIC::map_mmio()) {
            return EarlyIrqResult::MMIO_MAP_FAILED;
        }
        // Fail-open: APIC::init() returning false leaves the PIC path live;
        // the timer init below then drives the PIT, never a dead APIC timer.
        (void)APIC::init();
    }
    Timer::init(frequency_hz);
    (void)Timer::calibrate();
    // calibrate() is fail-closed with a nonzero fallback frequency on x86
    // (false only selects the fallback source), so the success criterion is
    // the unified frequency, not the boolean.
    if (Timer::freq_hz() == 0) {
        return EarlyIrqResult::TIMER_FREQ_UNKNOWN;
    }
    g_early_irq_done = true;
    return EarlyIrqResult::OK;
}

void early_irq_init_ap() {
    // Per-AP state only (mirrors smp.cpp bring_up order): the shared
    // PIC/APIC distributor setup from early_irq_init() is never repeated.
    APIC::init_ap();
    IDT::load();
}

} // namespace arch
