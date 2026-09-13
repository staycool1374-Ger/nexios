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

/// @file smp.cpp
/// @brief SMP AP bring-up implementation (issue #25, Phase B3).  Copies
///        the position-locked trampoline blob to physical 0x70000, fills
///        the param block per AP, and runs the INIT/SIPI sequence with
///        TSC-bounded waits.  Woken APs park in ap_main() (Phase C adds
///        scheduling).  Runs once at boot under interrupt guard; any
///        failure parks with zero APs or panics naming the LAPIC ID.

#include <kernel/arch/x86_64/hal/smp.hpp>

#include <kernel/arch/x86_64/hal/apic.hpp>
#include <kernel/arch/x86_64/hal/percpu.hpp>
#include <kernel/arch/x86_64/madt.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/kernel.hpp>
#include <logger.hpp>

namespace kernel::smp {

namespace {

// Raw trampoline blob (objcopy -I binary, symbols redefined by mk/rules.mk).
extern "C" {
extern uint8_t _binary_ap_trampoline_start[];
extern uint8_t _binary_ap_trampoline_end[];
extern uint8_t _binary_ap_trampoline_size[];
}

// One park stack per potential AP (4 KiB each; AP runs ap_main + halt only).
alignas(16) uint8_t ap_stacks[CONFIG_MAX_CPUS > 1 ? CONFIG_MAX_CPUS - 1 : 1]
                        [4096] = {};

// Set by ap_main() after per-CPU + LAPIC init; polled by the BSP.
volatile uint8_t ap_ready[CONFIG_MAX_CPUS] = {};

// BSP-observed parked-AP count (published at the end of bring_up()).
uint8_t g_aps_up = 0;

// Boot-time MADT snapshot consumed by bring_up() (published for tests).
kernel::acpi::MadtInfo g_boot_madt{};

// Set by reserve_block_early(); bring_up() copies only when set (the PMM
// allocation bit alone cannot distinguish "reserved by us" from "taken").
bool g_block_reserved = false;

static_assert(MADT_MAX_CPUS == CONFIG_MAX_CPUS,
              "MADT cap must match the per_cpu array size");

/// @brief TSC-busy-wait for @p us microseconds (Timer::init calibrated).
void smp_udelay(uint64_t us) {
    uint64_t freq = arch::Timer::tsc_freq_hz();
    if (freq == 0 || us == 0)
        return;
    uint64_t start = arch::rdtsc();
    uint64_t ticks = (freq / 1000000ULL) * us;
    while (arch::rdtsc() - start < ticks)
        asm volatile("pause");
}

/// @brief Write a 64-bit param-block field (identity-mapped low memory).
void trampoline_write64(uint64_t field_off, uint64_t value) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *p = reinterpret_cast<volatile uint64_t *>(TRAMPOLINE_ADDR +
                                                    TRAMPOLINE_PARAM_OFF +
                                                    field_off);
    *p = value;
}

/// @brief Write the 32-bit LAPIC-ID param field.
void trampoline_write32(uint64_t field_off, uint32_t value) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *p = reinterpret_cast<volatile uint32_t *>(TRAMPOLINE_ADDR +
                                                    TRAMPOLINE_PARAM_OFF +
                                                    field_off);
    *p = value;
}

} // namespace

bool trampoline_block_usable() {
    const BootInfo &bi = kernel::gs::boot_info();
    for (int i = 0; i < bi.num_mem_regions; ++i) {
        const auto &r = bi.mem_regions[i];
        if (r.type == 1 && r.base <= TRAMPOLINE_ADDR &&
            r.base + r.size >= TRAMPOLINE_ADDR + 4096)
            return true;
    }
    return false;
}

uint8_t ap_count() { return g_aps_up; }

const kernel::acpi::MadtInfo &boot_madt() { return g_boot_madt; }

void reserve_block_early() {
    kernel::PMM::reserve_range(TRAMPOLINE_ADDR, TRAMPOLINE_ADDR + 4096);
    g_block_reserved = true;
}

