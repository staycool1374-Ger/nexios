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

/// @file irq_storm.hpp
/// @brief Allocation-free per-tick same-INTID storm limiter (issue #349).
/// Shared by the AArch64 and RISC-V interrupt paths. The caller feeds every
/// dispatched non-timer INTID; when the same INTID repeats
/// CONFIG_IRQ_STORM_LIMIT times within one tick, observe() returns true and
/// the caller masks the line plus one dmesg ERROR. ISR-safe: plain integer
/// fields, no locks, no allocation. Never feed the timer INTID.

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace arch {

/// @brief Per-tick same-INTID repetition detector.
class IrqStormLimiter {
  public:
    /// @brief Observe one dispatch of @p intid at tick @p now_tick.
    /// @return true when this INTID hit the per-tick limit (mask it).
    bool observe(uint64_t intid, uint64_t now_tick) {
        if (intid != last_intid_ || now_tick != last_tick_) {
            last_intid_ = intid;
            last_tick_ = now_tick;
            burst_ = 1;
            return false;
        }
        if (burst_ < CONFIG_IRQ_STORM_LIMIT) {
            ++burst_;
            return false;
        }
        return true;
    }

    /// @brief Number of times the limiter tripped (diagnostic).
    /// @return Trip count.
    uint64_t masked_count() const { return trips_; }

    /// @brief Record that the caller masked the line (diagnostic).
    void note_masked() { ++trips_; }

    /// @brief Reset burst state (test isolation).
    void reset() {
        last_intid_ = 0;
        last_tick_ = 0;
        burst_ = 0;
    }

  private:
    uint64_t last_intid_ = 0;
    uint64_t last_tick_ = 0;
    uint64_t burst_ = 0;
    uint64_t trips_ = 0;
};

} // namespace arch
