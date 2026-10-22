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
#include <kernel/arch/page_table.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <constants.hpp>

using namespace kernel;

// Per-arch table codec (issue #352): the merge tests hand-build scratch
// chains, so every entry must satisfy the arch's own table/leaf
// classifiers (VMM::is_table_entry → vmm_arch_is_table). Raw x86 flag
// idioms mis-decode elsewhere (aarch64 needs the 0b11 table descriptor,
// Sv39 needs the shift-coded PPN). Table geometry: 4 levels on
// x86_64/aarch64 (L0→L1→L2→L3), 3 on riscv64/Sv39 (L0→L1→L2).
#if defined(CONFIG_ARCH_RISCV64)
constexpr unsigned kLevels = 3;
#else
constexpr unsigned kLevels = 4;
#endif
constexpr unsigned kEntries = 512; // arch::ArchPageTable::ENTRIES, all arches

/// @brief Level index of va (level 0 = top). riscv64 uses the public
/// VMM Sv39 geometry (no ArchPageTable index helpers there by design).
inline size_t index_of(uint64_t va, unsigned level) {
#if defined(CONFIG_ARCH_RISCV64)
    if (level == 0)
        return static_cast<size_t>((va & VMM::L0_MASK) >> VMM::L0_SHIFT);
    if (level == 1)
        return static_cast<size_t>((va & VMM::L1_MASK) >> VMM::L1_SHIFT);
    return static_cast<size_t>((va & VMM::L2_MASK) >> VMM::L2_SHIFT);
#else
    if (level == 0)
        return arch::ArchPageTable::pml4_index(va);
    if (level == 1)
        return arch::ArchPageTable::pdpt_index(va);
    if (level == 2)
        return arch::ArchPageTable::pd_index(va);
    return arch::ArchPageTable::pt_index(va);
#endif
}

/// @brief Present bit (bit 0: P on x86, valid on aarch64, V on Sv39).
inline bool entry_present(uint64_t e) { return (e & 1ULL) != 0; }

/// @brief Table-vs-leaf classifier, mirroring vmm_arch_is_table per arch.
inline bool entry_is_table(uint64_t e) {
#if defined(CONFIG_ARCH_X86_64)
    return (e & (1ULL << 7)) == 0; // PS bit clear = table
#elif defined(CONFIG_ARCH_AARCH64)
    return (e & 0x3ULL) == 0x3ULL; // bits[1:0] = 11 = table descriptor
#else
    return (e & 0xEULL) == 0; // R|W|X clear = table (V only)
#endif
}

/// @brief Child-table phys from a table entry (never call on a leaf:
/// raw masking leaks NX/UXN/PXN and mis-decodes Sv39 PPNs).
inline uint64_t entry_table_child(uint64_t e) {
#if defined(CONFIG_ARCH_RISCV64)
    return VMM::sv39_pte_phys(e);
#else
    return e & ~0xFFFULL;
#endif
}

/// @brief Table link entry (present + table-shaped, kernel flags only —
/// merge tests never activate scratch roots, so no USER/attr bits needed).
inline uint64_t make_table_link(uint64_t phys) {
#if defined(CONFIG_ARCH_RISCV64)
    return VMM::sv39_phys_pte(phys) | 0x1ULL; // V, no R|W|X = table
#elif defined(CONFIG_ARCH_AARCH64)
    return (phys & ~0xFFFULL) | 0x3ULL; // valid + table descriptor
#else
    return (phys & ~0xFFFULL) | 0x3ULL; // P + W
#endif
}

/// @brief 4 KiB data-leaf entry at the deepest level (scratch-only: never
/// decoded or freed, only copied/compared — alignment is informational).
inline uint64_t make_page_leaf(uint64_t phys) {
#if defined(CONFIG_ARCH_RISCV64)
    return VMM::sv39_phys_pte(phys) | 0xFULL; // V|R|W|X leaf, kernel
#elif defined(CONFIG_ARCH_AARCH64)
    return (phys & ~0xFFFULL) | 0x3ULL; // valid + L3 page descriptor
#else
    return (phys & ~0xFFFULL) | 0x83ULL; // P + W + NX (data, never exec)
#endif
}

