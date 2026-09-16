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

/// @file hpet.cpp
/// @brief Minimal HPET probe for the HRT tick-source chain (issue #16).

#include <kernel/arch/x86_64/hal/hpet.hpp>

namespace arch::hpet {

namespace {
/// @brief Probe latch (set on first call; probe is idempotent).
constinit bool probed_ = false;
/// @brief Cached presence result.
constinit bool present_ = false;
/// @brief Cached frequency in Hz (0 when absent).
constinit uint64_t cached_freq_hz_ = 0;
} // namespace

bool probe() {
    if (probed_) {
        return present_;
    }
    probed_ = true;
#if CONFIG_HAS_HPET
    // Fail-closed: HPET_PHYS_BASE sits above the HHDM identity window, so
    // a raw dereference would fault. ACPI-table discovery plus a VMM
    // mapping must publish a readable base before this path trusts the
    // capabilities register (follow-up work, not issue #16).
    present_ = false;
#else
    present_ = false;
#endif
    return present_;
}

uint64_t freq_hz() {
    if (!probe()) {
        return 0;
    }
    return cached_freq_hz_;
}

uint64_t counter() {
    if (!probe()) {
        return 0;
    }
    return 0;
}

} // namespace arch::hpet
