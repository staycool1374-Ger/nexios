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
 * along with this program.  If not, you should have received a copy of
 * the GNU General Public License.  If not, see <https://www.gnu.org/licenses/>.
 */

/// @file sched_trigger.hpp
/// @brief M3 self-triggered apply (F3/Q3): fire the trap that applies a
///        direct-switched deferred arm.  Arch dispatch inside the header
///        (ipi.hpp pattern): shared scheduler/IPC TUs call these; each
///        arch resolves its own transport.
///        Semantics (all arches): asynchronous, at-most-once per call, loss
///        is harmless (the tick path still applies the arm — the direct
///        switch degrades to one tick, never strands).  No lock, no
///        allocation, callable with IF=0.

#pragma once

#include <types.hpp>
#include <kernel/arch/cpu_context.hpp>
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/apic.hpp>
#elif defined(CONFIG_ARCH_AARCH64)
#include <kernel/arch/aarch64/hal/gic.hpp>
#include <kernel/arch/aarch64/hal/smp.hpp>
#endif

namespace arch {

/// @brief Fire the M3 self-triggered apply trap (direct-switch fast path).
/// x86_64: LAPIC FIXED self-IPI with SCHED_APPLY_VECTOR (0xE1, depth-2
/// nested-apply allowlist; unregistered C++ handler = trap only).
/// aarch64: SGI M3_APPLY_SGI (11) to self (Aff0); dispatches through the
/// IDT table to a null slot = no-op; the trap epilogue applies the arm.
/// riscv64: set SSIP (sip bit 1); the S-trap epilogue applies at the
/// nested-SSIP rule; handle_plic_trap clears SSIP first (level-like).
/// @return void always — delivery is best-effort by contract.
inline void sched_apply_trigger() noexcept {
#if defined(CONFIG_ARCH_X86_64)
    APIC::send_self_ipi(APIC::SCHED_APPLY_VECTOR);
#elif defined(CONFIG_ARCH_AARCH64)
    const uint32_t self = static_cast<uint32_t>(
        kernel::smp::cpu_aff0(
            static_cast<uint8_t>(arch::cpu_index())) &
        0xFFU);
    (void)gic_send_sgi(self, M3_APPLY_SGI);
#elif defined(CONFIG_ARCH_RISCV64)
    asm volatile("csrs sip, %0" : : "r"(1ULL << 1U) : "memory");
#else
    // Unknown arch: degrade to the tick path (no trigger available).
#endif
}

} // namespace arch