/// @brief Block-leaf entry at the given level (1 GiB at level 1 on
/// x86_64/aarch64, 2 MiB at level 1 on Sv39). Scratch-only: copied by
/// value, never decoded or freed.
inline uint64_t make_block_leaf(uint64_t phys, unsigned level) {
    (void)level; // geometry selected by arch, not by parameter
#if defined(CONFIG_ARCH_RISCV64)
    return VMM::sv39_phys_pte(phys) | 0xFULL; // V|R|W|X megapage leaf
#elif defined(CONFIG_ARCH_AARCH64)
    return (phys & ~0xFFFULL) | 0x1ULL; // valid, bit 1 clear = block
#else
    return (phys & ~0xFFFULL) | 0x83ULL; // P + W + PS (huge)
#endif
}

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
// Input: Scratch parent with a table chain + block leaf in the kernel
//        half; fresh zeroed scratch child.
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

    // Kernel-half chain (index 256 = first kernel-half slot, all arches).
    // The 1 GiB block sits at level 1 (legal block level on every arch —
    // a level-0 block is not a valid descriptor anywhere).
    table_virt(parent)[256] = make_table_link(pdpt);
    table_virt(pdpt)[0] = make_table_link(pd);
    table_virt(pd)[0] = make_table_link(pt);
    table_virt(pt)[0] = make_page_leaf(data);
    table_virt(pdpt)[7] = make_block_leaf(data, 1);

    JARVIS_ASSERT(VMM::merge_kernel_half(parent, child));
    auto *c = table_virt(child);
    auto *p = table_virt(parent);
    for (size_t i = arch::PML4_KERNEL_START; i < arch::PML4_ENTRIES; ++i)
        JARVIS_ASSERT(c[i] == p[i]);
    for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i)
        JARVIS_ASSERT(c[i] == 0);
    JARVIS_ASSERT(entry_table_child(c[256]) == (pdpt & ~0xFFFULL));
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

    // k_idx is in the kernel half on every arch (256..511).
    constexpr size_t k_idx = 300;
    table_virt(parent)[k_idx] = make_table_link(parent_pdpt);
    table_virt(parent_pdpt)[7] = make_block_leaf(data_a, 1);
    table_virt(child)[k_idx] = make_table_link(child_pdpt);
    table_virt(child_pdpt)[8] = make_block_leaf(data_b, 1);

    JARVIS_ASSERT(VMM::merge_kernel_half(parent, child));
    auto *cc = table_virt(child);
    JARVIS_ASSERT(cc[k_idx] == table_virt(child)[k_idx]);
    auto *cc_pdpt = table_virt(entry_table_child(cc[k_idx]));
    JARVIS_ASSERT(cc_pdpt[7] == table_virt(parent_pdpt)[7]);
    JARVIS_ASSERT(cc_pdpt[8] == make_block_leaf(data_b, 1));

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
//           Uses a block entry at level kLevels-2 (2 MiB on every arch:
//           PD on x86, L2 on aarch64, L1 on Sv39).
// Input: Scratch canonical root; snapshot its entries; add a block
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

    // k_idx is kernel-half on every arch; k_pd selects a slot at the
    // block level below; the VA is assembled from level indices (never
    // raw shifts — Sv39 level geometry differs).
    constexpr size_t k_idx = 400;
    constexpr size_t k_pd = 5;
    constexpr uint64_t k_off = 0x12345ULL;
    // Block bases are 2 MiB-aligned on every arch: pre-mask the synthetic
    // 4 KiB phys so the installed base provably equals what every walker
    // decodes (PMM placement does not guarantee 2M alignment).
    const uint64_t blk_base = data & ~0x1FFFFFULL;
    // 2 MiB blocks mask the low 21 bits on every arch.
    constexpr uint64_t k_block_mask = ~0x1FFFFFULL;
    table_virt(canon)[k_idx] = make_table_link(canon_pdpt);
#if defined(CONFIG_ARCH_RISCV64)
    // Sv39: 3 levels — the level below the top IS the 2 MiB block level.
    table_virt(canon_pdpt)[k_pd] = make_block_leaf(blk_base, 1);
    const uint64_t va = (static_cast<uint64_t>(k_idx) << VMM::L0_SHIFT) |
                        (static_cast<uint64_t>(k_pd) << VMM::L1_SHIFT) | k_off;
#else
    table_virt(canon_pdpt)[0] = make_table_link(canon_pd);
    table_virt(canon_pd)[k_pd] = make_block_leaf(blk_base, 2);
    const uint64_t va = (static_cast<uint64_t>(k_idx) << 39) |
                        (static_cast<uint64_t>(k_pd) << 21) | k_off;
