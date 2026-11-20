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
/// @brief RISC-V64 per-hart init (issue #351, slice-3): tp identity.

#include <kernel/arch/riscv64/hal/percpu.hpp>

#include <kernel/arch/cpu_context.hpp>
#include <kernel/arch/io.hpp>
#include <assert.hpp>

namespace arch {

// Issue #589 (owner option (a)): the ONLY writer of the handover
// normalization. The S-mode trap vector addresses the plain global via
// `la t0, isr_nesting_depth` (syscall_entry.S:160/253/468), so the plain
// global is the authoritative counter on rv64; the PerCpu 0x20 slot zeroed
// below is layout parity only (normalizing that slot would be a no-op no
// reader observes). Precondition INV-NEST-1 (single-threaded, SIE==0,
// zero outstanding frames) is asserted, not trusted: the handover call
// site is the last boot point where it holds. cli()-first is declined by
// design: OpenSBI enters S-mode with SIE==0 and the scheduler does not
// exist yet, so no concurrent RMW from the asm entry/tail sites can
// interleave; the ENSURE documents the contract instead of masking a
// firmware that violates it.
uint64_t normalize_isr_nesting_baseline() noexcept {
    ENSURE(!arch::interrupts_enabled());
    uint64_t &nesting = kernel::isr_nesting_own();
    const uint64_t observed = nesting;
    __atomic_store_n(&nesting, 0, __ATOMIC_RELEASE);
    return observed;
}

// Per-hart array (CONFIG_MAX_CPUS pages, 4 KiB each).
PerCpu per_cpu[CONFIG_MAX_CPUS];

/// @brief Linkage anchor for the trap entry (issue #351 S3 fix-2).
/// Holds &per_cpu[0] as a C-linkage constant so assembly can derive the
/// current hart's slot index as (tp - base) >> 12 without a PC-relative
/// reference to the C++-mangled array (out of range is impossible here:
/// same linkage unit, absolute address materialized by the linker).
/// Marked used: no C++ TU references it, only syscall_entry.S.
extern "C" PerCpu *const arch_per_cpu_base __attribute__((used)) =
    per_cpu;

void percpu_init_bsp(uint64_t bsp_hart) {
    // Initialize BSP (hart 0) per-hart state.
    PerCpu &bsp_block = per_cpu[0];
    bsp_block.cpu_id = 0;
    bsp_block.hart_id = bsp_hart;
    bsp_block.user_rsp = 0;
    bsp_block.kernel_rsp = 0;
    bsp_block.isr_nesting_depth = 0;
    bsp_block.irq_entry_tsc = 0;
    bsp_block.fpu_owner = nullptr;
    bsp_block.current_task = nullptr;
    bsp_block.tpr_shadow = 0; // Reserved on RISC-V (x86 TPR has no peer).

    // Set tp to point at this hart's PerCpu page.
    write_tp(reinterpret_cast<uint64_t>(&per_cpu[0]));
}

void percpu_init_ap(uint64_t logical_id, uint64_t ap_hart) {
    if (logical_id >= CONFIG_MAX_CPUS) {
        // Out of bounds — caller guards this; never install a wild base.
        return;
    }

    PerCpu &ap_block = per_cpu[logical_id];
    ap_block.cpu_id = logical_id;
    ap_block.hart_id = ap_hart;
    ap_block.user_rsp = 0;
    ap_block.kernel_rsp = 0;
    ap_block.isr_nesting_depth = 0;
    ap_block.irq_entry_tsc = 0;
    ap_block.fpu_owner = nullptr;
    ap_block.current_task = nullptr;
    ap_block.tpr_shadow = 0; // Reserved on RISC-V (x86 TPR has no peer).

    // Set tp for this AP to point at its PerCpu page.
    write_tp(reinterpret_cast<uint64_t>(&per_cpu[logical_id]));
}

} // namespace arch
