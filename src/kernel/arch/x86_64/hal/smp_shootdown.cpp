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
/// @brief x86_64 strong cross-CPU TLB-shootdown hook (issue #389).
///        Overrides the weak `VMM::smp_tlb_shootdown_hook` no-op: fans
///        each shootdown out to every up CPU through the existing
///        `ShootdownBatch` transport (queue collapse/order/overflow +
///        bounded 1M-spin drain, issue #159 protocol untouched).  The
///        choke point already flushed local first, so self is skipped
///        (a self-IPI under IF=0 cannot land — the skip is load-bearing,
///        not an optimization).  PCID travels in the slot and is honored
///        on apply; the all-flag selects a full purge.  No new IDT vector
///        (ABI frozen): delivery reuses SHOOTDOWN_BATCH_VECTOR, whose ISR
///        is registered in kernel.cpp (shared IDT serves BSP and APs).
/// @note Concurrency/caller contract: Lock: none.  Context: any-task.
///       SMP: fan-out to up CPUs only; per-leg bounded (1M-spin drain;
///       a down target never drains — entries stay queued for the next
///       flush instead of spinning forever).

#include <kernel/memory/vmm.hpp>
#include <kernel/arch/x86_64/hal/shootdown_ipi.hpp>
#include <kernel/arch/x86_64/hal/smp.hpp>
#include <kernel/arch/x86_64/hal/percpu.hpp>

namespace kernel {

void VMM::smp_tlb_shootdown_hook(uint64_t va, uint16_t context_id, bool all) {
    // Pre-gate the parked APs cannot drain (IF=0 until scheduler start):
    // local was already flushed by the choke — fan out only when live.
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
        if (!arch::ShootdownBatch::queue_remote(cpu, va, context_id, all)) {
            arch::ShootdownBatch::flush_target(cpu);
            arch::ShootdownBatch::queue_remote(cpu, va, context_id, all);
        }
        arch::ShootdownBatch::flush_target(cpu);
    }
}

} // namespace kernel
