/*
 * NexIOS RTOS — SMP bring-up (Phase B1)
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

/// @file test_smp_madt.cpp
/// @brief ACPI MADT discovery tests (issue #25, Phase B1).
///        The parser reads boot-firmware physical memory, so the externally
///        drivable contract is the scan_madt() result envelope: fail-closed
///        absence on machines without a MADT, the BSP LAPIC ID present in
///        the enabled list on machines with one (default single-CPU QEMU
///        lists exactly the BSP; the smp2 variant lists two — asserted by
///        the smp_bringup class in Phase B4), and purity across calls.
///        The mb2-identity-mapping envelope is borrowed from
///        test_acpi_parse.cpp (same firmware-table reader discipline).

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/x86_64/madt.hpp>
#include <kernel/arch/x86_64/hal/apic.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/multiboot2.hpp>

using namespace kernel;

namespace {

/// @brief Re-establishes the boot identity mapping for the Multiboot2 info
///        structure in the ACTIVE page table (a test task runs on its own
///        private kernel-half PML4).  Same contract as the acpi_parse
///        helper: bounded by total_size, verified per page, droppable.
constexpr uint64_t MB2_MAP_CAP = 256ULL * 1024ULL;

bool ensure_mb2_readable(uint64_t *mapped_pages_out, size_t *count_out) {
    *count_out = 0;
    if (multiboot_magic != 0x36D76289 || multiboot_info_ptr == 0)
        return false;
    uint64_t active_pml4 = VMM::current_pml4();
    uint64_t base_page = multiboot_info_ptr & ~0xFFFULL;
    VMM::map_page_in_pml4(base_page, base_page, false, active_pml4);
    uint64_t pt = VMM::virt_to_phys_in_pml4(base_page, active_pml4);
    if (pt != base_page)
        return false;
    mapped_pages_out[(*count_out)++] = base_page;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *info = reinterpret_cast<const volatile Multiboot2Info *>(
        multiboot_info_ptr);
    uint64_t total_size = info->total_size;
    if (total_size == 0 || total_size > MB2_MAP_CAP)
        return false;
    uint64_t end_va = multiboot_info_ptr + total_size;
    uint64_t end_page = (end_va + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t page = base_page + 0x1000ULL; page < end_page;
         page += 0x1000ULL) {
        VMM::map_page_in_pml4(page, page, false, active_pml4);
        uint64_t check = VMM::virt_to_phys_in_pml4(page, active_pml4);
        if (check != page)
            return false;
        mapped_pages_out[(*count_out)++] = page;
    }
    return true;
}

void drop_mb2_mapping(const uint64_t *mapped_pages, size_t count) {
    if (count == 0)
        return;
    uint64_t active_pml4 = VMM::current_pml4();
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<volatile uint64_t *>(
        arch::HHDM_OFFSET + (active_pml4 & ~0xFFFULL));
    for (size_t i = 0; i < count; ++i) {
        uint64_t page = mapped_pages[i];
        size_t pml4_idx = (page >> 39) & 0x1FF;
        size_t pdpt_idx = (page >> 30) & 0x1FF;
        size_t pd_idx = (page >> 21) & 0x1FF;
        size_t pt_idx = (page >> 12) & 0x1FF;
        uint64_t pml4e = pml4[pml4_idx];
        if (!(pml4e & 1))
            continue;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pdpt = reinterpret_cast<volatile uint64_t *>(
            arch::HHDM_OFFSET + (pml4e & ~0xFFFULL));
        uint64_t pdpte = pdpt[pdpt_idx];
        if (!(pdpte & 1))
            continue;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pd = reinterpret_cast<volatile uint64_t *>(
            arch::HHDM_OFFSET + (pdpte & ~0xFFFULL));
        uint64_t pde = pd[pd_idx];
        if (!(pde & 1) || (pde & (1ULL << 7)))
            continue;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pt = reinterpret_cast<volatile uint64_t *>(
            arch::HHDM_OFFSET + (pde & ~0xFFFULL));
        pt[pt_idx] = 0;
        asm volatile("invlpg (%0)" : : "r"(page) : "memory");
    }
}

} // namespace

// Runmode: kernel
// Testidea: The MadtInfo contract is pure data with safe defaults: a
// default-constructed result reports not-found and NOT malformed, with
// zero CPUs and a zeroed ID list — the bring-up can act on the fields
// without extra gating (zero APs woken when !found).
// Input: Default-constructed acpi::MadtInfo.
// Expect: found == false, malformed == false, ncpus == 0, all IDs == 0.
// Depends: kernel::acpi, madt.hpp
JARVIS_TEST(smp_madt_info_default_contract, "PRE: iocd | POST: none") {
    acpi::MadtInfo info{};
    JARVIS_ASSERT(!info.found);
    JARVIS_ASSERT(!info.malformed);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                     static_cast<uint64_t>(info.ncpus));
    for (uint8_t i = 0; i < MADT_MAX_CPUS; ++i)
        JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                         static_cast<uint64_t>(info.lapic_ids[i]));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: scan_madt on the RUNNING machine lists the BSP: the MADT is
// present on QEMU (SeaBIOS provides ACPI), well-formed, and contains the
// BSP LAPIC ID among the enabled entries.  Disabled LAPIC entries are
// never listed (they must never be woken).
// Input: scan_madt() on the live firmware tables (default single-CPU
//        variant: exactly the BSP; smp2 variant: BSP + 1 AP).
// Expect: Mapping succeeds; found && !malformed; 1 <= ncpus <=
//         MADT_MAX_CPUS; APIC::lapic_id() present in lapic_ids[].
// Depends: kernel::acpi::scan_madt, APIC::lapic_id, VMM, multiboot2 globals
JARVIS_TEST(smp_madt_scan_finds_bsp, "PRE: iocd | POST: none") {
    uint64_t mapped[64] = {};
    size_t count = 0;
    bool readable = ensure_mb2_readable(mapped, &count);
    if (readable) {
        acpi::MadtInfo info = acpi::scan_madt();
        JARVIS_ASSERT(info.found);
        JARVIS_ASSERT(!info.malformed);
        JARVIS_ASSERT(info.ncpus >= 1);
        JARVIS_ASSERT(info.ncpus <= MADT_MAX_CPUS);
        uint32_t bsp = arch::APIC::lapic_id();
        bool bsp_listed = false;
        for (uint8_t i = 0; i < info.ncpus; ++i) {
            if (info.lapic_ids[i] == bsp) {
                bsp_listed = true;
                break;
            }
        }
        JARVIS_ASSERT(bsp_listed);
    }
    drop_mb2_mapping(mapped, count);
    JARVIS_ASSERT(readable);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: scan_madt is pure — two consecutive scans return identical
// results (no hidden state mutation; the firmware tables are static and
// the result carries no table pointers).
// Input: scan_madt() twice under the re-established boot-info mapping,
//        field-by-field comparison.
// Expect: found/malformed/ncpus and all recorded IDs identical.
// Depends: kernel::acpi::scan_madt, VMM, multiboot2 globals
JARVIS_TEST(smp_madt_scan_is_pure, "PRE: iocd | POST: none") {
    uint64_t mapped[64] = {};
    size_t count = 0;
    bool readable = ensure_mb2_readable(mapped, &count);
    if (readable) {
        acpi::MadtInfo first = acpi::scan_madt();
        acpi::MadtInfo second = acpi::scan_madt();
        JARVIS_ASSERT_EQ(first.found, second.found);
        JARVIS_ASSERT_EQ(first.malformed, second.malformed);
        JARVIS_ASSERT_EQ(static_cast<uint64_t>(first.ncpus),
                         static_cast<uint64_t>(second.ncpus));
        for (uint8_t i = 0; i < MADT_MAX_CPUS; ++i)
            JARVIS_ASSERT_EQ(
                static_cast<uint64_t>(first.lapic_ids[i]),
                static_cast<uint64_t>(second.lapic_ids[i]));
    }
    drop_mb2_mapping(mapped, count);
    JARVIS_ASSERT(readable);
    JARVIS_TEST_PASS();
}

void register_smp_madt_tests() {
    Logger::info("Registering smp madt tests");
    JARVIS_REGISTER_TEST(smp_madt_info_default_contract);
    JARVIS_REGISTER_TEST(smp_madt_scan_finds_bsp);
    JARVIS_REGISTER_TEST(smp_madt_scan_is_pure);
}
#endif // CONFIG_ARCH_X86_64
