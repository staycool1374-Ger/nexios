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
///        negative, runtime rollback via the deterministic table-alloc
///        fault injector (issue #361), registry math. Drives
///        VMM::map_phys_range_try() with the slot reset/capture under
///        IrqGuard.

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
    // Expected values are fold-computed (never hand-derived).
    JARVIS_ASSERT_EQ(path_expected(VmmMapSteps::Validate, VmmMapSteps::Commit,
                                   VmmMapSteps::Done),
                     k_vmm_map_success_expected);
    JARVIS_ASSERT_EQ(path_expected(VmmMapSteps::Validate, VmmMapSteps::Rollback,
                                   VmmMapSteps::Done),
                     k_vmm_map_rollback_expected);
    JARVIS_ASSERT_EQ(
        path_expected(VmmMapSteps::Reject, VmmMapSteps::Done),
        k_vmm_map_reject_expected);
    // Pairwise distinct (order-sensitive fold).
    JARVIS_ASSERT(k_vmm_map_success_expected != k_vmm_map_rollback_expected);
    JARVIS_ASSERT(k_vmm_map_success_expected != k_vmm_map_reject_expected);
    JARVIS_ASSERT(k_vmm_map_rollback_expected != k_vmm_map_reject_expected);
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

#if defined(CONFIG_DEBUG)
/// @brief RAII injector arm (issue #361): disarms on scope exit, so an
///        assert-abort (JARVIS_ASSERT returns) cannot leak armed state.
struct VmmTableFailGuard {
    explicit VmmTableFailGuard(size_t fail_at) {
        VMM::test_arm_table_alloc_fail_at(fail_at);
    }
    ~VmmTableFailGuard() {
        VMM::test_disarm_table_alloc_fail();
    }
    VmmTableFailGuard(const VmmTableFailGuard &) = delete;
    VmmTableFailGuard &operator=(const VmmTableFailGuard &) = delete;
};
#endif

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
                  path_validate(path_expected(VmmMapSteps::Validate,
                                              VmmMapSteps::Done));
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

/// @brief Rollback positive test (issue #361): with the injector armed to
///        fail the 2nd table allocation, a 2-page range straddling a 2 MiB
///        boundary maps page 0 (split PT allocated) then fails page 1.
///        Expect VMM_ERR_PAGE_ALLOC + the Rollback chain, both VAs
///        unmapped, the second PD slot untouched, and zero PMM delta.
///        Debug-only; trivial pass elsewhere (count-row pin).
static void test_vmm_map_range_rollback_segment() {
    bool rollback_ok = true; // not applicable without the debug injection
#if defined(CONFIG_DEBUG)
    // va1 is 2 MiB-aligned (kVmmScratchVa already is); va0 is the last
    // page of the previous PD slot. Both PD entries are boot huge pages.
    const uint64_t va1 = kVmmScratchVa;
    const uint64_t va0 = va1 - arch::PAGE_SIZE;
    size_t idx0 = 0;
    uint64_t *pd0 = scratch_pd_at(va0, idx0);
    size_t idx1 = 0;
    uint64_t *pd1 = scratch_pd_at(va1, idx1);
    JARVIS_ASSERT(pd0 == pd1);
    JARVIS_ASSERT(idx1 == idx0 + 1);
    const uint64_t boot0 = pd0[idx0];
    const uint64_t boot1 = pd1[idx1];
    JARVIS_ASSERT(boot0 != 0);
    JARVIS_ASSERT(boot1 != 0);

    const uint64_t phys = PMM::alloc_contiguous(2);
    JARVIS_ASSERT(phys != 0);
    const uint64_t pmm_before = PMM::free_pages_ref();

    bool chain_ok = false;
    bool cleared_ok = false;
    bool prefix_ok = false;
    bool untouched_ok = false;
    {
        arch::IrqGuard guard{}; // issue #200: HHDM PD surgery is IRQ-critical
        VmmTableFailGuard fail_at_second(2); // alloc #1 (va0 PT) succeeds,
                                             // alloc #2 (va1 PT) fails
        path_reset();
        const errors::VmmError err = VMM::map_phys_range_try(
            va0, phys, 2, false, VMM::get_kernel_pml4(), false);
        chain_ok = (err == errors::VMM_ERR_PAGE_ALLOC) &&
                   path_validate(k_vmm_map_rollback_expected);
        // Only va0 can read clear: it was mapped then rolled back. va1
        // was never installed, so it still resolves through the
        // untouched boot huge entry (which untouched_ok pins below) —
        // asserting it clear would mistake the baseline mapping for a
        // rollback leak.
        cleared_ok = (VMM::virt_to_phys(va0) == 0);
        // The prefix really installed (else this test would be vacuous):
        // only then is there a split PT to restore and free.
        prefix_ok = (pd0[idx0] != boot0);
        if (prefix_ok) {
            const uint64_t split_pt = pd0[idx0] & kX86TableFrameMask;
            pd0[idx0] = boot0;
            arch::ArchPageTable::tlb_flush(va0);
            PMM::free_page(split_pt);
        }
        untouched_ok = (pd1[idx1] == boot1);
        path_reset();
    }
    PMM::free_page(phys);
    PMM::free_page(phys + arch::PAGE_SIZE);
    // pmm_before covers the 2 device frames; both are released above, so
    // the free count must land exactly 2 above the capture.
    rollback_ok = chain_ok && cleared_ok && prefix_ok && untouched_ok &&
                  (PMM::free_pages_ref() == pmm_before + 2);
#endif
    JARVIS_ASSERT(rollback_ok);
}