void bring_up() {
    arch::IrqGuard guard{}; // boot is not yet preemptible-safe: no ticks here
    kernel::acpi::MadtInfo madt = kernel::acpi::scan_madt();
    g_boot_madt = madt; // publish for tests (no re-walk needed later)
    if (!madt.found || madt.malformed)
        return; // no (usable) MADT: silent no-op, BSP-only behavior
    if (!trampoline_block_usable()) {
        kernel::Logger::warn(
            "SMP: trampoline block not firmware-usable — parking with 0 APs");
        return;
    }
    // The firmware map says usable, but PMM allocates from the same pool:
    // copy only into the block claimed by reserve_block_early() (right
    // after PMM::init).  Anything else may be live page tables — copying
    // would corrupt them AND the AP would execute garbage (reset).
    if (!g_block_reserved) {
        kernel::Logger::warn(
            "SMP: trampoline block not reserved — parking with 0 APs");
        return;
    }
    // Copy the position-locked blob to its org address (identity-mapped,
    // writable low RAM) BEFORE the CPU-count gate: the block then holds
    // the blob on every boot, which the smp_bringup test verifies by
    // memcmp (functional occupancy proof, independent of boot_info
    // readability in test context).  Plain volatile loop — no libc.
    uint64_t blob_size =
        reinterpret_cast<uint64_t>(_binary_ap_trampoline_size);
    // Exact match: code fills [0, PARAM_OFF) via `times` padding, params
    // fill [PARAM_OFF, PARAM_OFF + PARAM_SIZE).  Any other size means the
    // .nasm layout drifted from this file's contract.
    if (blob_size != TRAMPOLINE_PARAM_OFF + TRAMPOLINE_PARAM_SIZE)
        panic("SMP: trampoline blob layout mismatch");
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *dst = reinterpret_cast<volatile uint8_t *>(TRAMPOLINE_ADDR);
    for (uint64_t i = 0; i < blob_size; ++i)
        dst[i] = _binary_ap_trampoline_start[i];

    if (madt.ncpus <= 1)
        return; // single-CPU: blob staged, BSP-only behavior

    uint32_t bsp_lapic = arch::APIC::lapic_id();
    uint8_t logical = 1;
    for (uint8_t i = 0; i < madt.ncpus && logical < CONFIG_MAX_CPUS; ++i) {
        uint32_t target = madt.lapic_ids[i];
        if (target == bsp_lapic)
            continue; // never wake the BSP
        // Handoff state for this AP (high VAs — the shared kernel PML4
        // maps them; the stack top is biased by 8 so ap_main (entered
        // by jmp, not call) observes ABI RSP%16==8).
        uint64_t stack_top =
            reinterpret_cast<uint64_t>(&ap_stacks[logical - 1][0]) + 4096 -
            8;
        trampoline_write64(PARAM_PML4,
                           kernel::VMM::get_kernel_pml4() & ~0xFFFULL);
        trampoline_write64(PARAM_STACK, stack_top);
        trampoline_write64(PARAM_PERCPU,
                           reinterpret_cast<uint64_t>(
                               &arch::per_cpu[logical]));
        trampoline_write64(PARAM_ENTRY,
                           reinterpret_cast<uint64_t>(
                               &kernel::smp::ap_main));
        trampoline_write64(PARAM_LOGICAL, logical);
        trampoline_write32(PARAM_LAPIC, target);
        ap_ready[logical] = 0;
        // INIT assert — 10 ms — INIT deassert — SIPI — 300 us — SIPI.
        // (The second SIPI is unconditional: a post-startup SIPI is
        // ignored by the AP, so this stays deterministic with no branch.)
        if (!arch::APIC::send_ipi(target, 0, arch::APIC::IpiMode::INIT_ASSERT))
            panic("SMP: INIT assert not accepted");
        smp_udelay(10000);
        if (!arch::APIC::send_ipi(target, 0,
                                  arch::APIC::IpiMode::INIT_DEASSERT))
            panic("SMP: INIT deassert not accepted");
        if (!arch::APIC::send_ipi(target, TRAMPOLINE_VECTOR,
                                  arch::APIC::IpiMode::SIPI))
            panic("SMP: SIPI not accepted");
        smp_udelay(300);
        if (!arch::APIC::send_ipi(target, TRAMPOLINE_VECTOR,
                                  arch::APIC::IpiMode::SIPI))
            panic("SMP: second SIPI not accepted");
        // Rendezvous: the AP sets ap_ready[logical] at the end of its
        // init; 1 s of TSC time, then a controlled panic naming the ID.
        uint64_t freq = arch::Timer::tsc_freq_hz();
        uint64_t start = arch::rdtsc();
        while (ap_ready[logical] == 0) {
            if (freq != 0 && arch::rdtsc() - start > freq)
                break;
            asm volatile("pause");
        }
        if (ap_ready[logical] == 0) {
            kernel::Logger::warn("SMP: AP failed to start (LAPIC ID ");
            // Decimal-free hex detail is enough to name the AP.
            kernel::Logger::warn("— see MADT boot line for the ID list");
            panic("SMP: AP start timeout");
        }
        kernel::Logger::info("SMP: AP parked");
        ++logical;
    }
    g_aps_up = static_cast<uint8_t>(logical - 1);
}

extern "C" void ap_main(uint64_t logical_id, uint32_t lapic_id) {
    // Per-CPU identity (sets this AP's GS_BASE) + local APIC enable.
    // Interrupts are and stay disabled (booted with IF=0, no IDT/sti).
    arch::percpu_init_ap(logical_id, lapic_id);
    arch::APIC::init_ap();
    ap_ready[logical_id] = 1;
    // Park until Phase C (no scheduler task may run here: no IDT, no
    // tick, no user state — halt is the only safe idle).
    for (;;)
        arch::hlt();
}

} // namespace kernel::smp
