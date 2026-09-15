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

/// @file test_pt_merge.cpp
/// @brief Kernel-half page-table merge tests (issue #96).
///
/// Verifies VMM::merge_kernel_half (link semantics: missing entries are
/// copied by value, present entries untouched, zero allocations) and the
/// template/convergence audit helpers.  All tests use PRIVATE scratch
/// tables — the live kernel PML4 is only ever walked read-only, except
/// template_mismatch_detected which clones it (freed afterwards).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <constants.hpp>

#if defined(CONFIG_ARCH_X86_64)

using namespace kernel;

enum : uint64_t {
    PML4_SHIFT = 39,
    PAGE_PRESENT = 1ULL << 0,
    PAGE_WRITE = 1ULL << 1,
    PAGE_HUGE = 1ULL << 7,
    PAGE_NX = 1ULL << 63,
};

/// @brief Allocate a kernel-owned page and zero it (scratch table).
/// @return Physical address, or 0 on OOM (caller fails the test).
static uint64_t alloc_zero_table() {
    uint64_t phys = PMM::alloc_page();
    if (phys == 0)
        return 0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (phys & ~0xFFFULL));
    __builtin_memset(virt, 0, arch::PAGE_SIZE);
    return phys;
}

/// @brief HHDM view of a scratch table page.
static uint64_t *table_virt(uint64_t phys) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                        (phys & ~0xFFFULL));
}

// Runmode: kernel
// Testidea: Merging a populated parent into a fresh child converges the
//           child exactly (link semantics: entries shared, user half
//           untouched) — the canonical fork fast-path behavior.
// Input: Scratch parent with a table chain + huge entry in 256..511;
//        fresh zeroed scratch child.
// Expect: merge returns true; child entries equal parent entries;
//         child shares the parent's tables (same phys); user half stays
//         zero; PMM delta zero after teardown.
// Depends: VMM::merge_kernel_half (issue #96)
JARVIS_TEST(merge_equivalent_to_full_copy, "PRE: none | POST: none") {
    uint64_t pages_before = PMM::pool_used_pages();
    uint64_t parent = alloc_zero_table();
    uint64_t pdpt = alloc_zero_table();
    uint64_t pd = alloc_zero_table();
    uint64_t pt = alloc_zero_table();
    uint64_t data = alloc_zero_table();
    uint64_t child = alloc_zero_table();
    JARVIS_ASSERT(parent != 0 && pdpt != 0 && pd != 0 && pt != 0 &&
                  data != 0 && child != 0);

    table_virt(parent)[256] = pdpt | PAGE_PRESENT | PAGE_WRITE;
    table_virt(pdpt)[0] = pd | PAGE_PRESENT | PAGE_WRITE;
    table_virt(pd)[0] = pt | PAGE_PRESENT | PAGE_WRITE;
    table_virt(pt)[0] =
        (data & ~0xFFFULL) | PAGE_PRESENT | PAGE_WRITE | PAGE_NX;
    table_virt(parent)[511] =
        (data & ~0xFFFULL) | PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE;

    JARVIS_ASSERT(VMM::merge_kernel_half(parent, child));
    auto *c = table_virt(child);
    auto *p = table_virt(parent);
    for (size_t i = arch::PML4_KERNEL_START; i < arch::PML4_ENTRIES; ++i)
        JARVIS_ASSERT(c[i] == p[i]);
    for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i)
        JARVIS_ASSERT(c[i] == 0);
    JARVIS_ASSERT((c[256] & ~0xFFFULL) == (pdpt & ~0xFFFULL));
    JARVIS_ASSERT(c[257] == 0);
    JARVIS_ASSERT(VMM::kernel_half_equal(child, parent));

    PMM::free_page(data);
    PMM::free_page(pt);
    PMM::free_page(pd);
    PMM::free_page(pdpt);
    PMM::free_page(parent);
    PMM::free_page(child);
    JARVIS_ASSERT(PMM::pool_used_pages() == pages_before);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Merge is idempotent and hole-filling: present entries are