/// @brief Rollback fail-first test (issue #361): with the injector armed
///        to fail the 1st table allocation, the same 2-page cross-boundary
///        range installs nothing. Expect VMM_ERR_PAGE_ALLOC + the Rollback
///        chain, BOTH PD slots still the boot huge entries (verified via
///        unchanged virt_to_phys on both VAs), and zero PMM delta.
///        Debug-only; trivial pass elsewhere (count-row pin).
static void test_vmm_map_range_rollback_first_fail() {
    bool rollback_ok = true; // not applicable without the debug injection
#if defined(CONFIG_DEBUG)
    const uint64_t va1 = kVmmScratchVa;
    const uint64_t va0 = va1 - arch::PAGE_SIZE;
    size_t idx0 = 0;
    uint64_t *pd0 = scratch_pd_at(va0, idx0);
    size_t idx1 = 0;
    uint64_t *pd1 = scratch_pd_at(va1, idx1);
    JARVIS_ASSERT(pd0 == pd1);
    JARVIS_ASSERT(idx1 == idx0 + 1);
    const uint64_t boot0 = pd0[idx0];
    const uint64_t boot1 = pd1[idx1];
    JARVIS_ASSERT(boot0 != 0);
    JARVIS_ASSERT(boot1 != 0);
    // Baseline translations through the boot huge entries: nothing will
    // be installed, so these must read back unchanged afterwards.
    const uint64_t base0 = VMM::virt_to_phys(va0);
    const uint64_t base1 = VMM::virt_to_phys(va1);
    JARVIS_ASSERT(base0 != 0);
    JARVIS_ASSERT(base1 != 0);

    const uint64_t phys = PMM::alloc_contiguous(2);
    JARVIS_ASSERT(phys != 0);
    const uint64_t pmm_before = PMM::free_pages_ref();

    bool chain_ok = false;
    bool untouched_ok = false;
    {
        arch::IrqGuard guard{}; // issue #200: HHDM PD surgery is IRQ-critical
        VmmTableFailGuard fail_at_first(1); // alloc #1 (va0 PT) fails:
                                            // nothing installs at all
        path_reset();
        const errors::VmmError err = VMM::map_phys_range_try(
            va0, phys, 2, false, VMM::get_kernel_pml4(), false);
        chain_ok = (err == errors::VMM_ERR_PAGE_ALLOC) &&
                   path_validate(k_vmm_map_rollback_expected);
        untouched_ok = (pd0[idx0] == boot0) && (pd1[idx1] == boot1) &&
                       (VMM::virt_to_phys(va0) == base0) &&
                       (VMM::virt_to_phys(va1) == base1);
        path_reset();
    }
    PMM::free_page(phys);
    PMM::free_page(phys + arch::PAGE_SIZE);
    rollback_ok = chain_ok && untouched_ok &&
                  (PMM::free_pages_ref() == pmm_before + 2);
#endif
    JARVIS_ASSERT(rollback_ok);
}

