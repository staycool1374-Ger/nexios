/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as
 * published by the Free Software Foundation.
 */

/// @file test_acpi_parse.cpp
/// @brief ACPI/DMAR discovery tests (milestone v0.4.3 issue #113).
///        The parser internals (RSDP/RSDT/XSDT walkers) live in an
///        anonymous namespace and read boot-firmware physical memory, so
///        the externally drivable contract is the scan_dmar() result
///        envelope: fail-closed absence on machines without a DMAR,
///        sane unit geometry on machines with one (q35 iommu_live
///        variant — the DMAR-positive assertions additionally live in
///        test_iommu_live.cpp), purity across calls, and the DmarInfo
///        default-initialisation contract.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/x86_64/acpi.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/multiboot2.hpp>

using namespace kernel;

namespace {

/// @brief The scan_dmar() parser walks the Multiboot2 info structure via
/// raw physical VAs — a mapping that exists at boot (boot identity map)
/// but is NOT part of the restored test-context page tables.  This helper
/// re-establishes the identity mapping for the info structure (bounded by
/// its own total_size, capped), verifies every page landed, and returns
/// true when the structure is readable again.  Pages are recorded so the
/// caller can drop the mapping afterwards.
constexpr uint64_t MB2_MAP_CAP = 256ULL * 1024ULL;

bool ensure_mb2_readable(uint64_t *mapped_pages_out, size_t *count_out) {
    *count_out = 0;
    if (multiboot_magic != 0x36D76289 || multiboot_info_ptr == 0)
        return false;
    // Map into the ACTIVE page table (a test task runs on its own private
    // kernel-half PML4 — the boot-time kernel_pml4_ target of map_page is
    // not the table the CPU is walking here).
    uint64_t active_pml4 = VMM::current_pml4();
    uint64_t base_page = multiboot_info_ptr & ~0xFFFULL;
    VMM::map_page_in_pml4(base_page, base_page, false, active_pml4);
    uint64_t pt = VMM::virt_to_phys_in_pml4(base_page, active_pml4);
    if (pt != base_page)
        return false; // mapping did not take — leave nothing behind
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

/// @brief Drops the identity mapping re-established by ensure_mb2_readable
/// (same active-PML4 walk the mapping used).
void drop_mb2_mapping(const uint64_t *mapped_pages, size_t count) {
    if (count == 0)
        return;
    uint64_t active_pml4 = VMM::current_pml4();
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 =
        reinterpret_cast<volatile uint64_t *>(arch::HHDM_OFFSET +
                                             (active_pml4 & ~0xFFFULL));
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
            continue; // not present or 2 MiB leaf — nothing we mapped
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *pt = reinterpret_cast<volatile uint64_t *>(
            arch::HHDM_OFFSET + (pde & ~0xFFFULL));
        pt[pt_idx] = 0;
        asm volatile("invlpg (%0)" : : "r"(page) : "memory");
    }
}

} // namespace

// Runmode: kernel
// Testidea: The DmarInfo contract is pure data with safe defaults: a
// default-constructed result reports not-found and NOT malformed, with a
// zeroed unit (no base, no segment, no include-pci-all) — any consumer
// can act on the fields without extra gating.
// Input: Default-constructed dmar::DmarInfo.
// Expect: found == false, malformed == false, unit.present == false,
//         unit.base_phys == 0, unit.segment == 0,
//         unit.include_pci_all == false.
// Depends: kernel::iommu::acpi, dmar.hpp
JARVIS_TEST(acpi_parse_info_default_contract, "PRE: iocd | POST: none") {
    iommu::dmar::DmarInfo info{};
    JARVIS_ASSERT(!info.found);
    JARVIS_ASSERT(!info.malformed);
    JARVIS_ASSERT(!info.unit.present);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), info.unit.base_phys);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                     static_cast<uint64_t>(info.unit.segment));
    JARVIS_ASSERT(!info.unit.include_pci_all);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: scan_dmar on the RUNNING machine is fail-closed: absence of a
// DMAR is reported as not-found WITHOUT the malformed flag (only a
// present-but-broken table may set malformed).  The parser reads the
// Multiboot2 info via raw physical VAs, so the test first re-establishes
// the boot identity mapping for the info structure (bounded by its own
// total_size) and drops it afterwards — boot firmware data must never be
// dereferenced through an unverified mapping.
// Input: scan_dmar() on the live firmware tables (pc: ACPI tags present,
//        no DMAR table; q35 iommu_live variant: real DMAR).
// Expect: Mapping of the info structure succeeds; if found: base_phys !=
//         0 && page-aligned && unit.present && !malformed.  If !found:
//         !malformed.  Identity mapping removed afterwards.
// Depends: kernel::iommu::acpi::scan_dmar, VMM, multiboot2 globals
JARVIS_TEST(acpi_parse_scan_fail_closed, "PRE: iocd | POST: none") {
    uint64_t mapped[64] = {};
    size_t count = 0;
    bool readable = ensure_mb2_readable(mapped, &count);
    if (readable) {
        iommu::dmar::DmarInfo info = iommu::acpi::scan_dmar();
        if (info.found) {
            bool aligned = (info.unit.base_phys & 0xFFFULL) == 0;
            JARVIS_ASSERT(info.unit.base_phys != 0);
            JARVIS_ASSERT(aligned);
            JARVIS_ASSERT(info.unit.present);
            JARVIS_ASSERT(!info.malformed);
        } else {
            JARVIS_ASSERT(!info.malformed);
        }
    }
    drop_mb2_mapping(mapped, count);
    JARVIS_ASSERT(readable);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: scan_dmar is pure — two consecutive scans return identical
// results (no hidden state mutation; the firmware tables are static).
// Input: scan_dmar() twice under the re-established boot-info mapping,
//        field-by-field comparison.
// Expect: found/malformed and all unit fields identical.
// Depends: kernel::iommu::acpi::scan_dmar, VMM, multiboot2 globals
JARVIS_TEST(acpi_parse_scan_is_pure, "PRE: iocd | POST: none") {
    uint64_t mapped[64] = {};
    size_t count = 0;
    bool readable = ensure_mb2_readable(mapped, &count);
    if (readable) {
        iommu::dmar::DmarInfo first = iommu::acpi::scan_dmar();
        iommu::dmar::DmarInfo second = iommu::acpi::scan_dmar();
        JARVIS_ASSERT_EQ(first.found, second.found);
        JARVIS_ASSERT_EQ(first.malformed, second.malformed);
        JARVIS_ASSERT_EQ(first.unit.present, second.unit.present);
        JARVIS_ASSERT_EQ(first.unit.base_phys, second.unit.base_phys);
        JARVIS_ASSERT_EQ(first.unit.segment, second.unit.segment);
        JARVIS_ASSERT_EQ(first.unit.include_pci_all,
                         second.unit.include_pci_all);
    }
    drop_mb2_mapping(mapped, count);
    JARVIS_ASSERT(readable);
    JARVIS_TEST_PASS();
}

void register_acpi_parse_tests() {
    Logger::info("Registering acpi parse tests");
    JARVIS_REGISTER_TEST(acpi_parse_info_default_contract);
    JARVIS_REGISTER_TEST(acpi_parse_scan_fail_closed);
    JARVIS_REGISTER_TEST(acpi_parse_scan_is_pure);
}
#endif // CONFIG_ARCH_X86_64