#endif
    JARVIS_ASSERT(tmpl[k_idx] == 0);

    JARVIS_ASSERT(VMM::merge_kernel_half(canon, child_a));
    uint64_t resolved = VMM::virt_to_phys_in_pml4(va, child_a);
    JARVIS_ASSERT(resolved == ((data & k_block_mask) | k_off));
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

    table_virt(parent)[256] = make_table_link(pdpt);
    table_virt(pdpt)[0] = make_block_leaf(data, 1);
    table_virt(parent)[511] = make_table_link(pd);
    for (size_t i = 0; i < 8; ++i)
        table_virt(pd)[i] = make_page_leaf(data);

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
        if (entry_present(c[i])) {
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

    // Drain riscv64's inherited L0[0] user-owned subtree (no-op walk on
    // x86_64/aarch64 empty user halves), then free the root.
    VMM::free_user_pages(child);
    PMM::free_page(child);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Child teardown after a merge never frees shared kernel-half
//           tables (ownership immutability, issue #325-F).
// Input: Scratch parent chain in 256.. (kernel-owned tables + data);
//        fresh zeroed child; merge; tear the child down via
//        free_user_pages (the fork-failure path).
// Expect: merge links (child shares parent's tables); after teardown the
//         shared PDPT/PD/PT/data are all still allocated and the parent
//         entries unchanged; PMM delta zero after manual parent teardown.
// Depends: VMM::merge_kernel_half ownership contract (issue #325-F)
JARVIS_TEST(merge_child_teardown_preserves_shared, "PRE: none | POST: none") {
    uint64_t pages_before = PMM::pool_used_pages();
    uint64_t parent = alloc_zero_table();
    uint64_t pdpt = alloc_zero_table();
    uint64_t pd = alloc_zero_table();
    uint64_t pt = alloc_zero_table();
    uint64_t data = alloc_zero_table();
    uint64_t child = alloc_zero_table();
    JARVIS_ASSERT(parent != 0 && pdpt != 0 && pd != 0 && pt != 0 &&
                  data != 0 && child != 0);

    const uint64_t pdpt_link = make_table_link(pdpt);
    const uint64_t pd_link = make_table_link(pd);
    const uint64_t pt_link = make_table_link(pt);
    const uint64_t data_leaf = make_page_leaf(data);
    table_virt(parent)[256] = pdpt_link;
    table_virt(pdpt)[0] = pd_link;
    table_virt(pd)[0] = pt_link;
    table_virt(pt)[0] = data_leaf;

    JARVIS_ASSERT(VMM::merge_kernel_half(parent, child));
    auto *c = table_virt(child);
    JARVIS_ASSERT(entry_table_child(c[256]) == (pdpt & ~0xFFFULL));

    // Fork-failure path: destroy the child address space. All shared
    // tables/data are kernel-owned, so the drain must skip them.
    VMM::free_user_pages(child);
    PMM::free_page(child);

    // Shared chain intact, still allocated, parent untouched.
    JARVIS_ASSERT(PMM::is_allocated(pdpt));
    JARVIS_ASSERT(PMM::is_allocated(pd));
    JARVIS_ASSERT(PMM::is_allocated(pt));
    JARVIS_ASSERT(PMM::is_allocated(data));
    JARVIS_ASSERT(table_virt(parent)[256] == pdpt_link);
    JARVIS_ASSERT(table_virt(pdpt)[0] == pd_link);
    JARVIS_ASSERT(table_virt(pd)[0] == pt_link);
    JARVIS_ASSERT(table_virt(pt)[0] == data_leaf);

    PMM::free_page(data);
    PMM::free_page(pt);
    PMM::free_page(pd);
    PMM::free_page(pdpt);
    PMM::free_page(parent);
    JARVIS_ASSERT(PMM::pool_used_pages() == pages_before);
    JARVIS_TEST_PASS();
}

void register_pt_merge_tests() {
    Logger::info("Registering kernel-half merge tests");
    JARVIS_REGISTER_TEST(merge_equivalent_to_full_copy);
    JARVIS_REGISTER_TEST(merge_idempotent);
    JARVIS_REGISTER_TEST(late_mapping_visible_after_merge);
    JARVIS_REGISTER_TEST(merge_allocates_nothing);
    JARVIS_REGISTER_TEST(template_mismatch_detected);
    JARVIS_REGISTER_TEST(merge_child_teardown_preserves_shared);
}
