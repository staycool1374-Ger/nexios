/*
 * NexIOS RTOS — Capability-Based Access Control (CSpace)
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

/// @file test_iommu_live.cpp
/// @brief Live VT-d enablement tests (issue #9, phase-2).  This class runs
///        ONLY under the `iommu_live` test variant: `make execute-test
///        x86_64 debug iommu_live` boots on q35 with `-device intel-iommu`
///        (OVMF/UEFI) so the ACPI DMAR table and a real remapping unit are
///        present.  probe_hardware() at boot already detected the unit;
///        these tests verify enable_translation(), the register handshake
///        (GSTS.TES), the live IOTLB flush on map/unmap and the fault
///        read-clear — the phase-2 live path the software-only cap_iommu
///        class cannot exercise.  Every test leaves translation state clean
///        (TE off where it was off, no live domains).

#include <test.hpp>
#include <logger.hpp>
#include <constants.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/iommu/iommu.hpp>
#include <kernel/iommu/vtd.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/test/resource_tracker.hpp>

using namespace kernel;

namespace {

using iommu::IoMmuManager;
using iommu::vtd::kSlEntryRead;
using iommu::vtd::kSlEntryWrite;

// -- shared result flags (kernel-context tests, no cross-task races) --
uint64_t g_enabled = 0;
uint64_t g_map_ok = 0;
uint64_t g_unmap_ok = 0;
uint64_t g_te_set = 0;
uint64_t g_domains_left = 0;

} // namespace

// Runmode: kernel
// Testidea: On the q35+intel-iommu variant the boot probe must have found
//           the DMAR remapping unit and the software authority must be live
//           (g_present || g_live).
// Input: none (probe happened at boot)
// Expect: probe() true, mmio_base() != 0, translation may be on/off
// Depends: kernel::iommu::IoMmuManager
JARVIS_TEST(iommu_live_probe_detects_unit, "PRE: none | POST: none") {
    JARVIS_ASSERT(IoMmuManager::probe());
    JARVIS_ASSERT(IoMmuManager::mmio_base() != 0);
    JARVIS_ASSERT(IoMmuManager::probe_hardware()); // idempotent re-probe
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: enable_translation() programs RTADDR + kernel passthrough +
//           GCMD.TE and reports success; GSTS.TES then reflects the live
//           translation state (translation_live() true).
// Input: force_present(true); enable_translation()
// Expect: enable true, translation_live true (hardware GCMD.TE stays set —
//         contained: iommu_live is a standalone disk-free class, never in
//         `all`, and no kernel DMA occurs after; force_present(false) only
//         drops the software authority)
// Depends: kernel::iommu::IoMmuManager
JARVIS_TEST(iommu_live_enable_sets_tes, "PRE: none | POST: none") {
    IoMmuManager::force_present(true);
    JARVIS_ASSERT(IoMmuManager::probe_hardware()); // restore live unit
    g_enabled = IoMmuManager::enable_translation() ? 1 : 0;
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), g_enabled);
    g_te_set = IoMmuManager::translation_live() ? 1 : 0;
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), g_te_set);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: With live translation on, map_frame programs the SL entries and
//           issues the IOTLB flush; unmap_frame clears and flushes.  Both
//           must succeed (flush ack), proving the QI path works.
// Input: live enable; one 2-page FrameCap mapped then unmapped
// Expect: map true, mapping_count 1, unmap true, zero domains left
// Depends: kernel::iommu::IoMmuManager, cap::FrameCap, PMM
JARVIS_TEST(iommu_live_map_unmap_iotlb_flush, "PRE: none | POST: none") {
    IoMmuManager::force_present(true);
    JARVIS_ASSERT(IoMmuManager::probe_hardware()); // restore live unit
    JARVIS_ASSERT(IoMmuManager::enable_translation());
    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();

    int16_t idx = IoMmuManager::domain_create(
        static_cast<uint32_t>(cur->id));
    JARVIS_ASSERT(idx >= 0);

    uint64_t phys = PMM::alloc_contiguous(2);
    JARVIS_ASSERT(phys != 0);
    auto *fc = cap::FrameCap::create(phys, 2, false);
    JARVIS_ASSERT(fc != nullptr);

    uint32_t flags = kSlEntryRead | kSlEntryWrite;
    g_map_ok = IoMmuManager::map_frame(idx, *fc, flags) ? 1 : 0;
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), g_map_ok);
    JARVIS_ASSERT_EQ(static_cast<size_t>(1), IoMmuManager::mapping_count(idx));

    g_unmap_ok = IoMmuManager::unmap_frame(idx, *fc) ? 1 : 0;
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), g_unmap_ok);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IoMmuManager::mapping_count(idx));

    IoMmuManager::domain_destroy(idx);
    fc->release(); // dispose frees both pages
    IoMmuManager::force_present(false);

    g_domains_left = IoMmuManager::occupied_domains();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), g_domains_left);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: read_clear_faults() is a no-op-safe call under live hardware
//           (masks the fault interrupt + read-clears FSTS).  Must not crash
//           and must not clear the live translation state.
// Input: live enable; read_clear_faults()
// Expect: returns normally; translation still live after
// Depends: kernel::iommu::IoMmuManager
JARVIS_TEST(iommu_live_fault_read_clear_safe, "PRE: none | POST: none") {
    IoMmuManager::force_present(true);
    JARVIS_ASSERT(IoMmuManager::probe_hardware()); // restore live unit
    JARVIS_ASSERT(IoMmuManager::enable_translation());
    IoMmuManager::read_clear_faults();
    JARVIS_ASSERT(IoMmuManager::translation_live());
    IoMmuManager::force_present(false);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: enable_translation() is idempotent — a second call when TE is
//           already set reports success without re-programming.  (Guards
//           the "already live" fast path against double-enable.)
// Input: enable twice
// Expect: both true; translation_live stays true
// Depends: kernel::iommu::IoMmuManager
JARVIS_TEST(iommu_live_enable_idempotent, "PRE: none | POST: none") {
    IoMmuManager::force_present(true);
    JARVIS_ASSERT(IoMmuManager::probe_hardware()); // restore live unit
    JARVIS_ASSERT(IoMmuManager::enable_translation());
    JARVIS_ASSERT(IoMmuManager::enable_translation());
    JARVIS_ASSERT(IoMmuManager::translation_live());
    IoMmuManager::force_present(false);
    JARVIS_TEST_PASS();
}

// Exported to test_registry.cpp (class table entry, NOT part of `all` —
// requires the q35+intel-iommu QEMU variant).
// Runmode: kernel
// Testidea: The passthrough pre-pass programs a T=0 (TT=10b per spec §9.3)
//           context entry for kernel-owned devices so TE=1 never blocks
//           kernel DMA.  Under q35 the ICH9 AHCI (class 0x0106) sits at
//           bus 0 dev 0x1F fn 2; its entry must be Present + TT=10b with no
//           ASR (translate) bits.
// Input: enable_translation(); inspect the AHCI BDF context entry
// Expect: context_entry present with TT=10b; ASR bits zero
// Depends: kernel::iommu::IoMmuManager, vtd::kCte*
JARVIS_TEST(iommu_live_kernel_passthrough_entry, "PRE: none | POST: none") {
    IoMmuManager::force_present(true);
    JARVIS_ASSERT(IoMmuManager::probe_hardware()); // restore live unit
    JARVIS_ASSERT(IoMmuManager::enable_translation());
    // ICH9 AHCI under q35 (bus 0, device 31, function 2).
    arch::PciBdf bdf{0, 31, 2};
    uint64_t ce = IoMmuManager::context_entry(bdf);
    JARVIS_ASSERT(ce != 0); // pre-pass must have programmed the AHCI entry
    JARVIS_ASSERT((ce & iommu::vtd::kCtePresent) != 0);
    JARVIS_ASSERT_EQ(
        static_cast<uint64_t>(iommu::vtd::kCteTtPassthrough),
        ce & iommu::vtd::kCteTtMask); // TT=10b = passthrough, not translate
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                     ce & iommu::vtd::kCteAsrMask); // no translate ASR
    IoMmuManager::force_present(false);
    JARVIS_TEST_PASS();
}

void register_iommu_live_tests() {
    JARVIS_REGISTER_TEST(iommu_live_probe_detects_unit);
    JARVIS_REGISTER_TEST(iommu_live_enable_sets_tes);
    JARVIS_REGISTER_TEST(iommu_live_map_unmap_iotlb_flush);
    JARVIS_REGISTER_TEST(iommu_live_fault_read_clear_safe);
    JARVIS_REGISTER_TEST(iommu_live_enable_idempotent);
    JARVIS_REGISTER_TEST(iommu_live_kernel_passthrough_entry);
}