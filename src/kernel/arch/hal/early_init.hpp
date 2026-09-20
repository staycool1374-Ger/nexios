#pragma once

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

/// @file hal/early_init.hpp
/// @brief Early IRQ-controller + architectural timer init contract (issue
///        #198). Single per-arch entry point run before the scheduler exists:
///        IDT init/load, controller init, timer init, timer calibrate — with
///        global IRQs masked throughout. Fail-closed via EarlyIrqResult;
///        reachable failures never panic.

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace arch {

/// @brief Early IRQ-init outcome (issue #198). Reachable failures are error
///        codes, never ENSURE/panic (CODING_STYLE §5).
enum class EarlyIrqResult : uint8_t {
    OK = 0,              ///< All four stages completed.
    BAD_FREQ = 1,        ///< frequency_hz == 0 (caller error).
    MMIO_MAP_FAILED = 2, ///< Controller MMIO window could not be mapped.
    CTRL_PROBE_FAILED = 3, ///< Controller version/probe unreadable.
    CTRL_TIMEOUT = 4,    ///< Controller ready-poll (GIC redistributor WAKER)
                         ///< did not complete within its bound.
    TIMER_FREQ_UNKNOWN = 5, ///< Timer backing frequency still 0 after init.
};

/// @brief Run the ordered early IRQ bring-up for this architecture.
/// Stages: IDT::init + IDT::load → ArchInterruptController::init →
/// Timer::init(frequency_hz) → Timer::calibrate. Idempotent (BOOT_ONLY):
/// a second call repeats the sequence with no hardware side-effect delta
/// and returns the same result.
/// @param frequency_hz Desired system-tick rate in Hz (must be nonzero).
/// @return EarlyIrqResult outcome; OK on success.
EarlyIrqResult early_irq_init(uint32_t frequency_hz);

/// @cond
#if defined(CONFIG_ARCH_X86_64)
/// @endcond

/// @brief Per-AP early IRQ enable (x86_64 only): LAPIC init + IDT load.
/// Touches per-CPU state only; the shared distributor/PIC setup from
/// early_irq_init() is never repeated here.
/// Defined in src/kernel/arch/x86_64/early_init.cpp.
void early_irq_init_ap();

/// @cond
#endif
/// @endcond

} // namespace arch
