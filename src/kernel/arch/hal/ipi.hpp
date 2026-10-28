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

/// @file ipi.hpp
/// @brief Arch-neutral cross-CPU IPI send interface (issue #351, slice-4).
/// Extends the tpr_guard.hpp pattern (arch dispatch inside the header,
/// no CONFIG_ARCH at call sites) to reschedule/shootdown delivery:
/// shared scheduler TUs call these; each arch resolves its own
/// transport (x86 LAPIC vectors, aa64 SGIs, rv64 SBI + mailbox bit).
/// x86 behavior is a move, not semantics (byte-identical send sequence).

#pragma once

#include <types.hpp>
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/apic.hpp>
#include <kernel/arch/x86_64/hal/percpu.hpp>
#elif defined(CONFIG_ARCH_AARCH64)
#include <kernel/arch/aarch64/hal/gic.hpp>
#include <kernel/arch/aarch64/hal/smp.hpp>
#elif defined(CONFIG_ARCH_RISCV64)
#include <kernel/arch/riscv64/hal/smp.hpp>
#endif

namespace arch {

/// @brief Send a reschedule IPI to a CPU (issue #351, slice-4).
/// The caller owns the wake payload (shared wake mailbox published
/// before this call on x86/aa64; rv64 sets its own mailbox bit here).
/// x86_64: LAPIC FIXED IPI with SCHED_VECTOR (moved verbatim from
/// scheduler_registry.cpp).  aarch64: SGI SMP_SCHED_SGI to the CPU's
/// Aff0 (GIC targets are Aff0, not the logical index).  riscv64:
/// forwards to kernel::smp::send_resched_ipi (mailbox RESCHED bit +
/// legacy SBI hart-mask ecall, C2: present in QEMU virt OpenSBI).
/// @param cpu Target logical CPU index.
/// @return true when the send was accepted, false when rejected
///         (out-of-range target or transport refusal; queued payloads
///         stay queued for the next IPI/tick).
inline bool send_resched_ipi(uint64_t cpu) noexcept {
#if defined(CONFIG_ARCH_X86_64)
    if (cpu >= CONFIG_MAX_CPUS) {
        return false;
    }
    const uint32_t lapic = (cpu == cpu_index()) ? APIC::lapic_id()
                                                : per_cpu[cpu].lapic_id;
    return APIC::send_ipi(lapic, APIC::SCHED_VECTOR, APIC::IpiMode::FIXED);
#elif defined(CONFIG_ARCH_AARCH64)
    if (cpu >= CONFIG_MAX_CPUS) {
        return false;
    }
    const uint32_t target = static_cast<uint32_t>(
        kernel::smp::ap_aff0(static_cast<uint8_t>(cpu)) & 0xFFU);
    return gic_send_sgi(target, SMP_SCHED_SGI);
#elif defined(CONFIG_ARCH_RISCV64)
    return kernel::smp::send_resched_ipi(cpu);
#else
    (void)cpu;
    return false;
#endif
}

/// @brief Send a TLB-shootdown IPI to a CPU (issue #351, slice-4).
/// Batch framing stays with the caller (queue/flush/apply); this only
/// delivers the notification.  x86_64: LAPIC FIXED IPI with
/// SHOOTDOWN_BATCH_VECTOR (moved verbatim from flush_target).
/// aarch64: SGI SMP_SHOOTDOWN_SGI to the CPU's Aff0 (the slice-5
/// handler owns the apply side).  riscv64: forwards to
/// kernel::smp::send_shootdown_ipi (mailbox SHOOTDOWN bit + legacy
/// SBI hart-mask ecall, same C2 contract as the resched leg).
/// @param cpu Target logical CPU index.
/// @return true when the send was accepted, false otherwise (entries
///         stay queued for the caller's retry).
inline bool send_shootdown_ipi(uint64_t cpu) noexcept {
#if defined(CONFIG_ARCH_X86_64)
    if (cpu >= CONFIG_MAX_CPUS) {
        return false;
    }
    const uint32_t lapic = (cpu == cpu_index()) ? APIC::lapic_id()
                                                : per_cpu[cpu].lapic_id;
    return APIC::send_ipi(lapic, APIC::SHOOTDOWN_BATCH_VECTOR,
                          APIC::IpiMode::FIXED);
#elif defined(CONFIG_ARCH_AARCH64)
    if (cpu >= CONFIG_MAX_CPUS) {
        return false;
    }
    const uint32_t target = static_cast<uint32_t>(
        kernel::smp::ap_aff0(static_cast<uint8_t>(cpu)) & 0xFFU);
    return gic_send_sgi(target, SMP_SHOOTDOWN_SGI);
#elif defined(CONFIG_ARCH_RISCV64)
    return kernel::smp::send_shootdown_ipi(cpu);
#else
    (void)cpu;
    return false;
#endif
}

} // namespace arch
