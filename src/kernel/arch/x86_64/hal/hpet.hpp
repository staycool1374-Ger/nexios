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

/// @file hpet.hpp
/// @brief Minimal HPET probe for the HRT tick-source chain (issue #16).

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace arch::hpet {

/// @brief HPET MMIO base (from the ACPI HPET table address field).
static constexpr uint64_t HPET_PHYS_BASE = 0xFED00000ULL;
/// @brief General capabilities and ID register offset.
static constexpr uint64_t HPET_REG_CAP = 0x000ULL;
/// @brief Main counter register offset.
static constexpr uint64_t HPET_REG_COUNTER = 0x0F0ULL;
/// @brief Period mask in the capabilities register (femtoseconds).
static constexpr uint64_t HPET_CAP_PERIOD_MASK = 0xFFFFFFFF00000000ULL;
/// @brief Period shift in the capabilities register.
static constexpr uint64_t HPET_CAP_PERIOD_SHIFT = 32ULL;
/// @brief Femtoseconds per second (period → frequency conversion).
static constexpr uint64_t HPET_FEMTO_PER_SEC = 1000000000000000ULL;

/// @brief Probe for an HPET (idempotent, fail-closed).
/// Returns false until ACPI-table discovery plus a VMM mapping make the
/// base readable; CONFIG_HAS_HPET gates the register path. Never faults:
/// without a mapping the probe reports absent.
/// @return true when an HPET is present and readable.
bool probe();
/// @brief HPET frequency in Hz (0 when absent).
/// @return Frequency in Hz, or 0.
uint64_t freq_hz();
/// @brief HPET main counter value (0 when absent).
/// @return Counter value, or 0.
uint64_t counter();

} // namespace arch::hpet
