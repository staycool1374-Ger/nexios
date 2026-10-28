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
/// @brief Per-CPU state block (issue #351, slice-2).  One 4 KiB page per
///        logical CPU, indexed by logical CPU ID.  Accessed via TPIDR_EL1
///        (AArch64), mirroring the x86_64 GS_BASE approach in
///        arch/x86_64/hal/percpu.hpp.  Field offsets match the x86 layout
///        so cross-arch readers (test isolation, debuggers) see one shape.
///        CONFIG_MAX_CPUS pages; the BSP owns index 0, APs own 1...n.

#include <types.hpp>
#include <constants.hpp>

namespace arch {

/// @brief Maximum number of logical CPUs supported.
/// Canonical default lives in nexios_config.h (CONFIG_MAX_CPUS 8);
/// this fallback keeps the header self-contained for early includes.
#ifndef CONFIG_MAX_CPUS
#define CONFIG_MAX_CPUS 8
#endif

/// @brief Per-CPU state block (4 KiB-aligned, one page per logical CPU).
/// Migrates global state that must be per-CPU under SMP.
/// @note AArch64: accessed via TPIDR_EL1 (holds &per_cpu[cpu]).
struct alignas(arch::PAGE_SIZE) PerCpu {
    // ─── ABI-frozen slots (mirrors x86 gs:0x00..0x08) ───────────────────
    uint64_t user_rsp;          // 0x00 — saved user RSP (syscall entry)
    uint64_t kernel_rsp;        // 0x08 — loaded kernel stack (ISR epilogue)

    // ─── CPU identity ──────────────────────────────────────────────────
    uint64_t cpu_id;            // 0x10 — logical CPU index (0..CONFIG_MAX_CPUS-1)
    uint64_t mpidr;             // 0x18 — MPIDR_EL1 value (affinity levels)

    // ─── Migrated from globals (matches x86 offsets) ───────────────────
    uint64_t isr_nesting_depth; // 0x20 — was global isr_nesting_depth
    uint64_t irq_entry_tsc;     // 0x28 — was global irq_entry_tsc

    // ─── FPU / scheduler ───────────────────────────────────────────────
    // Own-CPU FPU register owner (issue #151): which task's FPU state is
    // currently live in this CPU's registers.  nullptr = none since boot.
    void *fpu_owner;            // 0x30 — per-CPU (was global)
    void *current_task;         // 0x38 — RESERVED (superseded by CpuContext
                                //   array, issue #25 C1; kept for layout)

    // ─── Interrupt prioritization (reserved on AArch64) ────────────────
    // x86 carries the TPR shadow here (gs:0x40); AArch64 has no TPR, so
    // this slot is reserved to keep the cross-arch layout identical.
    uint64_t tpr_shadow;        // 0x40 — reserved (x86: TPR class shadow)

    // ─── Reserved / future expansion ───────────────────────────────────
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

/// @brief Read TPIDR_EL1 (per-CPU base register).
/// @return Raw TPIDR_EL1 value.
inline uint64_t read_tpidr_el1() {
    uint64_t reg_value = 0;
    asm volatile("mrs %0, tpidr_el1" : "=r"(reg_value));
    return reg_value;
}

/// @brief Write TPIDR_EL1 (per-CPU base register).
/// @param base_value Value to store (normally &per_cpu[cpu]).
inline void write_tpidr_el1(uint64_t base_value) {
    asm volatile("msr tpidr_el1, %0" : : "r"(base_value) : "memory");
    asm volatile("isb");
}

/// @brief Read MPIDR_EL1 (multiprocessor affinity register).
/// @return Raw MPIDR_EL1 value (Aff3..Aff0 + U bit).
inline uint64_t read_mpidr_el1() {
    uint64_t reg_value = 0;
    asm volatile("mrs %0, mpidr_el1" : "=r"(reg_value));
    return reg_value;
}

/// @brief MPIDR Aff0 field (level-0 affinity = aa64 cpu id in DTB reg).
/// Slice-0 gates secondaries on Aff0 == 0; slice-2 maps DTB reg to this.
/// @return Aff0 (bits [7:0] of MPIDR_EL1).
inline uint64_t mpidr_aff0() {
    return read_mpidr_el1() & 0xFFU;
}

/// @brief Get pointer to current CPU's PerCpu block.
/// @return Pointer to current CPU's PerCpu struct (via TPIDR_EL1).
/// TPIDR_EL1 is range-checked against per_cpu[]: callers running before
/// TPIDR is valid (early boot, TPIDR=0) resolve to &per_cpu[0].
inline PerCpu *per_cpu_current() {
    const uint64_t base = read_tpidr_el1();
    const uint64_t array_lo = reinterpret_cast<uint64_t>(&per_cpu[0]);
    if (base >= array_lo && base < array_lo + sizeof(per_cpu)) {
        return &per_cpu[(base - array_lo) / arch::PAGE_SIZE];
    }
    return &per_cpu[0];
}

/// @brief Get current CPU's logical ID (index in per_cpu array).
/// @return Logical CPU index (0..CONFIG_MAX_CPUS-1).
inline uint64_t cpu_id() {
    return per_cpu_current()->cpu_id;
}

/// @brief Current CPU's logical index (0..CONFIG_MAX_CPUS-1).
/// Derived from the TPIDR_EL1 range check above: pre-TPIDR callers get 0.
/// @return Logical CPU index.
inline uint64_t cpu_index() {
    return static_cast<uint64_t>(per_cpu_current() - &per_cpu[0]);
}

/// @brief Get current CPU's MPIDR value.
/// @return MPIDR_EL1 snapshot stored at percpu init.
inline uint64_t cpu_mpidr() {
    return per_cpu_current()->mpidr;
}

/// @brief Initialize per-CPU state for the BSP (CPU 0).
/// Called early in arch init before any ISR can run; points TPIDR_EL1 at
/// per_cpu[0].
/// @param bsp_mpidr BSP MPIDR_EL1 value (Aff0 == 0 by the slice-0 gate).
void percpu_init_bsp(uint64_t bsp_mpidr);

/// @brief Initialize per-CPU state for an AP.
/// Called by the AP entry stub after setting its stack, before enabling
/// interrupts on that CPU.
/// @param logical_id Index into per_cpu[] (1..CONFIG_MAX_CPUS-1).
/// @param ap_mpidr This AP's MPIDR_EL1 value (must match its DTB reg).
void percpu_init_ap(uint64_t logical_id, uint64_t ap_mpidr);

} // namespace arch
