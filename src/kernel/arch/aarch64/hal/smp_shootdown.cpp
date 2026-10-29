/*
 * NexIOS RTOS — Cross-CPU TLB Shootdown (SMP)
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

/// @file smp_shootdown.cpp
/// @brief AArch64 strong cross-CPU TLB-shootdown hook (issue #389).
///        Overrides the weak `VMM::smp_tlb_shootdown_hook` no-op: sends
///        one SGI SMP_SHOOTDOWN_SGI per up CPU (Aff0 via `ap_aff0`, S4
///        interface `arch::send_shootdown_ipi`).  Purge-on-receipt: the
///        SGI carries no payload, so the target applies a self-purge via
///        the existing `tlb_flush_all` primitive (`tlbi vmalle1`) in the
///        SGI-10 handler registered by kernel.cpp — no shared VA queue
///        (a per-VA queue would be new cross-CPU shared state, i.e. #386
///        surface).  IS form (`VMALLE1IS`) is explicitly NOT required:
///        the handler runs on the target, so a self-purge suffices.
/// @note Concurrency/caller contract: Lock: none.  Context: any-task.
///       SMP: send-only fan-out to up CPUs; straight-line SGI sends,
///       no wait, no retry (unmapped Aff0 skips fail-closed).

#include <kernel/memory/vmm.hpp>
#include <kernel/arch/hal/ipi.hpp>
#include <kernel/arch/aarch64/hal/smp.hpp>
#include <kernel/arch/aarch64/hal/percpu.hpp>

namespace kernel {

void VMM::smp_tlb_shootdown_hook(uint64_t va, uint16_t context_id, bool all) {
    (void)va;
    (void)context_id;
    (void)all;
    // Pre-gate the parked APs cannot drain yet: local was already
    // flushed by the choke — fan out only when live.
    if (!kernel::smp::scheduler_started()) {
        return;
    }
    const uint64_t self_cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    const uint64_t up_count =
        static_cast<uint64_t>(kernel::smp::ap_count());
    for (uint64_t cpu = 1; cpu <= up_count; ++cpu) {
        if (cpu >= CONFIG_MAX_CPUS) {
            break;
        }
        if (cpu == self_cpu) {
            continue;
        }
        const uint64_t aff0 =
            kernel::smp::ap_aff0(static_cast<uint8_t>(cpu));
        if (aff0 == 0xFFU) {
            continue;
        }
        arch::send_shootdown_ipi(cpu);
    }
}

} // namespace kernel
