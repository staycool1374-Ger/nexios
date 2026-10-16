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

/// @file test_vmm_path.cpp
/// @brief VMM map/rollback segment tests (issue #347 Slice D): successful
///        transactional range map, validation-reject, skip-one-step
///        negative, registry math. Drives VMM::map_phys_range_try() with
///        the slot reset/capture under IrqGuard. The Rollback branch is
///        instrumented and validated by inspection only: the sole clean
///        trigger is a mid-range table-allocation failure, which needs a
///        full PMM drain; a drain-based fixture page-faulted the running
///        kernel (freed an in-use page), so it is not driven here. Filed
///        as a follow-up to add a safe deterministic partial-failure
///        injector.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/core/path_tracer.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/vmm_path.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/arch/page_table.hpp>

using namespace kernel;

namespace {

static void test_vmm_map_range_reject_segment() {
    // Misaligned VA → entry validation refuses → Reject ^ Done.
    bool reject_ok = false;
    {
        arch::IrqGuard guard{};
        path_reset();
        const errors::VmmError err = VMM::map_phys_range_try(
            arch::HHDM_OFFSET + 0x2000001ULL, 0x1000, 2, false,
            VMM::get_kernel_pml4(), false);
        reject_ok = (err == errors::VMM_ERR_INVALID_ADDR) &&
                    path_validate(k_vmm_map_reject_expected);
        path_reset();
    }
    JARVIS_ASSERT(reject_ok);
}

static void test_vmm_map_registry_math() {
    JARVIS_ASSERT_EQ(0x13ULL, k_vmm_map_success_expected);
    JARVIS_ASSERT_EQ(0x15ULL, k_vmm_map_rollback_expected);
    JARVIS_ASSERT_EQ(0x18ULL, k_vmm_map_reject_expected);
    // Success and rollback share Validate + Done; rollback and reject
    // share Done; the three outcomes are pairwise distinct.
    JARVIS_ASSERT_EQ(0x11ULL,
                     k_vmm_map_success_expected & k_vmm_map_rollback_expected);
    JARVIS_ASSERT_EQ(0x10ULL,
                     k_vmm_map_rollback_expected & k_vmm_map_reject_expected);
    JARVIS_ASSERT(k_vmm_map_success_expected != k_vmm_map_rollback_expected);
    JARVIS_ASSERT(k_vmm_map_success_expected != k_vmm_map_reject_expected);
}

#if defined(CONFIG_ARCH_X86_64)
// Scratch VA inside the 128 MiB boot HHDM huge-page window, distinct from
// other tests' scratch regions. Adjacent pages share one PD entry.
constexpr uint64_t kVmmScratchVa = arch::HHDM_OFFSET + 0x4000000ULL;
constexpr uint64_t kX86TableFrameMask = 0x000FFFFFFFFFF000ULL;

/// @brief PD page for @p va, plus the slot index into that PD page.
static uint64_t *scratch_pd_at(uint64_t va, size_t &out_idx) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (VMM::get_kernel_pml4() & kX86TableFrameMask));
    const size_t pml4_idx = arch::ArchPageTable::pml4_index(va);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pdpt = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (pml4[pml4_idx] & kX86TableFrameMask));
    const size_t pdpt_idx = arch::ArchPageTable::pdpt_index(va);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pd = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (pdpt[pdpt_idx] & kX86TableFrameMask));
    out_idx = arch::ArchPageTable::pd_index(va);
    return pd;
}

/// @brief PD page for the scratch VA, plus its slot in the parent PD.
static uint64_t *scratch_pd(size_t &out_idx) {
    return scratch_pd_at(kVmmScratchVa, out_idx);
}

static void test_vmm_map_range_success_segment() {
    const uint64_t phys = PMM::alloc_contiguous(2);
    JARVIS_ASSERT(phys != 0);

    size_t pd_idx = 0;
    uint64_t *pd = scratch_pd(pd_idx);
    const uint64_t boot_entry = pd[pd_idx];

    bool success_ok = false;
    {
        arch::IrqGuard guard{}; // issue #200: HHDM PD surgery is IRQ-critical
        path_reset();
        const errors::VmmError err = VMM::map_phys_range_try(
            kVmmScratchVa, phys, 2, false, VMM::get_kernel_pml4(), false);
        success_ok = (err == errors::VMM_ERR_OK) &&
                     path_validate(k_vmm_map_success_expected);
        const uint64_t v2p0 = VMM::virt_to_phys(kVmmScratchVa);
        const uint64_t v2p1 = VMM::virt_to_phys(kVmmScratchVa + 4096);
        success_ok = success_ok && (v2p0 == phys) &&
                     (v2p1 == phys + 4096);
        // Undo the split: restore the boot huge entry, free the PT page.
        VMM::unmap_page(kVmmScratchVa);
        VMM::unmap_page(kVmmScratchVa + 4096);
        const uint64_t split_pt = pd[pd_idx] & kX86TableFrameMask;
        pd[pd_idx] = boot_entry;
        arch::ArchPageTable::tlb_flush(kVmmScratchVa);
        PMM::free_page(split_pt);
        path_reset();
    }
    PMM::free_page(phys);
    PMM::free_page(phys + 4096);
    JARVIS_ASSERT(success_ok);
}

static void test_vmm_map_range_skip_detected() {
    const uint64_t phys = PMM::alloc_page();
    JARVIS_ASSERT(phys != 0);

    size_t pd_idx = 0;
    uint64_t *pd = scratch_pd(pd_idx);
    const uint64_t boot_entry = pd[pd_idx];

    bool skip_ok = true; // not applicable without the debug injection
    {
        arch::IrqGuard guard{};
#if defined(CONFIG_DEBUG)
        g_test_skip_vmm_step =
            static_cast<uint64_t>(to_underlying(VmmMapSteps::Commit));
#endif
        path_reset();
        [[maybe_unused]] const errors::VmmError err = VMM::map_phys_range_try(
            kVmmScratchVa, phys, 1, false, VMM::get_kernel_pml4(), false);
#if defined(CONFIG_DEBUG)
        skip_ok = (err == errors::VMM_ERR_OK) &&
                  !path_validate(k_vmm_map_success_expected) &&
                  path_validate(k_vmm_map_success_expected ^
                                static_cast<uint64_t>(
                                    to_underlying(VmmMapSteps::Commit)));
        g_test_skip_vmm_step = 0;
#endif
        VMM::unmap_page(kVmmScratchVa);
        const uint64_t split_pt = pd[pd_idx] & kX86TableFrameMask;
        pd[pd_idx] = boot_entry;
        arch::ArchPageTable::tlb_flush(kVmmScratchVa);
        PMM::free_page(split_pt);
        path_reset();
    }
    PMM::free_page(phys);
    JARVIS_ASSERT(skip_ok);
}

#endif // CONFIG_ARCH_X86_64

} // namespace

void register_vmm_path_tests() {
    JARVIS_REGISTER_TEST(vmm_map_range_reject_segment);
    JARVIS_REGISTER_TEST(vmm_map_registry_math);
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_REGISTER_TEST(vmm_map_range_success_segment);
    JARVIS_REGISTER_TEST(vmm_map_range_skip_detected);
#endif
}
