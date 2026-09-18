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
// Maximum tag-structure pages recorded (issue #180: hard bound so the
// table cannot overflow on corrupt total_size).
constexpr size_t MB2_MAP_MAX_PAGES = 64;

/// @brief Drops the identity mapping re-established by ensure_mb2_readable
/// (same active-PML4 walk the mapping used).
void drop_mb2_mapping(const uint64_t *mapped_pages, size_t count);

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
        // Issue #180: bound the table and unwind on failure — the old
        // code leaked already-mapped pages on the check-failure path.
        if (*count_out >= MB2_MAP_MAX_PAGES) {
            drop_mb2_mapping(mapped_pages_out, *count_out);
            *count_out = 0;
            return false;
        }
        VMM::map_page_in_pml4(page, page, false, active_pml4);
        uint64_t check = VMM::virt_to_phys_in_pml4(page, active_pml4);
        if (check != page) {
            drop_mb2_mapping(mapped_pages_out, *count_out);
            *count_out = 0;
            return false;
        }
        mapped_pages_out[(*count_out)++] = page;
    }
    return true;
}

/// @brief Drops the identity mapping re-established by ensure_mb2_readable
/// (same active-PML4 walk the mapping used).
/// @note Issue #180: uses the canonical VMM::unmap_page_in_pml4 (leaf
///       clear + TLB flush). First-touch huge-page splits stay in place by
///       design — the active table is shared across tests in the class, so
///       freeing split tables explicitly could reuse a live page-table
///       page; snapshot_restore rewinds PMM + PD state at the boundary.
void drop_mb2_mapping(const uint64_t *mapped_pages, size_t count) {
    if (count == 0)
        return;
    uint64_t active_pml4 = VMM::current_pml4();
    for (size_t i = 0; i < count; ++i)
        VMM::unmap_page_in_pml4(mapped_pages[i], active_pml4);
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

// Runmode: kernel
// Testidea: mb2_find_tag is bounded on corrupt tag streams (issue #180):
//           a non-terminal tag reporting size 0 used to stall the walk
//           forever (`addr += (0+7)&~7` advances 0) with no panic text.
// Input: Crafted static tag stream [type=1,size=0][type=0,size=8] under
//        saved/restored live multiboot globals.
// Expect: Lookup of the absent type returns 0 promptly; lookup of the
//         present type still finds it (no over-rejection).
// Depends: kernel::mb2_find_tag
JARVIS_TEST(mb2_tag_zero_size_bounded, "PRE: none | POST: none") {
    uint64_t saved_magic = multiboot_magic;
    uint64_t saved_ptr = multiboot_info_ptr;
    alignas(8) static uint32_t fake_words[6] = {24, 0, 1, 0, 0, 8};
    multiboot_magic = 0x36D76289;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    multiboot_info_ptr = reinterpret_cast<uint64_t>(&fake_words[0]);
    // Zero-size lookup first: pre-fix this never returns (hangs the class).
    uint64_t hit_zero = mb2_find_tag(15);
    uint64_t hit_valid = mb2_find_tag(1);
    multiboot_magic = saved_magic;
    multiboot_info_ptr = saved_ptr;
    JARVIS_ASSERT_EQ(0ULL, hit_zero);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    JARVIS_ASSERT_EQ(reinterpret_cast<uint64_t>(&fake_words[0]) + 8,
                     hit_valid);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Repeated ensure/scan/drop cycles are stable (issue #180):
//           exercises the exact failing shape (back-to-back scans under a
//           re-established mapping) several times in one test.
// Input: ensure_mb2_readable once, scan_dmar() 4x with field comparison,
//        drop_mb2_mapping.
// Expect: readable every time; all scans agree field-by-field.
// Depends: kernel::iommu::acpi::scan_dmar
JARVIS_TEST(acpi_scan_repeat_stable, "PRE: iocd | POST: none") {
    uint64_t mapped[64] = {};
    size_t count = 0;
    bool readable = ensure_mb2_readable(mapped, &count);
    if (readable) {
        iommu::dmar::DmarInfo first = iommu::acpi::scan_dmar();
        for (size_t idx = 0; idx < 3; ++idx) {
            iommu::dmar::DmarInfo cur = iommu::acpi::scan_dmar();
            JARVIS_ASSERT_EQ(first.found, cur.found);
            JARVIS_ASSERT_EQ(first.malformed, cur.malformed);
            JARVIS_ASSERT_EQ(first.unit.present, cur.unit.present);
            JARVIS_ASSERT_EQ(first.unit.base_phys, cur.unit.base_phys);
            JARVIS_ASSERT_EQ(first.unit.segment, cur.unit.segment);
            JARVIS_ASSERT_EQ(first.unit.include_pci_all,
                             cur.unit.include_pci_all);
        }
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
    JARVIS_REGISTER_TEST(mb2_tag_zero_size_bounded);
    JARVIS_REGISTER_TEST(acpi_scan_repeat_stable);
}
#endif // CONFIG_ARCH_X86_64
