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

/// @file percpu.hpp
/// @brief Per-CPU state block (issue #94).  One 4 KiB page per logical CPU,
/// indexed by LAPIC ID / logical CPU ID.  Accessed via GS_BASE (x86_64).
/// CONFIG_MAX_CPUS pages; the BSP owns index 0, APs own 1...n (issue #25).

#pragma once

#include <types.hpp>
#include <constants.hpp>
#include <kernel/arch/msr.hpp>
#include <kernel/task/task.hpp>
#include <kernel/sync/spinlock.hpp>

namespace arch {

/// @brief Maximum number of logical CPUs supported.
/// Canonical default lives in nexios_config.h (CONFIG_MAX_CPUS 8);
/// this fallback keeps the header self-contained for early includes.
#ifndef CONFIG_MAX_CPUS
#define CONFIG_MAX_CPUS 8
#endif

/// @brief Per-CPU state block (4 KiB-aligned, one page per logical CPU).
/// Migrates global state that must be per-CPU under SMP.
/// @note x86_64: accessed via GS_BASE (gs:0x00 .. gs:0xFF).
///       AArch64/RISC-V: equivalent via TPIDR_EL1 / tp register.
struct alignas(arch::PAGE_SIZE) PerCpu {
    // ─── ABI-frozen slots (existing gs:0x00/0x08 semantics) ─────────────────
    uint64_t user_rsp;          // gs:0x00  — saved user RSP (syscall entry)
    uint64_t kernel_rsp;        // gs:0x08  — loaded kernel stack (ISR epilogue)

    // ─── CPU identity ──────────────────────────────────────────────────────
    uint64_t cpu_id;            // gs:0x10  — logical CPU index (0..CONFIG_MAX_CPUS-1)
    uint64_t lapic_id;          // gs:0x18  — LAPIC ID from MADT

    // ─── Migrated from globals (issue #94) ──────────────────────────────────
    uint64_t isr_nesting_depth; // gs:0x20  — was global isr_nesting_depth
    uint64_t irq_entry_tsc;     // gs:0x28  — was global irq_entry_tsc

    // ─── FPU / scheduler ───────────────────────────────────────────────────
    // Own-CPU FPU register owner (issue #151): which task's FPU state is
    // currently live in this CPU's registers.  nullptr = none since boot.
    kernel::TaskControlBlock *fpu_owner; // gs:0x30 — per-CPU (was global)
    void *current_task;         // gs:0x38  — RESERVED (superseded by CpuContext
                                //   array, issue #25 C1; kept for layout)

    // ─── Interrupt prioritization (issue #26) ──────────────────────────────
    // Own-CPU intended TPR class (source of truth; HW re-asserted from it).
    // C++-only via per_cpu_current()/cpu_index() — no asm access, no alias.
    uint64_t tpr_shadow;        // gs:0x40  — default TPR_CLASS_ACCEPT_ALL

    // ─── Deferred-switch atoms: NOT in this page ─────────────────────────────
    // C1 stores them as Scheduler/global_state arrays indexed by CPU
    // (arch-neutral C++; riscv asm keeps working via array base == [0]).
    // x86 asm indexes via gs:0x10 (INV-PC4 forbids BARE [rel scheduler_*]).

    // ─── Reserved / future expansion ───────────────────────────────────────
    // NOTE: explicit fields total 483 u64s; alignas(PAGE_SIZE) tail-pads
    // sizeof to 4096 (enforced by the static_assert below).  Keep the
    // explicit total constant when adding fields (shrink reserved).
    uint64_t reserved[474];
};

static_assert(sizeof(PerCpu) == arch::PAGE_SIZE,
              "PerCpu must be exactly one 4 KiB page");

/// @brief Array of per-CPU blocks (CONFIG_MAX_CPUS pages).
/// In single-core builds (CONFIG_MAX_CPUS == 1), this is one page.
extern PerCpu per_cpu[CONFIG_MAX_CPUS];

/// @brief Get pointer to current CPU's PerCpu block.
/// @return Pointer to current CPU's PerCpu struct (via GS_BASE).
/// GS_BASE is range-checked against per_cpu[]: callers running before
/// GS is valid (early boot, GS_BASE=0) resolve to &per_cpu[0].
inline PerCpu *per_cpu_current() {
    uint64_t base = arch::rdmsr(arch::MSR_GS_BASE);
    uint64_t lo = reinterpret_cast<uint64_t>(&per_cpu[0]);
    if (base >= lo && base < lo + sizeof(per_cpu))
        return &per_cpu[(base - lo) / arch::PAGE_SIZE];
    return &per_cpu[0];
}

/// @brief Get current CPU's logical ID (index in per_cpu array).
inline uint64_t cpu_id() {
    return per_cpu_current()->cpu_id;
}

/// @brief Current CPU's logical index (0..CONFIG_MAX_CPUS-1).
/// Derived from the GS_BASE range check above: pre-GS callers get 0.
inline uint64_t cpu_index() {
    return static_cast<uint64_t>(per_cpu_current() - &per_cpu[0]);
}

/// @brief Get current CPU's LAPIC ID.
inline uint64_t lapic_id() {
    return per_cpu_current()->lapic_id;
}

/// @brief Initialize per-CPU state for the BSP (CPU 0).
/// Called early in arch_init() before any ISRs or context switches.
void percpu_init_bsp(uint64_t bsp_lapic_id);

/// @brief Initialize per-CPU state for an AP.
/// Called by the AP's trampoline after setting GS_BASE to its PerCpu page.
void percpu_init_ap(uint64_t logical_id, uint64_t lapic_id);

} // namespace arch