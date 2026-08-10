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

/// @file test_wcet_memory.cpp
/// @brief WCET benchmarks for the memory subsystem (v0.3.8): MemPool
///        alloc/free and VMM map/unmap cycle counts via rdtsc.  These are
///        TF_BENCH tests — excluded from normal runs, included in the
///        `bench` class and the `all` suite.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>

using namespace kernel;

namespace {

struct CycleStats {
    uint64_t min;
    uint64_t avg;
    uint64_t max;
};

/// @brief Measure a function's cycle cost over @p iterations.
///        Returns min/avg/max.  fn() is called once per iteration.
template <typename F> CycleStats measure_cycles(size_t iterations, F fn) {
    CycleStats r = {~0ULL, 0, 0};
    for (size_t i = 0; i < iterations; ++i) {
        uint64_t t0 = arch::rdtsc();
        fn();
        uint64_t elapsed = arch::rdtsc() - t0;
        if (elapsed < r.min)
            r.min = elapsed;
        if (elapsed > r.max)
            r.max = elapsed;
        r.avg += elapsed;
    }
    r.avg = (iterations > 0) ? (r.avg / iterations) : 0;
    return r;
}

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
///        split) — the PD page must already exist.  The boot HHDM PD lives
///        at physical 0x5000 (PDPT[0]); restoring the exact saved huge entry
///        makes the live table match the snapshot.
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

} // anonymous namespace

// Runmode: kernel
// Testidea: Measure the fixed-size MemPool alloc/free cycle cost (C-class
//           benchmark, no dispatch).  The allocator must be deterministic:
//           a repeated alloc/free pair returns the same pool block.
// Input: 200 iterations of MemPool::alloc(32) + MemPool::free().
// Expect: max > 0; stats logged as [WCET] mempool_alloc_free.
// Depends: kernel/memory/mempool.hpp, arch::rdtsc
JARVIS_TEST(wcet_mempool_alloc_free, "PRE: none | POST: none") {
    static constexpr size_t ITERATIONS = 200;
    CycleStats r = measure_cycles(ITERATIONS, []() {
        void *p = MemPool::alloc(32);
        if (p)
            MemPool::free(p);
    });
    Logger::info("[WCET] mempool_alloc_free: min=%lu avg=%lu max=%lu", r.min,
                 r.avg, r.max);
    JARVIS_ASSERT_FMT(r.max > 0, "mempool alloc/free must take > 0 cycles");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Measure VMM::map_page/unmap_page cycle cost on a HHDM scratch
//           alias (same pattern as the snapshot guard pages).  The physical
//           page is allocated once, mapped/unmapped N times, and freed —
//           the PMM free-page count must be net-zero across the WHOLE test.
//           If the alias' 2 MB region is still a huge page, the first map
//           splits it (allocating a PT page unmap leaves in place); the
//           split is performed as a warm-up and then re-huged + freed here
//           so the live PD matches the snapshot (no tracker leak, no
//           dangling PT).
// Input: 200 iterations mapping HHDM_OFFSET+phys -> phys.
// Expect: max > 0 and PMM::free_pages_ref() unchanged over the test.
// Depends: kernel/memory/vmm.hpp, kernel/memory/pmm.hpp, arch::rdtsc
JARVIS_TEST(wcet_vmm_map_unmap, "PRE: none | POST: none") {
    static constexpr size_t ITERATIONS = 200;

    // Snapshot BEFORE the scratch allocation so the final free balances it.
    uint64_t free_before = PMM::free_pages_ref();
    uint64_t phys = PMM::alloc_page();
    JARVIS_ASSERT_FMT(phys != 0, "PMM::alloc_page failed for scratch page");
    uint64_t va = arch::HHDM_OFFSET + phys;

    // If the alias' 2 MB region is still huge, split it once (warm-up) so
    // the measured loop never allocates a PT page; remember the huge entry
    // to re-huge at the end.
    uint64_t saved_huge = kernel_pd_entry(va);
    bool split_now = (saved_huge & (1ULL << 7)) != 0;
    if (split_now) {
        VMM::map_page(va, phys, false);
        VMM::unmap_page(va);
    }

    CycleStats r = measure_cycles(ITERATIONS, [va, phys]() {
        VMM::map_page(va, phys, false);
        VMM::unmap_page(va);
    });

    // Re-huge + free the split PT page (only if we created it).
    if (split_now) {
        uint64_t pt_phys = kernel_pd_entry(va) & 0x000FFFFFFFFFF000ULL;
        kernel_pd_entry_restore(va, saved_huge);
        if (pt_phys)
            PMM::free_page(pt_phys);
    }
    PMM::free_page(phys);

    uint64_t free_after = PMM::free_pages_ref();
    Logger::info("[WCET] vmm_map_unmap: min=%lu avg=%lu max=%lu", r.min,
                 r.avg, r.max);
    JARVIS_ASSERT_FMT(r.max > 0, "vmm map/unmap must take > 0 cycles");
    JARVIS_ASSERT_FMT(free_before == free_after,
                      "PMM free-pages drift in vmm map/unmap: %lu -> %lu",
                      free_before, free_after);
    JARVIS_TEST_PASS();
}

void register_wcet_memory_tests() {
    Logger::info("Registering WCET memory benchmarks");
    JARVIS_REGISTER_TEST_FLAGS(wcet_mempool_alloc_free, kernel::test::TF_BENCH);
    JARVIS_REGISTER_TEST_FLAGS(wcet_vmm_map_unmap, kernel::test::TF_BENCH);
}
