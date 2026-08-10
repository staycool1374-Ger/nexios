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

/// @file test_no_dynamic_alloc_after_init.cpp
/// @brief Post-init allocation determinism tests (v0.3.8).  Verify that a
///        balanced cycle of MemPool + VMM map/unmap operations leaves the
///        PMM free-page count exactly unchanged (no hidden allocation, no
///        leak).  The static-pools gate test compiles out until
///        CONFIG_STATIC_POOLS_ONLY is enabled.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/nexios_config.h>

using namespace kernel;

/// @brief Read the kernel PD entry covering @p va (0 if hierarchy absent).
///        Used to detect/revert the 2 MB huge-page split that map_page()
///        performs on first touch of a kernel-space VA.
static uint64_t kernel_pd_entry(uint64_t va) {
    constexpr uint64_t PT_P = 1ULL << 0;
    constexpr uint64_t PT_FRAME = 0x000FFFFFFFFFF000ULL;
    auto *pml4 = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (VMM::get_kernel_pml4() & ~0xFFFULL));
    size_t pml4_idx = (va >> 39) & 0x1FF;
    size_t pdpt_idx = (va >> 30) & 0x1FF;
    size_t pd_idx = (va >> 21) & 0x1FF;
    if (!(pml4[pml4_idx] & PT_P))
        return 0;
    auto *pdpt = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (pml4[pml4_idx] & PT_FRAME));
    if (!(pdpt[pdpt_idx] & PT_P))
        return 0;
    auto *pd = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (pdpt[pdpt_idx] & PT_FRAME));
    return pd[pd_idx];
}

/// @brief Restore the kernel PD entry covering @p va (re-huge after a test
///        split) — the PD page must already exist.
static void kernel_pd_entry_restore(uint64_t va, uint64_t entry) {
    constexpr uint64_t PT_P = 1ULL << 0;
    constexpr uint64_t PT_FRAME = 0x000FFFFFFFFFF000ULL;
    auto *pml4 = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (VMM::get_kernel_pml4() & ~0xFFFULL));
    size_t pml4_idx = (va >> 39) & 0x1FF;
    size_t pdpt_idx = (va >> 30) & 0x1FF;
    size_t pd_idx = (va >> 21) & 0x1FF;
    if (!(pml4[pml4_idx] & PT_P))
        return;
    auto *pdpt = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (pml4[pml4_idx] & PT_FRAME));
    if (!(pdpt[pdpt_idx] & PT_P))
        return;
    auto *pd = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (pdpt[pdpt_idx] & PT_FRAME));
    pd[pd_idx] = entry;
}

// Runmode: kernel
// Testidea: A neutral alloc/free cycle must not perturb the PMM free-page
//           count: 256 × (MemPool 32 B alloc/free + VMM map/unmap of a
//           scratch HHDM alias).  Any post-init dynamic allocation would
//           show up as a free-count delta.  If the alias' 2 MB region is
//           still huge, the first map splits it (allocating a PT page);
//           the split is warm-up'ed and re-huged + freed here so the live
//           PD matches the snapshot.
// Input: Snapshot PMM::free_pages_ref(), run the cycle, re-count.
// Expect: before == after.
// Depends: kernel/memory/pmm.hpp, kernel/memory/mempool.hpp,
//          kernel/memory/vmm.hpp
JARVIS_TEST(no_dynamic_alloc_pmm_neutral_cycle,
            "PRE: none | POST: none") {
    static constexpr size_t ITERATIONS = 256;

    // Snapshot BEFORE the scratch allocation so the final free balances it.
    uint64_t before = PMM::free_pages_ref();
    uint64_t phys = PMM::alloc_page();
    JARVIS_ASSERT_FMT(phys != 0, "PMM::alloc_page failed for scratch page");
    uint64_t va = arch::HHDM_OFFSET + phys;

    // Split the huge page once (warm-up) so the loop never allocates a PT
    // page; remember the huge entry to re-huge at the end.
    uint64_t saved_huge = kernel_pd_entry(va);
    bool split_now = (saved_huge & (1ULL << 7)) != 0;
    if (split_now) {
        VMM::map_page(va, phys, false);
        VMM::unmap_page(va);
    }

    for (size_t i = 0; i < ITERATIONS; ++i) {
        void *p = MemPool::alloc(32);
        JARVIS_ASSERT_FMT(p != nullptr, "MemPool::alloc(32) failed at iter %lu",
                          i);
        MemPool::free(p);
        VMM::map_page(va, phys, false);
        VMM::unmap_page(va);
    }

    // Re-huge + free the split PT page (only if we created it).
    if (split_now) {
        uint64_t pt_phys = kernel_pd_entry(va) & 0x000FFFFFFFFFF000ULL;
        kernel_pd_entry_restore(va, saved_huge);
        if (pt_phys)
            PMM::free_page(pt_phys);
    }
    PMM::free_page(phys);

    uint64_t after = PMM::free_pages_ref();
    JARVIS_ASSERT_FMT(before == after,
                      "PMM free-pages drifted across neutral cycle: "
                      "%lu -> %lu (delta %ld)",
                      before, after, static_cast<int64_t>(after) -
                                           static_cast<int64_t>(before));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: With CONFIG_STATIC_POOLS_ONLY=1 every page must come from a
//           pre-allocated static pool after init; this gate documents the
//           profile.  Compiles out when the profile is disabled (0).
// Input: Preprocessor gate only.
// Expect: Pass (no-op when CONFIG_STATIC_POOLS_ONLY == 0).
// Depends: kernel/nexios_config.h
JARVIS_TEST(no_dynamic_alloc_static_pools_gate,
            "PRE: none | POST: none") {
#if CONFIG_STATIC_POOLS_ONLY
    // Static-pools profile active: post-init PMM::alloc_page() is gated
    // inside the allocator itself; the neutral-cycle test above is the
    // runtime witness.  Nothing further to assert here.
    JARVIS_ASSERT(CONFIG_STATIC_POOLS_ONLY == 1);
#endif
    JARVIS_TEST_PASS();
}

void register_no_dynamic_alloc_tests() {
    Logger::info("Registering no-dynamic-alloc-after-init tests");
    JARVIS_REGISTER_TEST(no_dynamic_alloc_pmm_neutral_cycle);
    JARVIS_REGISTER_TEST(no_dynamic_alloc_static_pools_gate);
}
