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
/// @brief Per-hart state block (issue #351, slice-3).  One 4 KiB page per
///        logical hart, indexed by logical hart ID.  Accessed via tp
///        (RISC-V), mirroring the x86_64 GS_BASE approach in
///        arch/x86_64/hal/percpu.hpp and the aa64 TPIDR_EL1 approach in
///        arch/aarch64/hal/percpu.hpp.  Field offsets match the x86 layout
///        so cross-arch readers (test isolation, debuggers) see one shape.
///        CONFIG_MAX_CPUS pages; the BSP owns index 0, APs own 1...n.

#include <types.hpp>
#include <constants.hpp>
#include <kernel/arch/hal/io.hpp>

namespace arch {

/// @brief Maximum number of logical harts supported.
/// Canonical default lives in nexios_config.h (CONFIG_MAX_CPUS 8);
/// this fallback keeps the header self-contained for early includes.
#ifndef CONFIG_MAX_CPUS
#define CONFIG_MAX_CPUS 8
#endif

/// @brief Per-hart state block (4 KiB-aligned, one page per logical hart).
/// Migrates global state that must be per-hart under SMP.
/// @note RISC-V: accessed via tp (holds &per_hart[hart]).
struct alignas(arch::PAGE_SIZE) PerCpu {
    // ─── ABI-frozen slots (mirrors x86 gs:0x00/0x08) ───────────────────
    uint64_t user_rsp;          // 0x00 — saved user RSP (syscall entry)
    uint64_t kernel_rsp;        // 0x08 — loaded kernel stack (trap epilogue)

    // ─── Hart identity ─────────────────────────────────────────────────
    uint64_t cpu_id;            // 0x10 — logical hart index (0..CONFIG_MAX_CPUS-1)
    uint64_t hart_id;           // 0x18 — mhartid value (matches DTB reg)

    // ─── Migrated from globals (matches x86 offsets) ───────────────────
    uint64_t isr_nesting_depth; // 0x20 — was global isr_nesting_depth
    uint64_t irq_entry_tsc;     // 0x28 — was global irq_entry_tsc

    // ─── FPU / scheduler ───────────────────────────────────────────────
    // Own-hart FPU register owner (issue #151): which task's FPU state is
    // currently live in this hart's registers.  nullptr = none since boot.
    void *fpu_owner;            // 0x30 — per-hart (was global)
    void *current_task;         // 0x38 — RESERVED (superseded by CpuContext
                                //   array, issue #25 C1; kept for layout)

    // ─── Interrupt prioritization (reserved on RISC-V) ─────────────────
    // x86 carries the TPR shadow here (gs:0x40); RISC-V has no TPR, so
    // this slot is reserved to keep the cross-arch layout identical.
    uint64_t tpr_shadow;        // 0x40 — reserved (x86 TPR class shadow)

    // ─── Reserved / future expansion ───────────────────────────────────
    // NOTE: explicit fields total 483 u64s; alignas(PAGE_SIZE) tail-pads
    // sizeof to 4096 (enforced by the static_assert below).  Keep the
    // explicit total constant when adding fields (shrink reserved).
    uint64_t reserved[474];
};

static_assert(sizeof(PerCpu) == arch::PAGE_SIZE,
              "PerCpu must be exactly one 4 KiB page");

/// @brief Array of per-hart blocks (CONFIG_MAX_CPUS pages).
/// In single-core builds (CONFIG_MAX_CPUS == 1), this is one page.
extern PerCpu per_cpu[CONFIG_MAX_CPUS];

/// @brief Get pointer to current hart's PerCpu block.
/// @return Pointer to current hart's PerCpu struct (via tp).
/// tp is range-checked against per_cpu[]: callers running before tp is
/// valid (early boot, tp=0) resolve to &per_cpu[0].
inline PerCpu *per_cpu_current() {
    const uint64_t base = read_tp();
    const uint64_t array_lo = reinterpret_cast<uint64_t>(&per_cpu[0]);
    if (base >= array_lo && base < array_lo + sizeof(per_cpu)) {
        return &per_cpu[(base - array_lo) / arch::PAGE_SIZE];
    }
    return &per_cpu[0];
}

/// @brief Get current hart's logical ID (index in per_cpu array).
/// @return Logical hart index (0..CONFIG_MAX_CPUS-1).
inline uint64_t cpu_id() {
    return per_cpu_current()->cpu_id;
}

/// @brief Current hart's logical index (0..CONFIG_MAX_CPUS-1).
/// Derived from the tp range check above: pre-tp callers get 0.
/// @return Logical hart index.
inline uint64_t cpu_index() {
    return static_cast<uint64_t>(per_cpu_current() - &per_cpu[0]);
}

/// @brief Get current hart's hardware hart ID.
/// @return mhartid snapshot stored at percpu init.
inline uint64_t cpu_hartid() {
    return per_cpu_current()->hart_id;
}

/// @brief Initialize per-hart state for the BSP (hart 0).
/// Called early in arch init before any trap can run; points tp at
/// per_cpu[0].
/// @param bsp_hart BSP mhartid value (boot hart, whatever firmware chose).
void percpu_init_bsp(uint64_t bsp_hart);

/// @brief Initialize per-hart state for an AP.
/// Called by the AP entry stub after setting its stack, before enabling
/// interrupts on that hart.
/// @param logical_id Index into per_cpu[] (1..CONFIG_MAX_CPUS-1).
/// @param ap_hart This AP's mhartid value (must match its DTB reg).
void percpu_init_ap(uint64_t logical_id, uint64_t ap_hart);

} // namespace arch
