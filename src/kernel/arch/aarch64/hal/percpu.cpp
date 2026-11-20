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

/// @file percpu.cpp
/// @brief AArch64 per-CPU init (issue #351, slice-2): TPIDR_EL1 identity.

#include <kernel/arch/aarch64/hal/percpu.hpp>

#include <kernel/arch/cpu_context.hpp>

namespace arch {

// Issue #589 (owner option (a)): VERIFY-ONLY body — must observe NO STORE.
// The aa64 trap vector never maintains the counter (only the layout-parity
// PerCpu 0x20 slot exists, zeroed below and read by nobody), so the
// effective counter (the plain global via isr_nesting_own()) reads 0
// trivially, not by construction. This function exists to VERIFY that
// baseline, never to set it; making aa64 vectors maintain the counter is
// a separate behavior change needing its own audit. The returned live
// value is logged at the handover call site so future drift is
// boot-visible.
uint64_t normalize_isr_nesting_baseline() noexcept {
    return kernel::isr_nesting_own();
}

// Per-CPU array (CONFIG_MAX_CPUS pages, 4 KiB each).
PerCpu per_cpu[CONFIG_MAX_CPUS];

void percpu_init_bsp(uint64_t bsp_mpidr) {
    // Initialize BSP (CPU 0) per-CPU state.
    PerCpu &bsp_block = per_cpu[0];
    bsp_block.cpu_id = 0;
    bsp_block.mpidr = bsp_mpidr;
    bsp_block.user_rsp = 0;
    bsp_block.kernel_rsp = 0;
    bsp_block.isr_nesting_depth = 0;
    bsp_block.irq_entry_tsc = 0;
    bsp_block.fpu_owner = nullptr;
    bsp_block.current_task = nullptr;
    bsp_block.tpr_shadow = 0; // Reserved on AArch64 (x86 TPR has no peer).

    // Set TPIDR_EL1 to point at this CPU's PerCpu page.
    write_tpidr_el1(reinterpret_cast<uint64_t>(&per_cpu[0]));
}

void percpu_init_ap(uint64_t logical_id, uint64_t ap_mpidr) {
    if (logical_id >= CONFIG_MAX_CPUS) {
        // Out of bounds — caller guards this; never install a wild base.
        return;
    }

    PerCpu &ap_block = per_cpu[logical_id];
    ap_block.cpu_id = logical_id;
    ap_block.mpidr = ap_mpidr;
    ap_block.user_rsp = 0;
    ap_block.kernel_rsp = 0;
    ap_block.isr_nesting_depth = 0;
    ap_block.irq_entry_tsc = 0;
    ap_block.fpu_owner = nullptr;
    ap_block.current_task = nullptr;
    ap_block.tpr_shadow = 0; // Reserved on AArch64 (x86 TPR has no peer).

    // Set TPIDR_EL1 for this AP to point at its PerCpu page.
    write_tpidr_el1(reinterpret_cast<uint64_t>(&per_cpu[logical_id]));
}

} // namespace arch