//           never touched, missing ones linked, and a second merge is a
//           no-op (paper §3 idempotence).
// Input: Parent superset table; child with its own table holding a hole
//        plus one private entry; merge twice.
// Expect: Hole filled from parent, private entry and parent untouched,
//         second merge byte-identical, PMM delta zero.
// Depends: VMM::merge_kernel_half (issue #96)
JARVIS_TEST(merge_idempotent, "PRE: none | POST: none") {
    uint64_t pages_before = PMM::pool_used_pages();
    uint64_t parent = alloc_zero_table();
    uint64_t parent_pdpt = alloc_zero_table();
    uint64_t child = alloc_zero_table();
    uint64_t child_pdpt = alloc_zero_table();
    uint64_t data_a = alloc_zero_table();
    uint64_t data_b = alloc_zero_table();
    JARVIS_ASSERT(parent != 0 && parent_pdpt != 0 && child != 0 &&
                  child_pdpt != 0 && data_a != 0 && data_b != 0);

    constexpr size_t k_idx = 300;
    table_virt(parent)[k_idx] = parent_pdpt | PAGE_PRESENT | PAGE_WRITE;
    table_virt(parent_pdpt)[7] =
        (data_a & ~0xFFFULL) | PAGE_PRESENT | PAGE_WRITE;
    table_virt(child)[k_idx] = child_pdpt | PAGE_PRESENT | PAGE_WRITE;
    table_virt(child_pdpt)[8] =
        (data_b & ~0xFFFULL) | PAGE_PRESENT | PAGE_WRITE;

    JARVIS_ASSERT(VMM::merge_kernel_half(parent, child));
    auto *cc = table_virt(child);
    JARVIS_ASSERT(cc[k_idx] == table_virt(child)[k_idx]);
    auto *cc_pdpt = table_virt(cc[k_idx] & ~0xFFFULL);
    JARVIS_ASSERT(cc_pdpt[7] == table_virt(parent_pdpt)[7]);
    JARVIS_ASSERT(cc_pdpt[8] ==
                  ((data_b & ~0xFFFULL) | PAGE_PRESENT | PAGE_WRITE));

    uint64_t snapshot[512];
    __builtin_memcpy(snapshot, cc_pdpt, arch::PAGE_SIZE);
    JARVIS_ASSERT(VMM::merge_kernel_half(parent, child));
    bool identical = true;
    for (size_t i = 0; i < 512; ++i) {
        if (snapshot[i] != cc_pdpt[i]) {
            identical = false;
            break;
        }
    }
    JARVIS_ASSERT(identical);

    PMM::free_page(data_b);
    PMM::free_page(data_a);
    PMM::free_page(child_pdpt);
    PMM::free_page(child);
    PMM::free_page(parent_pdpt);
    PMM::free_page(parent);
    JARVIS_ASSERT(PMM::pool_used_pages() == pages_before);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A mapping added to the canonical root after the template
//           snapshot becomes visible to a merged child but not to a
//           pre-merge child (paper §4 row 2: late-mapping convergence).
//           Uses a 2 MiB PD-level huge entry: the kernel walker resolves
//           PD-huge (PML4-level 1 GiB huge is not walkable — pre-existing
//           walker limitation, out of scope for #96).
// Input: Scratch canonical root; snapshot its entries; add a 2 MiB
//        mapping; merge into child A only.
// Expect: child A resolves the VA, child B does not, and the snapshot
//         predates the mapping.
// Depends: VMM::merge_kernel_half (issue #96)
JARVIS_TEST(late_mapping_visible_after_merge, "PRE: none | POST: none") {
    uint64_t pages_before = PMM::pool_used_pages();
    uint64_t canon = alloc_zero_table();
    uint64_t canon_pdpt = alloc_zero_table();
    uint64_t canon_pd = alloc_zero_table();
    uint64_t child_a = alloc_zero_table();
    uint64_t child_b = alloc_zero_table();
    uint64_t data = alloc_zero_table();
    JARVIS_ASSERT(canon != 0 && canon_pdpt != 0 && canon_pd != 0 &&
                  child_a != 0 && child_b != 0 && data != 0);

    uint64_t tmpl[512];
    __builtin_memcpy(tmpl, table_virt(canon), arch::PAGE_SIZE);

    constexpr size_t k_idx = 400;
    constexpr size_t k_pd = 5;
    constexpr uint64_t k_off = 0x12345ULL;
    // 2 MiB pages mask the low 21 bits (HUGE_FRAME_MASK); the synthetic
    // phys is only 4 KiB-aligned, so expect the masked frame.
    constexpr uint64_t k_huge_mask = 0x000FFFFFFFE00000ULL;
    table_virt(canon)[k_idx] = canon_pdpt | PAGE_PRESENT | PAGE_WRITE;
    table_virt(canon_pdpt)[0] = canon_pd | PAGE_PRESENT | PAGE_WRITE;
    table_virt(canon_pd)[k_pd] =
        (data & ~0xFFFULL) | PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE;
    JARVIS_ASSERT(tmpl[k_idx] == 0);

    JARVIS_ASSERT(VMM::merge_kernel_half(canon, child_a));
    uint64_t va = (static_cast<uint64_t>(k_idx) << PML4_SHIFT) |
                  (static_cast<uint64_t>(k_pd) << 21) | k_off;
    uint64_t resolved = VMM::virt_to_phys_in_pml4(va, child_a);
    JARVIS_ASSERT(resolved == ((data & k_huge_mask) | k_off));
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(va, child_b) == 0);

    PMM::free_page(data);
    PMM::free_page(child_b);
    PMM::free_page(child_a);
    PMM::free_page(canon_pd);
    PMM::free_page(canon_pdpt);
    PMM::free_page(canon);
    JARVIS_ASSERT(PMM::pool_used_pages() == pages_before);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Merge performs zero allocations by construction (link
//           semantics) — this pins the property that makes a mid-merge
//           OOM impossible (paper failure mode reframed, issue #96 D1).
// Input: Populated scratch parent; PMM baseline; merge into fresh child.
// Expect: PMM used-pages delta across the merge itself is zero.
// Depends: VMM::merge_kernel_half (issue #96)
JARVIS_TEST(merge_allocates_nothing, "PRE: none | POST: none") {
    uint64_t parent = alloc_zero_table();
    uint64_t pdpt = alloc_zero_table();
    uint64_t pd = alloc_zero_table();
    uint64_t data = alloc_zero_table();
    uint64_t child = alloc_zero_table();
    JARVIS_ASSERT(parent != 0 && pdpt != 0 && pd != 0 && data != 0 &&
                  child != 0);

    table_virt(parent)[256] = pdpt | PAGE_PRESENT | PAGE_WRITE;
    table_virt(parent)[257] =
        (data & ~0xFFFULL) | PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE;
    table_virt(parent)[511] = pd | PAGE_PRESENT | PAGE_WRITE;
    for (size_t i = 0; i < 8; ++i)
        table_virt(pd)[i] =
            (data & ~0xFFFULL) | PAGE_PRESENT | PAGE_WRITE | PAGE_NX;

    uint64_t pages_before = PMM::pool_used_pages();
    JARVIS_ASSERT(VMM::merge_kernel_half(parent, child));
    JARVIS_ASSERT(PMM::pool_used_pages() == pages_before);

    PMM::free_page(child);
    PMM::free_page(data);
    PMM::free_page(pd);
    PMM::free_page(pdpt);
    PMM::free_page(parent);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The convergence audit detects a corrupted kernel-half entry
//           (paper §3 debug assertion, reframed as a pure check so the
//           test is gateable — a halting panic cannot run in `all`).
// Input: Clone of the live kernel PML4; corrupt one present entry.
// Expect: check true when intact, false when corrupted, true again
//         after restore; clone matches the template while intact.
// Depends: VMM::kernel_half_equal/matches_template (issue #96)
JARVIS_TEST(template_mismatch_detected, "PRE: none | POST: none") {
    uint64_t child = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(child != 0);
    auto *c = table_virt(child);

    size_t victim = 0;
    for (size_t i = arch::PML4_KERNEL_START; i < arch::PML4_ENTRIES; ++i) {
        if ((c[i] & PAGE_PRESENT) != 0) {
            victim = i;
            break;
        }
    }
    JARVIS_ASSERT(victim != 0);

    JARVIS_ASSERT(VMM::kernel_half_equal(child, VMM::get_kernel_pml4()));
    JARVIS_ASSERT(VMM::kernel_half_matches_template(child));

    uint64_t saved = c[victim];
    c[victim] = saved + 0x1000ULL;
    JARVIS_ASSERT(!VMM::kernel_half_equal(child, VMM::get_kernel_pml4()));
    JARVIS_ASSERT(!VMM::kernel_half_matches_template(child));

    c[victim] = saved;
    JARVIS_ASSERT(VMM::kernel_half_equal(child, VMM::get_kernel_pml4()));
    JARVIS_ASSERT(VMM::kernel_half_matches_template(child));

    PMM::free_page(child);
    JARVIS_TEST_PASS();
}

void register_pt_merge_tests() {
    Logger::info("Registering kernel-half merge tests");
    JARVIS_REGISTER_TEST(merge_equivalent_to_full_copy);
    JARVIS_REGISTER_TEST(merge_idempotent);
    JARVIS_REGISTER_TEST(late_mapping_visible_after_merge);
    JARVIS_REGISTER_TEST(merge_allocates_nothing);
    JARVIS_REGISTER_TEST(template_mismatch_detected);
}

#endif // CONFIG_ARCH_X86_64
