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

/// @file early_init.cpp
/// @brief riscv64 early IRQ bring-up (issue #198): stvec/IDT → PLIC
///        (threshold 0 + supervisor external enable) → SBI timer →
///        mtime-frequency calibration. Mirrors kernel.cpp boot order.
///        Raw early init (trap vector, MMU) stays in boot.S (assembly).

#include <kernel/arch/early_init.hpp>
#include <kernel/arch/interrupt_controller.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/arch/timer.hpp>

namespace arch {

// BOOT_ONLY latch for early_irq_init (issue #198, INV-2): namespace scope,
// never function-local (no thread-safe statics on RT paths). Latched on
// success only — a failed first call may be retried.
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
    ArchInterruptController::init();
    Timer::init(frequency_hz);
    (void)Timer::calibrate();
    if (Timer::freq_hz() == 0) {
        return EarlyIrqResult::TIMER_FREQ_UNKNOWN;
    }
    g_early_irq_done = true;
    return EarlyIrqResult::OK;
}

} // namespace arch
