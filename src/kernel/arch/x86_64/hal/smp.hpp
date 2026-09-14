/*
 * NexIOS RTOS — SMP bring-up (Phase B3)
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

/// @file smp.hpp
/// @brief SMP AP bring-up (issue #25, Phase B).  The BSP copies the
///        position-locked trampoline blob to physical 0x70000, fills the
///        param block, and wakes each MADT-listed AP with INIT + SIPI;
///        woken APs run ap_main() (per-CPU init + LAPIC enable) and park
///        in a halt loop (scheduling on APs is Phase C).  Single-CPU
///        runs (MADT ncpus <= 1 or block unusable) are a silent no-op.
///        x86_64-only.

#pragma once

#include <types.hpp>
#include <kernel/arch/x86_64/madt.hpp>

namespace kernel::smp {

// ─── Fixed boot-block contract (mirrors ap_trampoline.nasm) ─────────────────
/// @brief Physical address of the trampoline block (SIPI vector 0x70
///        starts the AP at CS:IP 0x7000:0x0000 = linear 0x70000).
static constexpr uint64_t TRAMPOLINE_ADDR = 0x70000ULL;
/// @brief SIPI vector byte (TRAMPOLINE_ADDR >> 12).
static constexpr uint8_t TRAMPOLINE_VECTOR = 0x70;
/// @brief Param-block offset inside the 4 KiB block.
static constexpr uint64_t TRAMPOLINE_PARAM_OFF = 0x800ULL;
/// @brief Param-block size in bytes (6 fields, see the .nasm layout).
static constexpr uint64_t TRAMPOLINE_PARAM_SIZE = 0x30ULL;
/// @brief GDT offset inside the block (fixed by `times' in the .nasm;
///        the build fails if stage code overflows into it).
static constexpr uint64_t TRAMPOLINE_GDT_OFF = 0x700ULL;
/// @brief GDT size in bytes (4 descriptors + 6-byte pseudo-descriptor).
static constexpr uint64_t TRAMPOLINE_GDT_SIZE = 38ULL;
/// @brief GDT access-byte offsets (null/code32/code64/data) relative to
///        TRAMPOLINE_GDT_OFF; bit 0 (Accessed) is CPU-owned and masked.
static constexpr uint64_t GDT_ACCESS_OFFS[3] = {13, 21, 29};
/// @brief Param-block field offsets (must match the .nasm layout).
static constexpr uint64_t PARAM_PML4 = 0x00;
static constexpr uint64_t PARAM_STACK = 0x08;
static constexpr uint64_t PARAM_PERCPU = 0x10;
static constexpr uint64_t PARAM_ENTRY = 0x18;
static constexpr uint64_t PARAM_LOGICAL = 0x20;
static constexpr uint64_t PARAM_LAPIC = 0x28;

/// @brief True when [TRAMPOLINE_ADDR, +4KiB) is firmware-usable RAM
///        (multiboot2 memory map type 1) — the occupancy contract the
///        fixed-address copy depends on.
bool trampoline_block_usable();

/// @brief Claim the trampoline block from PMM.  Must run right after
///        PMM::init(), before any allocation (low pages go first-come).
///        bring_up() refuses to copy when this never ran (init-order
///        fail-closed: never write a block PMM may own).
void reserve_block_early();

/// @brief Number of APs woken AND parked (0 on single-CPU runs).
uint8_t ap_count();

/// @brief The MADT snapshot bring_up() consumed at boot (cached — firmware
///        tables are static).  Tests read this instead of re-walking the
///        tables (re-walks map kernel-half pages that test isolation
///        reports as PMM leaks).
const kernel::acpi::MadtInfo &boot_madt();

/// @brief Bring up application processors (called once at boot, after
///        Timer::init, under interrupt guard).  Fail-closed: any malformed
///        state (no MADT, unusable block, AP start timeout) parks with
///        zero APs or panics naming the LAPIC ID — never half-woken.
void bring_up();

/// @brief Publish the scheduler start-gate (BSP only, after
///        reboot_from_table() finishes spawning, before its idle loop).
///        APs spin parked until this lands (spec §3.4.3).
void publish_scheduler_ready();

/// @brief Application-processor entry (called from the trampoline's
///        64-bit stage with logical ID + LAPIC ID).  Never returns.
/// @param logical_id Index into arch::per_cpu[] (1..CONFIG_MAX_CPUS-1).
/// @param lapic_id   This AP's LAPIC ID (matches the MADT entry).
extern "C" void ap_main(uint64_t logical_id, uint32_t lapic_id);

} // namespace kernel::smp
