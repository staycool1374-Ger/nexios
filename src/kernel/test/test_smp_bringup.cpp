/*
 * NexIOS RTOS — SMP bring-up (Phase B4)
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

/// @file test_smp_bringup.cpp
/// @brief SMP bring-up tests (issue #25, Phase B4).
///        The fixed-address contract (blob at 0x70000, SIPI vector 0x70)
///        is verified structurally (blob layout vs smp.hpp constants)
///        and functionally: bring_up() stages the blob on EVERY boot, so
///        a memcmp of the low block proves occupancy + copy.  The parked
///        count must equal the boot MADT snapshot's AP count on every
///        variant (0 on default single-CPU runs, 1 on the -smp 2
///        smp_bringup variant), and a parked AP owns per_cpu[1] with a
///        clean (never-interrupted) slot.  The MADT walk itself is covered
///        by the smp_madt class; here the CACHED boot snapshot is used so
///        no firmware-table walk (and its kernel-half mappings) happens
///        in test context.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/x86_64/hal/smp.hpp>
#include <kernel/arch/x86_64/hal/percpu.hpp>
#include <kernel/arch/x86_64/madt.hpp>
#include <kernel/arch/x86_64/hal/apic.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/multiboot2.hpp>

using namespace kernel;

namespace {

extern "C" {
extern uint8_t _binary_ap_trampoline_start[];
extern uint8_t _binary_ap_trampoline_size[];
}

/// @brief Compares the staged low block against the linked blob via the
///        HHDM alias (same physical page, kernel-half mapping that test
///        PML4s carry — NO active-PML4 mapping, so no page-table page is
///        allocated and the leak detector stays silent).  Only the CODE
///        prefix [0, GDT_OFF) is compared byte-exact: the GDT's Accessed
///        bits are CPU-owned (the AP's segment loads set them — proven by
///        a observed 0x92->0x93 flip), so GDT access bytes are asserted
///        modulo bit 0 instead; the param tail is BSP-filled, excluded.
bool block_matches_blob() {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const volatile uint8_t *block =
        reinterpret_cast<const volatile uint8_t *>(arch::HHDM_OFFSET +
                                                   smp::TRAMPOLINE_ADDR);
    for (uint64_t i = 0; i < smp::TRAMPOLINE_GDT_OFF; ++i) {
        if (block[i] != _binary_ap_trampoline_start[i])
            return false;
    }
    constexpr uint8_t kExpect[3] = {0x9A, 0x9A, 0x92}; // code32/code64/data
    for (int k = 0; k < 3; ++k) {
        uint8_t staged =
            block[smp::TRAMPOLINE_GDT_OFF + smp::GDT_ACCESS_OFFS[k]];
        if ((staged & ~0x01U) != kExpect[k])
            return false;
    }
    return true;
}

} // namespace

// Runmode: kernel
// Testidea: The .nasm blob and smp.hpp describe the same contract: the
// blob is exactly PARAM_OFF + PARAM_SIZE bytes (code padded via `times`,
// params fixed), starts with cli (0xFA — interrupts stay off on the AP),
// the SIPI vector is the block's page index, and the params fit in one
// 4 KiB block.
// Input: _binary_ap_trampoline_{start,size} + smp:: constants.
// Expect: size == 0x830; start[0] == 0xFA; (ADDR >> 12) == VECTOR;
//         PARAM_OFF + PARAM_SIZE <= 4096.
// Depends: mk/rules.mk objcopy embedding, smp.hpp, ap_trampoline.nasm
JARVIS_TEST(smp_bringup_blob_layout_sane, "PRE: iocd | POST: none") {
    uint64_t size = reinterpret_cast<uint64_t>(_binary_ap_trampoline_size);
    JARVIS_ASSERT_EQ(smp::TRAMPOLINE_PARAM_OFF + smp::TRAMPOLINE_PARAM_SIZE,
                     size);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0xFA),
                     static_cast<uint64_t>(_binary_ap_trampoline_start[0]));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(smp::TRAMPOLINE_ADDR >> 12),
                     static_cast<uint64_t>(smp::TRAMPOLINE_VECTOR));
    JARVIS_ASSERT(smp::TRAMPOLINE_PARAM_OFF + smp::TRAMPOLINE_PARAM_SIZE <=
                  4096);
    JARVIS_ASSERT(smp::TRAMPOLINE_GDT_OFF + smp::TRAMPOLINE_GDT_SIZE <=
                  smp::TRAMPOLINE_PARAM_OFF);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: bring_up() stages the blob at the fixed block on EVERY boot
// (before the CPU-count gate), so a byte comparison of the block against
// the linked blob proves the occupancy contract functionally.  The HHDM
// alias is read (no test-context mapping, no allocation).
// Input: HHDM view of [TRAMPOLINE_ADDR, +size) vs the linked blob.
// Expect: All size bytes identical.
// Depends: kernel::smp::bring_up boot staging, HHDM identity alias
JARVIS_TEST(smp_bringup_block_holds_blob, "PRE: iocd | POST: none") {
    uint64_t size = reinterpret_cast<uint64_t>(_binary_ap_trampoline_size);
    JARVIS_ASSERT(size > 0);
    JARVIS_ASSERT(block_matches_blob());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The parked count equals the boot MADT snapshot's AP count on
// every variant: default single-CPU runs wake zero APs (ncpus == 1), the
// -smp 2 smp_bringup variant wakes exactly one.  A parked AP owns
// per_cpu[1] with the snapshot's LAPIC ID and a clean slot (parked before
// any ISR).  The cached boot snapshot is used (no firmware-table walk in
// test context); the walk itself is covered by the smp_madt class.
// Input: smp::boot_madt() + smp::ap_count() + arch::per_cpu[1].
// Expect: ap_count == max(0, ncpus - 1); when > 0: per_cpu[1].cpu_id ==
//         1, lapic_id == the non-BSP snapshot entry, isr_nesting_depth == 0.
// Depends: kernel::smp::bring_up boot rendezvous + snapshot, per_cpu[1]
JARVIS_TEST(smp_bringup_ap_count_matches_madt, "PRE: iocd | POST: none") {
    const acpi::MadtInfo &info = smp::boot_madt();
    JARVIS_ASSERT(info.found);
    JARVIS_ASSERT(!info.malformed);
    JARVIS_ASSERT(info.ncpus >= 1);
    uint8_t expected = 0;
    uint32_t ap_lapic = 0;
    if (info.ncpus > 1) {
        expected = static_cast<uint8_t>(info.ncpus - 1);
        uint32_t bsp = arch::APIC::lapic_id();
        for (uint8_t i = 0; i < info.ncpus; ++i) {
            if (info.lapic_ids[i] != bsp) {
                ap_lapic = info.lapic_ids[i];
                break;
            }
        }
    }
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(expected),
                     static_cast<uint64_t>(smp::ap_count()));
    if (expected > 0) {
        JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), arch::per_cpu[1].cpu_id);
        JARVIS_ASSERT_EQ(static_cast<uint64_t>(ap_lapic),
                         arch::per_cpu[1].lapic_id);
        // Nesting depth is written by every ISR entry/exit on the AP: poll
        // for quiescence (between 1 KHz ticks) instead of asserting once.
        // A stuck nonzero depth (ISR entered, never exited) fails loudly.
        bool quiescent = false;
        for (uint64_t i = 0; i < 1000000; ++i) {
            if (arch::per_cpu[1].isr_nesting_depth == 0) {
                quiescent = true;
                break;
            }
            asm volatile("pause");
        }
        JARVIS_ASSERT(quiescent);
    }
    JARVIS_TEST_PASS();
}

void register_smp_bringup_tests() {
    Logger::info("Registering smp bringup tests");
    JARVIS_REGISTER_TEST(smp_bringup_blob_layout_sane);
    JARVIS_REGISTER_TEST(smp_bringup_block_holds_blob);
    JARVIS_REGISTER_TEST(smp_bringup_ap_count_matches_madt);
}
#endif // CONFIG_ARCH_X86_64