/// @brief Rollback past-end test (issue #361): armed past the last needed
///        allocation (arm(3) for a 2-PT range), the mapping must succeed
///        exactly like the unarmed path — proving the injector is
///        transparent when it never fires. A repeat mapping after guard
///        scope exit proves the RAII disarm left no stuck-armed state.
///        Debug-only; trivial pass elsewhere (count-row pin).
static void test_vmm_map_range_rollback_past_end() {
    bool past_end_ok = true; // not applicable without the debug injection
#if defined(CONFIG_DEBUG)
    const uint64_t va1 = kVmmScratchVa;
    const uint64_t va0 = va1 - arch::PAGE_SIZE;
    size_t idx0 = 0;
    uint64_t *pd0 = scratch_pd_at(va0, idx0);
    size_t idx1 = 0;
    uint64_t *pd1 = scratch_pd_at(va1, idx1);
    JARVIS_ASSERT(pd0 == pd1);
    JARVIS_ASSERT(idx1 == idx0 + 1);
    const uint64_t boot0 = pd0[idx0];
    const uint64_t boot1 = pd1[idx1];
    JARVIS_ASSERT(boot0 != 0);
    JARVIS_ASSERT(boot1 != 0);

    const uint64_t phys = PMM::alloc_contiguous(2);
    JARVIS_ASSERT(phys != 0);
    const uint64_t pmm_before = PMM::free_pages_ref();

    bool success_ok = false;
    {
        arch::IrqGuard guard{}; // issue #200: HHDM PD surgery is IRQ-critical
        VmmTableFailGuard never_fires(3); // 2 allocs needed: success path
        path_reset();
        const errors::VmmError err = VMM::map_phys_range_try(
            va0, phys, 2, false, VMM::get_kernel_pml4(), false);
        success_ok = (err == errors::VMM_ERR_OK) &&
                     path_validate(k_vmm_map_success_expected) &&
                     (VMM::virt_to_phys(va0) == phys) &&
                     (VMM::virt_to_phys(va1) == phys + arch::PAGE_SIZE);
        // Teardown mirrors the success segment: unmap both, restore both
        // boot huge entries, free both split PT pages.
        VMM::unmap_page(va0);
        VMM::unmap_page(va1);
        const uint64_t split_pt0 = pd0[idx0] & kX86TableFrameMask;
        const uint64_t split_pt1 = pd1[idx1] & kX86TableFrameMask;
        pd0[idx0] = boot0;
        pd1[idx1] = boot1;
        arch::ArchPageTable::tlb_flush(va0);
        arch::ArchPageTable::tlb_flush(va1);
        PMM::free_page(split_pt0);
        PMM::free_page(split_pt1);
        path_reset();
    }
    PMM::free_page(phys);
    PMM::free_page(phys + arch::PAGE_SIZE);
    past_end_ok = success_ok && (PMM::free_pages_ref() == pmm_before + 2);
    // Guard scope exited: a repeat mapping must still succeed (no stuck
    // armed state). Fresh frames: the first pair is already freed and may
    // be reused elsewhere. Teardown identical to the success segment.
    if (past_end_ok) {
        const uint64_t phys2 = PMM::alloc_contiguous(2);
        bool repeat_ok = (phys2 != 0);
        if (repeat_ok) {
            arch::IrqGuard guard{};
            path_reset();
            const errors::VmmError err = VMM::map_phys_range_try(
                va0, phys2, 2, false, VMM::get_kernel_pml4(), false);
            repeat_ok = (err == errors::VMM_ERR_OK);
            VMM::unmap_page(va0);
            VMM::unmap_page(va1);
            const uint64_t split_pt0 = pd0[idx0] & kX86TableFrameMask;
            const uint64_t split_pt1 = pd1[idx1] & kX86TableFrameMask;
            pd0[idx0] = boot0;
            pd1[idx1] = boot1;
            arch::ArchPageTable::tlb_flush(va0);
            arch::ArchPageTable::tlb_flush(va1);
            PMM::free_page(split_pt0);
            PMM::free_page(split_pt1);
            path_reset();
        }
        if (phys2 != 0) {
            PMM::free_page(phys2);
            PMM::free_page(phys2 + arch::PAGE_SIZE);
        }
        past_end_ok = repeat_ok &&
                      (PMM::free_pages_ref() == pmm_before + 2);
    }
#endif
    JARVIS_ASSERT(past_end_ok);
}

#endif // CONFIG_ARCH_X86_64

} // namespace

void register_vmm_path_tests() {
    JARVIS_REGISTER_TEST(vmm_map_range_reject_segment);
    JARVIS_REGISTER_TEST(vmm_map_registry_math);
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_REGISTER_TEST(vmm_map_range_success_segment);
    JARVIS_REGISTER_TEST(vmm_map_range_skip_detected);
    JARVIS_REGISTER_TEST(vmm_map_range_rollback_segment);
    JARVIS_REGISTER_TEST(vmm_map_range_rollback_first_fail);
    JARVIS_REGISTER_TEST(vmm_map_range_rollback_past_end);
#endif
}
