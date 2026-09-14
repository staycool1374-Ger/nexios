/*
 * NexIOS RTOS — SMP bring-up (Phase 5)
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

/// @file test_core_isolation.cpp
/// @brief Core-state isolation tests (issue #85, module 5): per-CPU
///        slot sizing/stride, BSP slot ownership, cross-slot write
///        isolation, AP-slot ownership envelope, live PML4 validity.
///        Per-core TSS/IST stacks do not exist (single global TSSBlock
///        in GDT) and per-AP stack/PML4 handles have no accessors —
///        documented gaps, need main-branch API.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/x86_64/hal/percpu.hpp>
#include <kernel/arch/x86_64/hal/apic.hpp>
#include <kernel/arch/x86_64/hal/smp.hpp>
#include <kernel/arch/x86_64/madt.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/nexios_config.h>

using namespace kernel;

namespace {

// Scratch offset inside the 4 KiB PerCpu page, past all live fields
// (frozen slots end at current_task 0x38 + tpr_shadow): never touched
// by ISR asm, the scheduler, or AP bring-up — a clean cross-slot
// leakage probe.
constexpr uint64_t kScratchOff = 0x800;
constexpr uint64_t kSentinel = 0xC0DEC0DEC0DEC0DEULL;

uint64_t slot_scratch(uint8_t slot) {
    const volatile uint8_t *base =
        reinterpret_cast<const volatile uint8_t *>(&arch::per_cpu[slot]);
    uint64_t value = 0;
    for (uint64_t i = 0; i < sizeof(value); ++i) {
        value |= static_cast<uint64_t>(base[kScratchOff + i]) << (i * 8);
    }
    return value;
}

void set_slot_scratch(uint8_t slot, uint64_t value) {
    volatile uint8_t *base =
        reinterpret_cast<volatile uint8_t *>(&arch::per_cpu[slot]);
    for (uint64_t i = 0; i < sizeof(value); ++i) {
        base[kScratchOff + i] =
            static_cast<uint8_t>((value >> (i * 8)) & 0xFFU);
    }
}

}  // namespace

// Runmode: kernel
// Testidea: Every per-CPU slot is exactly one page and page-strided:
//           slot i+1 starts 4096 bytes after slot i, so no two cores'
//           state can overlap by construction.
// Input: Addresses/sizes of arch::per_cpu[0..CONFIG_MAX_CPUS).
// Expect: sizeof(PerCpu) == 4096; stride == 4096 for every pair.
// Depends: arch::PerCpu layout, CONFIG_MAX_CPUS
JARVIS_TEST(core_slots_sized_isolated, "PRE: none | POST: none") {
    JARVIS_ASSERT(sizeof(arch::PerCpu) == 4096);
    for (uint8_t slot = 0; slot + 1 < CONFIG_MAX_CPUS; ++slot) {
        uint64_t low = reinterpret_cast<uint64_t>(
            &arch::per_cpu[slot]);
        uint64_t high = reinterpret_cast<uint64_t>(
            &arch::per_cpu[slot + 1]);
        JARVIS_ASSERT(high - low == 4096);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The running CPU owns slot 0: cpu_id reads 0 and the live
//           GS_BASE routes at its own page — never a sibling's.
// Input: per_cpu[0].cpu_id, per_cpu_current(), RDMSR(GS_BASE).
// Expect: cpu_id == 0, current == &per_cpu[0], GS_BASE == &per_cpu[0].
// Depends: arch::percpu_init_bsp boot wiring (issue #25)
JARVIS_TEST(core_bsp_owns_slot_zero, "PRE: none | POST: none") {
    JARVIS_ASSERT(arch::per_cpu[0].cpu_id == 0);
    JARVIS_ASSERT(arch::per_cpu_current() == &arch::per_cpu[0]);
    uint64_t lo = 0;
    uint64_t hi = 0;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101));
    JARVIS_ASSERT((lo | (hi << 32)) ==
                  reinterpret_cast<uint64_t>(&arch::per_cpu[0]));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A write into one core's slot never leaks into the
//           neighbour: store a sentinel in slot 0's scratch area and
//           prove slot 1's scratch area is bit-identical before/after.
// Input: Sentinel store + neighbour readback around it.
// Expect: Slot 0 reads back the sentinel; slot 1 unchanged; restored.
// Depends: arch::per_cpu slot stride (core_slots_sized_isolated)
JARVIS_TEST(core_slot_write_isolated, "PRE: none | POST: none") {
    uint64_t own_before = slot_scratch(0);
    uint64_t next_before = slot_scratch(1);
    set_slot_scratch(0, kSentinel);
    JARVIS_ASSERT(slot_scratch(0) == kSentinel);
    JARVIS_ASSERT(slot_scratch(1) == next_before);
    set_slot_scratch(0, own_before);
    JARVIS_ASSERT(slot_scratch(0) == own_before);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Every AP slot is in exactly one of two states: pristine
//           (never woken — zeroed cpu_id AND lapic_id) or validly owned
//           (cpu_id == slot index, lapic_id listed in the boot MADT).
//           A half-initialised slot (id set, lapic zero or foreign) is
//           a bring-up/isolation defect.  Variant-proof: single-CPU
//           runs leave all AP slots pristine; -smp 2 owns slot 1.
// Input: per_cpu[1..MAX) + smp::boot_madt() snapshot.
// Expect: Each slot pristine or (cpu_id == index && lapic in MADT).
// Depends: kernel::smp::boot_madt, percpu_init_ap
JARVIS_TEST(core_ap_slots_owned_or_pristine, "PRE: none | POST: none") {
    const acpi::MadtInfo &info = smp::boot_madt();
    JARVIS_ASSERT(info.found);
    for (uint8_t slot = 1; slot < CONFIG_MAX_CPUS; ++slot) {
        uint64_t cpu_id = arch::per_cpu[slot].cpu_id;
        uint32_t lapic_id = arch::per_cpu[slot].lapic_id;
        if (cpu_id == 0) {
            JARVIS_ASSERT(lapic_id == 0);
            continue;
        }
        JARVIS_ASSERT(cpu_id == slot);
        bool listed = false;
        for (uint8_t i = 0; i < info.ncpus; ++i) {
            if (info.lapic_ids[i] == lapic_id) {
                listed = true;
                break;
            }
        }
        JARVIS_ASSERT(listed);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: This core executes on a valid address space: the live PML4
//           pointer is non-null.  (Per-core PML4 independence across APs
//           is not observable single-CPU — documented gap.)
// Input: VMM::current_pml4() on the running core.
// Expect: Non-zero root pointer.
// Depends: kernel::VMM boot mapping
JARVIS_TEST(core_pml4_valid, "PRE: none | POST: none") {
    JARVIS_ASSERT(VMM::current_pml4() != 0);
    JARVIS_TEST_PASS();
}

void register_core_isolation_tests() {
    Logger::info("Registering core isolation tests");
    JARVIS_REGISTER_TEST(core_slots_sized_isolated);
    JARVIS_REGISTER_TEST(core_bsp_owns_slot_zero);
    JARVIS_REGISTER_TEST(core_slot_write_isolated);
    JARVIS_REGISTER_TEST(core_ap_slots_owned_or_pristine);
    JARVIS_REGISTER_TEST(core_pml4_valid);
}
#endif  // CONFIG_ARCH_X86_64
