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

/// @file irq_storm.cpp
/// @brief x86_64 port of the per-tick same-line storm gate (F8 #561).
/// One IrqStormLimiter per kernel image, shared by the handle_interrupt_c
/// dispatch path only. A PIC-line vector (33–47) dispatched more than
/// CONFIG_IRQ_STORM_LIMIT (256) times within one arch::Timer::ticks() value
/// is masked (PIC mirror line AND the live I/O APIC entry — the exact twin
/// of irq_delivery.cpp mask_on_delivery's x86 branch) and counted once
/// instead of dispatched. The two tick vectors are exempt BY NAME: 32
/// (PIT/HPET, InterruptVector::TIMER) and 0xE0 (xAPIC timer,
/// APIC::APIC_TIMER_VECTOR, shared with TSC_DEADLINE) — masking either
/// would stop the scheduler tick and wedge the system. The gate window is
/// 33–47 only (MSI-X/IPI/spurious vectors never counted, so `vector - 32`
/// can never mask a foreign line). Residual note: vectors 34 (PIC cascade)
/// and 39/47 (spurious IRQ7/15) sit inside the window but map by identity
/// (line = vector - 32 = their own line), so no foreign line is reachable.
/// A storm-masked line stays masked until its owning slot releases it (or
/// reboot): storm_reset() restores burst state only. ISR-safe: no locks,
/// no allocation; runs with interrupts masked like the off-x86 limiters.

#include <kernel/arch/hal/irq_storm.hpp>
#include <kernel/arch/hal/irq_vectors.hpp>
#include <kernel/arch/hal/idt.hpp>
#include <kernel/arch/x86_64/hal/apic.hpp>
#include <kernel/arch/hal/interrupt_controller.hpp>
#include <kernel/nexios_config.h>

namespace arch {

namespace {
// F8 #561 gate window = the x86 user-claimable PIC window (IRQ1–15).
constexpr uint8_t kStormVectorMin = 33;
constexpr uint8_t kStormVectorMax = 47;
static_assert(kStormVectorMin == irq_cap_vector_min());
static_assert(kStormVectorMax == irq_cap_vector_max());
} // namespace

/// @brief Single x86 storm limiter (one per kernel image, F8 #561).
static IrqStormLimiter g_storm{};

bool storm_gate(uint64_t vector, uint64_t now_tick) {
    // INV-2: the tick vectors are exempt BY NAME, first. Masking either
    // stops the scheduler tick (PIT/HPET IRQ0 on 32; xAPIC/TSC_DEADLINE on
    // 0xE0) and wedges the system — the #252 EOI-completeness class.
    if (vector == static_cast<uint64_t>(InterruptVector::TIMER) ||
        vector == static_cast<uint64_t>(APIC::APIC_TIMER_VECTOR)) {
        return false;
    }
    // INV-4: gate window 33–47 only — MSI-X (48–255), IPI, 0xE0/0xE1/0xFF
    // and spurious vectors never counted, so `vector - 32` below can only
    // name the line the vector arrived on.
    if (vector < kStormVectorMin || vector > kStormVectorMax) {
        return false;
    }
    if (!g_storm.observe(vector, now_tick)) {
        return false;
    }
    g_storm.note_masked();
    return true;
}

void storm_mask_line(uint8_t vector) {
    // Fail-closed re-check: out-of-window vectors are a no-op.
    if (vector < kStormVectorMin || vector > kStormVectorMax) {
        return;
    }
    // Line identity: vector 33–47 = IRQ1–15, so `vector - 32` is the PIC
    // line (cascade 34 and spurious 39/47 map to their own line).
    const uint8_t irq_line = static_cast<uint8_t>(vector - 32U);
    ArchInterruptController::mask(irq_line);
    // INV-3: on an APIC-enabled kernel the I/O APIC entry is the live
    // delivery path — mask it too, exactly like mask_on_delivery.
    if (APIC::is_enabled()) {
        APIC::mask_irq(irq_line, true);
    }
}

uint64_t storm_trips() { return g_storm.masked_count(); }

void storm_reset() { g_storm.reset(); }

} // namespace arch
