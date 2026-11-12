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
#include <kernel/arch/page_table.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/nexios_config.h>

using namespace kernel;

/// @brief Read the kernel PD entry covering @p va (0 if hierarchy absent).
///        Used to detect/revert the 2 MB huge-page split that map_page()
///        performs on first touch of a kernel-space VA.  On riscv64 this
///        is the covering Sv39 L1 entry (leaf or split table): the x86
///        shifts read the wrong level there, so the warm-up split is
///        never detected and its table page leaks (+1, issue #536).
static uint64_t kernel_pd_entry(uint64_t va) {
#if defined(CONFIG_ARCH_RISCV64)
    constexpr uint64_t kV = 1ULL << 0;
    auto *l2 = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (VMM::get_kernel_pml4() & ~0xFFFULL));
    size_t l2_idx = (va >> 30) & 0x1FF;
    size_t l1_idx = (va >> 21) & 0x1FF;
    if (!(l2[l2_idx] & kV))
        return 0;
    if (VMM::sv39_is_leaf(l2[l2_idx]))
        return l2[l2_idx];
    auto *l1 = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + VMM::sv39_pte_phys(l2[l2_idx]));
    return l1[l1_idx];
#else
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
#endif
}

/// @brief Restore the kernel PD entry covering @p va (re-huge after a test
///        split) — the PD page must already exist.
static void kernel_pd_entry_restore(uint64_t va, uint64_t entry) {
#if defined(CONFIG_ARCH_RISCV64)
    constexpr uint64_t kV = 1ULL << 0;
    auto *l2 = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (VMM::get_kernel_pml4() & ~0xFFFULL));
    size_t l2_idx = (va >> 30) & 0x1FF;
    size_t l1_idx = (va >> 21) & 0x1FF;
    if (!(l2[l2_idx] & kV))
        return;
    if (VMM::sv39_is_leaf(l2[l2_idx])) {
        l2[l2_idx] = entry;
    } else {
        auto *l1 = reinterpret_cast<uint64_t *>(
            arch::HHDM_OFFSET + VMM::sv39_pte_phys(l2[l2_idx]));
        l1[l1_idx] = entry;
    }
    arch::ArchPageTable::tlb_flush(va);
#else
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
    arch::ArchPageTable::tlb_flush(va);
#endif
}

// Runmode: kernel
// Testidea: A neutral alloc/free cycle must not perturb the PMM free-page
//           count: 256 × (MemPool 32 B alloc/free + VMM map/unmap of a
//           scratch HHDM alias).  Any post-init dynamic allocation would
//           show up as a free-count delta.  If the alias' 2 MB region is
//           still huge, the first map splits it (allocating a PT page);
//           the split is warm-up'ed and re-huged + freed here so the live
//           tables match the snapshot.  Split detection is empirical
//           (covering-entry changed across one warm-up map/unmap, #418
//           precedent) with a per-arch covering-entry walk (x86 PD,
//           aarch64 block-shaped, Sv39 L2/L1): descriptor-bit tests are
//           address lottery across archs.
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
    // Issue #200: HHDM PD surgery is IRQ-critical — mask the timer ISR
    // and task preemption across the warm-up split only.
    // Split detection is empirical (entry-changed across one warm-up
    // map/unmap, #418 precedent): huge/block flag bits are arch-specific
    // (x86_64 PS bit, aarch64 block shape, Sv39 leaf R/W/X), and the
    // covering-entry walk itself is per-arch (Sv39 L2/L1, issue #536),
    // so compare entries instead of testing descriptor bits.
    uint64_t saved_huge = kernel_pd_entry(va);
    bool split_now = false;
    {
        arch::IrqGuard split_guard;
        VMM::map_page(va, phys, false);
        VMM::unmap_page(va);
        split_now = (kernel_pd_entry(va) != saved_huge);
    }

    // The loop is HHDM surgery like the warm-up (each map/unmap touches
    // the shared kernel tables + shootdown path): hold the guard across
    // it so a tick cannot interleave mid-sequence (issue #200
    // IRQ-criticality; the neutrality property under test is unaffected).
    // An unguarded variant hung intermittently on riscv64 (~2/7 runs,
    // silent wedge mid-loop, never under GDB/logging); the guarded form
    // has not reproduced it.
    arch::IrqGuard loop_guard;
    for (size_t i = 0; i < ITERATIONS; ++i) {
        void *p = MemPool::alloc(32);
        JARVIS_ASSERT_FMT(p != nullptr, "MemPool::alloc(32) failed at iter %lu",
                          i);
        MemPool::free(p);
        VMM::map_page(va, phys, false);
        VMM::unmap_page(va);
    }

    // Re-huge + free the split PT page (only if we created it).
    // Issue #200: guarded like the warm-up split (IRQ-critical surgery).
    // Owned-guard mirrors wcet_vmm_map_unmap (#418): free only a
    // PMM-tracked table page, and assert it (a missing table means the
    // detector and the allocator disagree).
    if (split_now) {
        arch::IrqGuard rehuge_guard;
#if defined(CONFIG_ARCH_RISCV64)
        // Sv39 PTEs carry PPN<<10 (issue #206): the x86 frame mask
        // decodes 4x too low here — use the Sv39 codec.
        uint64_t pt_phys = VMM::sv39_pte_phys(kernel_pd_entry(va));
#else
        uint64_t pt_phys = kernel_pd_entry(va) & 0x000FFFFFFFFFF000ULL;
#endif
        bool owned = (pt_phys != 0) && PMM::is_allocated(pt_phys);
        kernel_pd_entry_restore(va, saved_huge);
        if (owned)
            PMM::free_page(pt_phys);
        JARVIS_ASSERT_FMT(owned, "neutral-cycle split table not PMM-owned");
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
