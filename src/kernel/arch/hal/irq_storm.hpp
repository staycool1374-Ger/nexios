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
/// @brief Allocation-free per-tick same-INTID storm limiter (issue #349,
/// F8 #561). Shared by all three architectures: the caller feeds every
/// dispatched non-timer line INTID; when the same INTID repeats
/// CONFIG_IRQ_STORM_LIMIT times within one tick, observe() returns true and
/// the caller masks the line plus one dmesg ERROR. ISR-safe: plain integer
/// fields, no locks, no allocation. Never feed the timer INTID — off-x86
/// the timer never reaches the gate structurally; on x86 storm_gate()
/// exempts vector 32 (PIT/HPET) and 0xE0 (xAPIC timer) by name.
/// Single instance per kernel image. The plain-uint64_t state races if two
/// CPUs storm the same line in the same tick (shared verbatim with the
/// off-x86 limiters per Q8; the failure direction is fail-closed: a spurious
/// trip masks+logs, a missed trip delays masking by at most one tick).

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

/// @brief Storm-gate contract shared by all three arches (F8 #561): the rule
/// is identical everywhere — one limiter instance per kernel image, a
/// line-kind vector dispatched more than CONFIG_IRQ_STORM_LIMIT (256) times
/// within one arch::Timer::ticks() value is masked, counted once (trips_)
/// and logged once (dmesg BASE + 64, "irq storm masked") instead of
/// dispatched. Off-x86 the ISR calls these through the same object, in the
/// same call order, with the same counter as the pre-F8 inline sequence
/// (observe+note now happen inside storm_gate, before the controller mask
/// write — the trip count and the mask MMIO have no ordering dependency, so
/// the substitution is behavior-identical).
/// @param intid Raw vector/INTID of the dispatched line.
/// @param now_tick Current arch::Timer::ticks() value.
/// @return true when this dispatch hit the per-tick limit; the caller MUST
/// call storm_mask_line() and emit the one dmesg ERROR. The trip counter is
/// incremented inside (replaces the caller-side note_masked()).
bool storm_gate(uint64_t intid, uint64_t now_tick);

/// @brief Arch-effective mask for a tripped line (F8 #561): aarch64/riscv64
/// mask the GIC/PLIC source; x86 masks the PIC mirror line AND the live I/O
/// APIC entry when the APIC is enabled (the mask_on_delivery twin).
/// Out-of-window/non-line vectors are a fail-closed no-op.
/// @param vector Raw vector of the tripped line.
void storm_mask_line(uint8_t vector);

/// @brief Trip count of the arch limiter (diagnostic + test delta).
/// @return Number of recorded storm trips (monotonic).
uint64_t storm_trips();

/// @brief Reset burst state (test isolation). trips_ stays monotonic
/// (tests assert deltas). Does NOT unmask any line: a storm-masked line
/// stays masked until its owning slot releases it (or reboot) — the mask
/// is fail-closed stickiness for genuinely stuck hardware, so tests must
/// restore the controller snapshot themselves.
/// Restores burst state only.
void storm_reset();

} // namespace arch
