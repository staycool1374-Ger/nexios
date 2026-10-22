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

/// @file test_pml4_clone.cpp
/// @brief PML4 page-table clone (fork) tests.

#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/arch/page_table.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <constants.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

// v0.4.0 MP-7 compile-time requirement: the sharing flag must be GONE.
template <typename T>
constexpr bool has_page_table_shared_v = requires(T *t) { t->page_table_shared_; };
static_assert(!has_page_table_shared_v<TaskControlBlock>,
              "MP-7: TaskControlBlock::page_table_shared_ must be removed "
              "(deep-copy fork never shares table pages)");

// Per-arch table codec (issue #352): hand-built scratch chains must satisfy
// the arch's own table/leaf classifiers. Mirrors test_pt_merge.cpp §1
// (kept byte-identical in shape so an auditor can diff the two files).
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

/// @brief Child-table phys from a table entry (never call on a leaf:
/// raw masking leaks NX/UXN/PXN and mis-decodes Sv39 PPNs).
inline uint64_t entry_leaf_frame(uint64_t e) {
#if defined(CONFIG_ARCH_RISCV64)
    return VMM::sv39_pte_phys(e);
#elif defined(CONFIG_ARCH_AARCH64)
    return e & 0x0000FFFFFFFFF000ULL;
#else
    return e & 0x000FFFFFFFFFF000ULL;
#endif
}

/// @brief Whether an entry marks user-accessible memory (for the #387
/// inheritance rule: kernel-owned leaves are inherited by value and must
/// be skipped by independence walks).
inline bool entry_has_user_flag(uint64_t e) {
#if defined(CONFIG_ARCH_X86_64)
    return (e & (1ULL << 2)) != 0;
#elif defined(CONFIG_ARCH_AARCH64)
    return (e & (1ULL << 6)) != 0; // AP_USER
#else
    return (e & (1ULL << 4)) != 0; // U
#endif
}

/// @brief Whether an entry is user-owned: U-flagged, or a frame from the
/// user pool (covers flag-less kernel leaves mapping user frames).
inline bool entry_is_user_owned(uint64_t e) {
    if (entry_has_user_flag(e))
        return true;
    return PMM::is_user_page(entry_leaf_frame(e));
}

/// @brief Whether a present entry is a meaningful mapping at this level.
/// Only Sv39 has reserved present-but-meaningless encodings: a V-only
/// entry at the leaf level (L2) is an invalid non-leaf (#206) — neither
/// a mapping to compare nor a table to descend.
inline bool entry_is_meaningful(uint64_t e, unsigned level) {
#if defined(CONFIG_ARCH_RISCV64)
    if (level + 1 >= kLevels)
        return (e & 0xEULL) != 0; // R|W|X set = real leaf
#else
    (void)e;
    (void)level;
#endif
    return true;
}

/// @brief Level-aware page-table dumper (debug aid only): walks present
/// entries, prints table/leaf classification via the arch codec, descends
/// only into user-owned tables. Never uses raw masking on leaves.
static void dbg_dump_tables(uint64_t root_phys, unsigned level,
                            unsigned max_depth) {
    if (level >= max_depth)
        return;
    auto *tab = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                             (root_phys & ~0xFFFULL));
    for (unsigned i = 0; i < kEntries; ++i) {
        const uint64_t e = tab[i];
        if (!entry_present(e))
            continue;
        for (unsigned s = 0; s <= level; ++s)
            Logger::raw_write("  ");
        Logger::raw_write("[");
        Logger::print_dec(i);
        if (entry_is_table(e)) {
            Logger::raw_write("] T=0x");
            Logger::print_hex(entry_table_child(e));
            Logger::raw_write("\n");
            if (PMM::is_user_page(entry_table_child(e)))
                dbg_dump_tables(entry_table_child(e), level + 1, max_depth);
        } else {
            Logger::raw_write("] L=0x");
            Logger::print_hex(entry_leaf_frame(e));
            if (entry_is_user_owned(e))
                Logger::raw_write(" US");
            Logger::raw_write("\n");
        }
    }
}

static void dbg_dump_pml4(uint64_t pml4_phys) {
    Logger::raw_write("Tables at 0x");
    Logger::print_hex(pml4_phys);
    Logger::raw_write(":\n");
    dbg_dump_tables(pml4_phys, 0, kLevels);
}

// Runmode: kernel
// Testidea: Verifies clone_kernel_pml4() has zero entries in user range 0-255.
// Input: Call clone_kernel_pml4(), inspect entries 0-255
// Expect: All user entries are zero (no identity-map leak) — except
// riscv64 L0[0], the shared kernel identity/MMIO window (present but
// never user-accessible, U==0, by design #205/#221).
// Depends: kernel::memory::VMM
JARVIS_TEST(pml4_clone_clears_user_entries, "PRE: none | POST: none") {
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);

    auto *virt =
        reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + (pml4 & ~0xFFFULL));
    for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i) {
#if defined(CONFIG_ARCH_RISCV64)
        // L0[0]: shared kernel identity/MMIO window (present, U==0).
        if (i == 0 && virt[i] != 0 && !entry_has_user_flag(virt[i]))
            continue;
#endif
        if (virt[i] != 0) {
            VMM::free_user_pages(pml4);
            PMM::free_page(pml4);
            JARVIS_FAIL("LEAK: entry %u = 0x%x", i, virt[i]);
        }
    }

    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies clone_kernel_pml4() copies kernel entries 256-511
// matching the kernel PML4.
// Input: Call clone_kernel_pml4(), compare entries with kernel PML4
// Expect: Kernel entries match exactly
// Depends: kernel::memory::VMM
JARVIS_TEST(pml4_clone_kernel_entries_match, "PRE: none | POST: none") {
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);

    uint64_t kernel_pml4 = VMM::get_kernel_pml4();
    auto *new_virt =
        reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + (pml4 & ~0xFFFULL));
    auto *kern_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                   (kernel_pml4 & ~0xFFFULL));

    for (size_t i = arch::PML4_KERNEL_START; i < arch::PML4_ENTRIES; ++i) {
        if (new_virt[i] != kern_virt[i]) {
            VMM::free_user_pages(pml4);
            PMM::free_page(pml4);
            JARVIS_FAIL("MISMATCH entry %u: new=0x%x kernel=0x%x", i,
                        new_virt[i], kern_virt[i]);
        }
    }

    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Simulates fork PML4 setup: parent has user entries, child PML4
// copies them.
// Input: Create parent PML4 with user mapping, create child PML4 copying
// user entries
// Expect: Child's user entries match parent's; kernel entries match kernel PML4
// Depends: kernel::memory::VMM, PMM
JARVIS_TEST(pml4_fork_user_entries_match, "PRE: none | POST: none") {
    uint64_t parent_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(parent_pml4 != 0);
    uint64_t child_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(child_pml4 != 0);

    // Set up a USER-owned mapping at a VA in L0[1] on Sv39 (clear of the
    // L0[0] kernel identity/MMIO window) via map_page_in_pml4 — table
    // pages become USER-owned so free_user_pages reclaims them.
    constexpr uint64_t TEST_VA = 0x40000000ULL;
    uint64_t user_page = PMM::alloc_user_page();
    JARVIS_ASSERT(user_page != 0);
    VMM::map_page_in_pml4(TEST_VA, user_page, true, false, parent_pml4);

    // v0.4.0 MP-7 fork semantics: deep copy, never shared tables.
    JARVIS_ASSERT(VMM::deep_copy_user_pages(parent_pml4, child_pml4));

    const size_t top_idx = index_of(TEST_VA, 0);
    const size_t next_idx = index_of(TEST_VA, 1);
    JARVIS_ASSERT(top_idx < arch::PML4_USER_COUNT);

    auto *parent_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                     (parent_pml4 & ~0xFFFULL));
    auto *child_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                    (child_pml4 & ~0xFFFULL));
    auto *kern_virt = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (VMM::get_kernel_pml4() & ~0xFFFULL));

    // User entries are present in both (deep copy preserved the mapping).
    JARVIS_ASSERT(entry_present(child_virt[top_idx]));
    // But the table pages differ: the child owns its own chain.
    JARVIS_ASSERT(entry_table_child(child_virt[top_idx]) !=
                  entry_table_child(parent_virt[top_idx]));
    auto *child_next = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + entry_table_child(child_virt[top_idx]));
    auto *parent_next = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + entry_table_child(parent_virt[top_idx]));
    JARVIS_ASSERT(entry_table_child(child_next[next_idx]) !=
                  entry_table_child(parent_next[next_idx]));

    // The leaf data page differs too (deep copy of content).
    uint64_t parent_leaf =
        VMM::virt_to_phys_in_pml4(TEST_VA, parent_pml4);
    uint64_t child_leaf = VMM::virt_to_phys_in_pml4(TEST_VA, child_pml4);
    JARVIS_ASSERT(parent_leaf == user_page);
    JARVIS_ASSERT(child_leaf != 0);
    JARVIS_ASSERT(child_leaf != parent_leaf);

    // Kernel entries still mirror the kernel PML4 in the child.
    for (size_t i = arch::PML4_KERNEL_START; i < arch::PML4_ENTRIES; ++i) {
        if (child_virt[i] != kern_virt[i]) {
            JARVIS_FAIL("KERN MISMATCH entry %u: child=0x%x kernel=0x%x", i,
                        child_virt[i], kern_virt[i]);
        }
    }

    // Teardown: free_user_pages on each address space reclaims its own
    // USER-owned table pages + data (MP-7 unconditional semantics).
    VMM::free_user_pages(child_pml4);
    PMM::free_page(child_pml4);
    VMM::free_user_pages(parent_pml4);
    PMM::free_page(parent_pml4);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies mapping in child's PML4 does not corrupt parent's PML4.
// Input: Create parent PML4, fork child via clone_kernel_pml4 (private
// kernel-half copy, empty user half), map a new page in child's PML4.
// Expect: Parent's PML4 unchanged, child's PML4 has the new mapping.
// Depends: kernel::memory::VMM, PMM
JARVIS_TEST(pml4_fork_no_child_corrupt_parent, "PRE: none | POST: none") {
    uint64_t parent_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(parent_pml4 != 0);

    // Simulate fork: the child is a private clone (never a raw copy of
    // the parent's user half — on riscv64 that would share the L0[0]
    // kernel identity subtree and the child's teardown would free it
    // out from under the parent).
    auto *parent_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                     (parent_pml4 & ~0xFFFULL));
    uint64_t child_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(child_pml4 != 0);
    auto *child_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                    (child_pml4 & ~0xFFFULL));

    // Pin the "copy is a no-op" claim: the fresh clone's user half is
    // empty (riscv64 L0[0] kernel window allowed, never U-flagged).
    for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i) {
#if defined(CONFIG_ARCH_RISCV64)
        if (i == 0 && parent_virt[i] != 0 &&
            !entry_has_user_flag(parent_virt[i]))
            continue;
#endif
        JARVIS_ASSERT(parent_virt[i] == 0);
        JARVIS_ASSERT(child_virt[i] == 0);
    }

    // Snapshot parent's user entries before child modifies (fixed-size
    // snapshot: user half is 256 entries on every arch).
    uint64_t parent_snapshot[256];
    for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i)
        parent_snapshot[i] = parent_virt[i];

    // Map a page in the CHILD's PML4 (L0[1] on Sv39: clear of the
    // L0[0] kernel window; low user half everywhere else).
    constexpr uint64_t child_va = 0x58000000ULL;
    uint64_t child_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(child_phys != 0);
    VMM::map_page_in_pml4(child_va, child_phys, true, false, child_pml4);

    // Verify child can translate the new mapping
    uint64_t translated = VMM::virt_to_phys_in_pml4(child_va, child_pml4);
    JARVIS_ASSERT(translated == child_phys);

    // Verify parent's PML4 entries are unchanged
    for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i) {
        if (parent_virt[i] != parent_snapshot[i]) {
            JARVIS_FAIL("PARENT CORRUPTED at entry %u: before=0x%x after=0x%x",
                        i, parent_snapshot[i], parent_virt[i]);
        }
    }

    // Verify parent cannot see the mapping
    uint64_t parent_t = VMM::virt_to_phys_in_pml4(child_va, parent_pml4);
    JARVIS_ASSERT(parent_t == 0);

    // Teardown: free_user_pages reclaims each root's own USER-owned
    // tables + data (the child's fresh mapping, the riscv64 L0[0]
    // subtree); then free the roots.
    VMM::free_user_pages(child_pml4);
    PMM::free_page(child_pml4);
    VMM::free_user_pages(parent_pml4);
    PMM::free_page(parent_pml4);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-7 — verifies deep_copy_user_pages() produces a
// no-alias address space: the child's data frame AND its table pages
// all differ from the parent's, and a write through the child's frame
// does not disturb the parent's frame.
// Input: Parent PML4 via clone_kernel_pml4; map a USER-owned data page at
// TEST_VA via map_page_in_pml4 (user=true so the table pages are also
// USER-owned and reclaimed by free_user_pages); deep_copy_user_pages into a
// fresh child PML4.
// Expect: virt_to_phys_in_pml4(TEST_VA, child) != parent phys; writing 0xA5
// into the child frame via HHDM leaves the parent frame at its original
// 0x5A; the child's top table differs from the parent's.
// Depends: kernel::memory::VMM, PMM
JARVIS_TEST(pml4_deep_copy_no_alias, "PRE: none | POST: none") {
    uint64_t parent_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(parent_pml4 != 0);
    uint64_t child_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(child_pml4 != 0);

    constexpr uint64_t TEST_VA = 0x40000000ULL;
    uint64_t parent_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(parent_phys != 0);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *parent_frame =
        reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + parent_phys);
    parent_frame[0] = 0x5A;
    VMM::map_page_in_pml4(TEST_VA, parent_phys, true, false, parent_pml4);

    // Deep-copy user entries from parent to child.
    JARVIS_ASSERT(VMM::deep_copy_user_pages(parent_pml4, child_pml4));

    // Data frames must differ: child has its own copy.
    uint64_t child_phys = VMM::virt_to_phys_in_pml4(TEST_VA, child_pml4);
    JARVIS_ASSERT(child_phys != 0);
    JARVIS_ASSERT(child_phys != parent_phys);

    // Write through the child's frame: parent frame must stay untouched.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *child_frame =
        reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + child_phys);
    child_frame[0] = 0xA5;
    JARVIS_ASSERT(child_frame[0] == 0xA5);
    JARVIS_ASSERT(parent_frame[0] == 0x5A);

    // Table pages must differ too: the top-level entries point to
    // distinct tables.
    auto *parent_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                     (parent_pml4 & ~0xFFFULL));
    auto *child_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                    (child_pml4 & ~0xFFFULL));
    const size_t top_idx = index_of(TEST_VA, 0);
    JARVIS_ASSERT(entry_table_child(parent_virt[top_idx]) !=
                  entry_table_child(child_virt[top_idx]));

    // Teardown: free_user_pages reclaims the USER-owned table pages AND the
    // data page in each address space (MP-7 unconditional semantics).
    VMM::free_user_pages(child_pml4);
    PMM::free_page(child_pml4);
    VMM::free_user_pages(parent_pml4);
    PMM::free_page(parent_pml4);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-7 — free_user_pages with deep-copy fork semantics:
// tearing down the CHILD's address space frees only the child's own
// USER-owned table pages + data; the parent's tables and data survive
// untouched (no sharing, no double-free).
// Input: Parent PML4 with a USER-owned mapping at 0x10000000; deep copy to
// child; free_user_pages(child) + free_page(child PML4).
// Expect: child's VA no longer resolves; parent's VA still resolves to its
// own leaf; parent tables are distinct from the (freed) child tables.
// Depends: kernel::memory::VMM, PMM
JARVIS_TEST(pml4_free_user_pages_shared_safe, "PRE: none | POST: none") {
    uint64_t parent_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(parent_pml4 != 0);
    uint64_t child_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(child_pml4 != 0);

    constexpr uint64_t va = 0x50000000ULL;
    uint64_t user_page = PMM::alloc_user_page();
    JARVIS_ASSERT(user_page != 0);
    VMM::map_page_in_pml4(va, user_page, true, false, parent_pml4);

    JARVIS_ASSERT(VMM::deep_copy_user_pages(parent_pml4, child_pml4));

    uint64_t parent_leaf = VMM::virt_to_phys_in_pml4(va, parent_pml4);
    uint64_t child_leaf = VMM::virt_to_phys_in_pml4(va, child_pml4);
    JARVIS_ASSERT(parent_leaf == user_page);
    JARVIS_ASSERT(child_leaf != 0);
    JARVIS_ASSERT(child_leaf != parent_leaf);

    // Tear down the CHILD address space completely. free_user_pages
    // clears each drained slot, so the gone-mapping assert must run
    // BEFORE the root page itself is freed (no read-after-free).
    VMM::free_user_pages(child_pml4);
    // The child's VA must be gone (its USER-owned tables + data reclaimed).
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(va, child_pml4) == 0);
    PMM::free_page(child_pml4);

    // The parent is untouched: same leaf, still mapped.
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(va, parent_pml4) == user_page);

    // Parent teardown reclaims its own USER-owned tables + data.
    VMM::free_user_pages(parent_pml4);
    PMM::free_page(parent_pml4);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the full identity-map is absent from cloned PML4 (all
// 512 entries zero at PDPT level).
// Input: Call clone_kernel_pml4(), use dbg_dump_pml4 to inspect, then verify
// no user-accessible entries exist
// Expect: No user-accessible entries in user range 0-255
// Depends: kernel::memory::VMM
JARVIS_TEST(pml4_dump_no_user_entries, "PRE: none | POST: none") {
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);

    Logger::raw_write("  Dumping PML4 via dbg_dump_pml4:\n");
    dbg_dump_pml4(pml4);

    auto *virt =
        reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + (pml4 & ~0xFFFULL));
    for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i) {
#if defined(CONFIG_ARCH_RISCV64)
        // L0[0]: shared kernel identity/MMIO window (present, U==0).
        if (i == 0 && virt[i] != 0 && !entry_has_user_flag(virt[i]))
            continue;
#endif
        if (entry_present(virt[i])) {
            VMM::free_user_pages(pml4);
            PMM::free_page(pml4);
            JARVIS_FAIL("UNEXPECTED PRESENT entry %u = 0x%x", i, virt[i]);
        }
    }

    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-7 — after a REAL fork (clone() from a dispatched
// parent), the child's user page tables are fresh physical copies: for
// every present user PML4 entry, the child's PDPT phys differs from the
// parent's, and the same holds at PD and PT levels; the data page is a
// private copy (distinct phys, distinct tables).  Kernel entries mirror the
// kernel PML4.
// Input: REAL user parent (create_user: real PMM user stack + real user
// page tables, never dispatched) impersonated via ScopedCurrentTask;
// a USER-owned data page mapped at PROBE_VA; clone() runs the genuine
// user copy path in the test's own context, then every present user
// entry of both address spaces is walked recursively.
// Expect: every user table phys differs at every level; the child data
// leaf is a fresh copy (child_leaf != parent_leaf); kernel entries mirror
// the kernel PML4; the parent mapping survives the child's teardown.
// Depends: kernel::TaskControlBlock, kernel::Scheduler, kernel::memory::VMM
//
// Recursive independence walk: tables must be distinct pages at every
// level; leaves owned by the user must differ; kernel-owned leaves are
// inherited by value (#387) and skipped. Child-only entries are expected
// (the child's own user stack) and not visited.
static bool walk_tables_independent(uint64_t *p_tab, uint64_t *c_tab,
                                    unsigned level, size_t count) {
    for (size_t i = 0; i < count; ++i) {
#if defined(CONFIG_ARCH_RISCV64)
        // Skip the L0[0] kernel window entirely (level 0, index 0): it is
        // kernel infrastructure with its own fork rules (inherit-or-drop
        // per level, U=1 boot MMIO leaves) — not user fork semantics.
        // No test maps user VA below 1 GiB, so nothing user-owned is
        // skipped. Covered instead by the L0[0] allowlists + teardown
        // tests. See the #352 walk note (U=1 RTC leaf L2[257]).
        if (level == 0 && i == 0)
            continue;
#endif
        const uint64_t p_ent = p_tab[i];
        if (!entry_present(p_ent))
            continue;
        if (!entry_is_meaningful(p_ent, level))
            continue; // reserved encoding, never a mapping (Sv39 L2 V-only)
        const uint64_t c_ent = c_tab[i];
        if (!entry_present(c_ent))
            return false;
        // At the leaf level every present entry is a leaf; above it the
        // arch codec decides (blocks are leaves at any level).
        const bool p_tabbed =
            (level + 1 < kLevels) && entry_is_table(p_ent);
        const bool c_tabbed =
            (level + 1 < kLevels) && entry_is_table(c_ent);
        if (p_tabbed != c_tabbed)
            return false;
        if (!p_tabbed) {
            if (!entry_is_user_owned(p_ent))
                continue;
            if (entry_leaf_frame(c_ent) == entry_leaf_frame(p_ent))
                return false;
            continue;
        }
        if (entry_table_child(c_ent) == entry_table_child(p_ent))
            return false;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *p_next = reinterpret_cast<uint64_t *>(
            arch::HHDM_OFFSET + entry_table_child(p_ent));
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *c_next = reinterpret_cast<uint64_t *>(
            arch::HHDM_OFFSET + entry_table_child(c_ent));
        if (!walk_tables_independent(p_next, c_next, level + 1, kEntries))
            return false;
    }
    return true;
}

JARVIS_TEST(fork_deep_copy_child_tables_independent, "PRE: none | POST: none") {
    constexpr uint64_t PROBE_VA = 0x48000000ULL;

    auto *parent = TaskControlBlock::create_user(test::forever_entry, 5, 10,
                                                 32_KiB);
    JARVIS_ASSERT(parent != nullptr);
    JARVIS_ASSERT(parent->is_user_);
    uint64_t phys = PMM::alloc_user_page();
    JARVIS_ASSERT(phys != 0);
    VMM::map_page_in_pml4(PROBE_VA, phys, true, false, parent->get_page_table());

    uint64_t all_indep = 0;
    uint64_t leaf_diff = 0;
    uint64_t parent_ok = 0;
    uint64_t ran = 0;
    {
        test::ScopedCurrentTask scope(*parent);
        uint64_t regs[37] = {};
        test::make_synthetic_clone_frame(regs);

        auto *child = TaskControlBlock::clone(regs);
        if (child != nullptr) {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *pv = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                    (parent->get_page_table() &
                                                     ~0xFFFULL));
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *cv = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                    (child->get_page_table() &
                                                     ~0xFFFULL));
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *kv = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                    (VMM::get_kernel_pml4() &
                                                     ~0xFFFULL));

            all_indep = walk_tables_independent(pv, cv, 0,
                                                  arch::PML4_USER_COUNT)
                                ? 1
                                : 0;

            // Kernel entries mirror the kernel PML4 in the child.
            for (size_t i = arch::PML4_KERNEL_START; i < arch::PML4_ENTRIES;
                 ++i) {
                if (cv[i] != kv[i])
                    all_indep = 0;
            }

            const uint64_t p_leaf =
                VMM::virt_to_phys_in_pml4(PROBE_VA, parent->get_page_table());
            const uint64_t c_leaf =
                VMM::virt_to_phys_in_pml4(PROBE_VA, child->get_page_table());
            leaf_diff = (c_leaf != 0 && c_leaf != p_leaf) ? 1 : 0;

            child->cleanup();
            delete child;

            // Parent mapping survives the child's teardown.
            parent_ok = (VMM::virt_to_phys_in_pml4(
                             PROBE_VA, parent->get_page_table()) == p_leaf)
                            ? 1
                            : 0;
            ran = 1;
        }
        // Park the never-dispatched fixture BEFORE scope exit: set_current()
        // re-enqueues a READY/RUNNING outgoing current (issue #399 follow-up).
        parent->state = TaskState::BLOCKED;
    }
    parent->cleanup();
    delete parent;

    JARVIS_ASSERT_EQ(1ULL, ran);
    JARVIS_ASSERT_EQ(1ULL, all_indep);
    JARVIS_ASSERT_EQ(1ULL, leaf_diff);
    JARVIS_ASSERT_EQ(1ULL, parent_ok);
    // The parent's cleanup reclaimed the USER-owned leaf via free_user_pages.
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-7 — free_user_pages on the CHILD's deep-copied address
// space (via the real child->cleanup() after a real clone()) reclaims every
// copied user table page AND the data page; the parent's mappings survive
// untouched (phys entries unchanged); the whole fork+teardown cycle restores
// the PMM baseline (no leak, no double-free).
// Input: parent kernel task (prio 11, is_user_ set) with a USER-owned page
// at PROBE_VA; the dispatched lambda clones, captures the parent's
// PML4->PDPT entry + leaf and the child's leaf, then cleans the child up and
// re-walks the parent.
// Expect: child leaf differs from parent leaf before teardown; after
// teardown the parent PML4->PDPT entry is unchanged and the parent VA still
// resolves to its own leaf; PMM::free_pages_ref() returns to the baseline.
// Depends: kernel::TaskControlBlock, kernel::Scheduler, kernel::memory::VMM
JARVIS_TEST(fork_free_user_pages_child_deepcopy, "PRE: none | POST: none") {
    constexpr uint64_t PROBE_VA = 0x48001000ULL;

    uint64_t free_before = PMM::free_pages_ref();

    auto *parent = TaskControlBlock::create_user(test::forever_entry, 5, 10,
                                                 32_KiB);
    JARVIS_ASSERT(parent != nullptr);
    JARVIS_ASSERT(parent->is_user_);
    uint64_t phys = PMM::alloc_user_page();
    JARVIS_ASSERT(phys != 0);
    VMM::map_page_in_pml4(PROBE_VA, phys, true, false, parent->get_page_table());

    uint64_t leaf_diff = 0;
    uint64_t parent_entry_ok = 0;
    uint64_t parent_leaf_ok = 0;
    uint64_t ran = 0;
    {
        test::ScopedCurrentTask scope(*parent);
        uint64_t regs[37] = {};
        test::make_synthetic_clone_frame(regs);

        auto *child = TaskControlBlock::clone(regs);
        if (child != nullptr) {
            const size_t top_idx = index_of(PROBE_VA, 0);
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *pv = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                    (parent->get_page_table() &
                                                     ~0xFFFULL));
            const uint64_t parent_top = pv[top_idx];
            const uint64_t parent_leaf =
                VMM::virt_to_phys_in_pml4(PROBE_VA, parent->get_page_table());
            const uint64_t child_leaf =
                VMM::virt_to_phys_in_pml4(PROBE_VA, child->get_page_table());
            leaf_diff = (child_leaf != 0 && child_leaf != parent_leaf) ? 1 : 0;

            // Tear down the child: cleanup() frees its deep-copied tables
            // AND the copied data page (free_user_pages semantics).
            child->cleanup();
            delete child;

            // Parent survives: top-level entry unchanged, VA still resolves.
            parent_entry_ok = (pv[top_idx] == parent_top) ? 1 : 0;
            parent_leaf_ok =
                (VMM::virt_to_phys_in_pml4(PROBE_VA,
                                           parent->get_page_table()) ==
                 parent_leaf)
                    ? 1
                    : 0;
            ran = 1;
        }
        // Park the never-dispatched fixture (issue #399 follow-up).
        parent->state = TaskState::BLOCKED;
    }
    parent->cleanup();
    delete parent;

    JARVIS_ASSERT_EQ(1ULL, ran);
    JARVIS_ASSERT_EQ(1ULL, leaf_diff);
    JARVIS_ASSERT_EQ(1ULL, parent_entry_ok);
    JARVIS_ASSERT_EQ(1ULL, parent_leaf_ok);
    // Full cycle returns to the PMM baseline: no leak, no double-free.
    JARVIS_ASSERT_FMT(PMM::free_pages_ref() == free_before,
                      "PMM delta %ld pages after fork deep-copy teardown",
                      (long)(PMM::free_pages_ref() - free_before));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-7 — TaskControlBlock::page_table_shared_ no longer
// exists (compile-time static_assert) and a real clone() produces a child
// whose page_table_ is a fresh deep copy (never equal to the parent's).
// Input: REAL user parent (create_user, never dispatched) impersonated via
// ScopedCurrentTask; clone() runs the genuine user copy path; capture the
// child's page_table_.
// Expect: build succeeds with the field removed (static_assert passes);
// child->get_page_table() != 0 and != parent->get_page_table(); the child stack
// exists; teardown leaves zero task delta.
// Depends: kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(fork_page_table_shared_flag_absent, "PRE: none | POST: none") {
    auto *parent = TaskControlBlock::create_user(test::forever_entry, 5, 10,
                                                 32_KiB);
    JARVIS_ASSERT(parent != nullptr);
    JARVIS_ASSERT(parent->is_user_);
    const uint64_t parent_pt = parent->get_page_table();
    JARVIS_ASSERT(parent_pt != 0);
    uint64_t child_pt = 0;
    uint64_t child_stack = 0;
    uint64_t child_ok = 0;
    {
        test::ScopedCurrentTask scope(*parent);
        uint64_t regs[37] = {};
        test::make_synthetic_clone_frame(regs);

        auto *child = TaskControlBlock::clone(regs);
        if (child != nullptr) {
            child_pt = child->get_page_table();
            child_stack = child->user_stack_;
            child_ok = (child_pt != 0 && child_stack != 0) ? 1 : 0;
            child->cleanup();
            delete child;
        }
        // Park the never-dispatched fixture (issue #399 follow-up).
        parent->state = TaskState::BLOCKED;
    }
    parent->cleanup();
    delete parent;

    JARVIS_ASSERT_EQ(1ULL, child_ok);
    JARVIS_ASSERT(child_pt != 0);
    JARVIS_ASSERT(child_pt != parent_pt);
    JARVIS_ASSERT(child_stack != 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all PML4 clone / fork page table tests.
// Input: None
// Expect: All tests registered
// Depends: test framework
// Runmode: kernel
// Testidea: deep_copy preserves leaf permission flags verbatim
// Input: parent maps one RO page (non-exec) + one RWX page (exec) via the
//        shared primitives; deep-copy into a child clone
// Expect: RO bit + executability match parent→child on both pages; content
//         identical but phys distinct.  D/A + USER bits copy through the
//         same verbatim expression (HW-only / ring-0-indistinguishable,
//         construction-verified, not pinned here).
// Depends: kernel::memory::VMM::deep_copy_user_pages (issue #325-E)
JARVIS_TEST(fork_deep_copy_flag_parity, "PRE: none | POST: none") {
    uint64_t parent_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(parent_pml4 != 0);
    uint64_t child_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(child_pml4 != 0);

    constexpr uint64_t RO_VA = 0x40000000ULL;
    constexpr uint64_t X_VA = 0x40001000ULL;
    uint64_t ro_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(ro_phys != 0);
    uint64_t x_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(x_phys != 0);
    VMM::map_page_ro_in_pml4(RO_VA, ro_phys, false, parent_pml4);
    VMM::map_page_in_pml4(X_VA, x_phys, true, true, parent_pml4);

    bool parent_ro_exec = true;
    JARVIS_ASSERT(VMM::pte_is_ro_in_pml4(RO_VA, parent_pml4, &parent_ro_exec));
    JARVIS_ASSERT(!parent_ro_exec);
    JARVIS_ASSERT(VMM::pte_is_executable_in_pml4(X_VA, parent_pml4));

    JARVIS_ASSERT(VMM::deep_copy_user_pages(parent_pml4, child_pml4));

    bool child_ro_exec = true;
    JARVIS_ASSERT(VMM::pte_is_ro_in_pml4(RO_VA, child_pml4, &child_ro_exec));
    JARVIS_ASSERT(!child_ro_exec);
    JARVIS_ASSERT(VMM::pte_is_executable_in_pml4(X_VA, child_pml4));
    JARVIS_ASSERT(!VMM::pte_is_executable_in_pml4(RO_VA, child_pml4));
    JARVIS_ASSERT_EQ(ro_phys, VMM::virt_to_phys_in_pml4(RO_VA, parent_pml4));
    const uint64_t child_ro = VMM::virt_to_phys_in_pml4(RO_VA, child_pml4);
    JARVIS_ASSERT(child_ro != 0 && child_ro != ro_phys);
    const uint64_t child_x = VMM::virt_to_phys_in_pml4(X_VA, child_pml4);
    JARVIS_ASSERT(child_x != 0 && child_x != x_phys);

    VMM::free_user_pages(child_pml4);
    PMM::free_page(child_pml4);
    VMM::free_user_pages(parent_pml4);
    PMM::free_page(parent_pml4);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: deep_copy fails closed on a user-range huge entry (no alias)
// Input: parent holds a hand-installed block leaf at the 2 MiB level
//        (PD on x86, L2 on aarch64, L1 on Sv39); deep-copy into a child
// Expect: false; child block entry absent (no shared writable page);
//         parent restored + teardown neutral
// Depends: kernel::memory::VMM::deep_copy_user_pages (issue #325-E)
JARVIS_TEST(fork_deep_copy_huge_fails_closed, "PRE: none | POST: none") {
    uint64_t parent_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(parent_pml4 != 0);
    uint64_t child_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(child_pml4 != 0);

    // Build the tables first with a throwaway 4 KiB map, then replace the
    // block-level entry with a huge leaf (tables stay; leaf ownership
    // moves to huge). HUGE_VA is in L0[1] on Sv39 (clear of L0[0]).
    constexpr uint64_t HUGE_VA = 0x40000000ULL;
    uint64_t scratch_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(scratch_phys != 0);
    VMM::map_page_in_pml4(HUGE_VA, scratch_phys, true, false, parent_pml4);
    VMM::unmap_page_in_pml4(HUGE_VA, parent_pml4);
    PMM::free_page(scratch_phys);
    uint64_t huge_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(huge_phys != 0);
    // Pre-mask to the 2M frame so every arch decodes the identical base.
    const uint64_t huge_base = huge_phys & ~0x1FFFFFULL;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *parent_top = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (parent_pml4 & ~0xFFFULL));
    const size_t i0 = index_of(HUGE_VA, 0);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *mid1 = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + entry_table_child(parent_top[i0]));
    const size_t i1 = index_of(HUGE_VA, 1);
#if defined(CONFIG_ARCH_RISCV64)
    auto *blk_tab = mid1;
    const size_t ib = i1;
#else
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *mid2 = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + entry_table_child(mid1[i1]));
    const size_t ib = index_of(HUGE_VA, 2);
    auto *blk_tab = mid2;
#endif
    // Capture the orphaned next-level table before overwriting its entry
    // (free below; free_user_pages cannot reach it once the block leaf
    // is installed).
    const uint64_t orphan = entry_table_child(blk_tab[ib]);
#if defined(CONFIG_ARCH_X86_64)
    blk_tab[ib] = (huge_base & ~0xFFFULL) | 0x87ULL; // P+W+U+huge
#elif defined(CONFIG_ARCH_AARCH64)
    // valid + block descriptor + EL0 access (USER bit is load-bearing).
    blk_tab[ib] = (huge_base & ~0xFFFULL) | 0x1ULL | (1ULL << 6);
#else
    // Sv39 megapage leaf: V|R|W|U (USER bit is load-bearing).
    blk_tab[ib] =
        VMM::sv39_phys_pte(huge_base) | 0x1ULL | 0x2ULL | 0x4ULL | 0x10ULL;
#endif
    // Issue #387: the USER bit is load-bearing — a production user huge
    // PTE carries U=1 (the throwaway map above was user); U=0 + user frame
    // is a hand-made corner the fork copy inherits by value.
    arch::ArchPageTable::tlb_flush(HUGE_VA);

    JARVIS_ASSERT(!VMM::deep_copy_user_pages(parent_pml4, child_pml4));

    // No alias: the child installed no block entry for the huge region.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *child_top = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (child_pml4 & ~0xFFFULL));
    if (entry_present(child_top[i0])) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *child_mid1 = reinterpret_cast<uint64_t *>(
            arch::HHDM_OFFSET + entry_table_child(child_top[i0]));
        if (entry_present(child_mid1[i1])) {
#if defined(CONFIG_ARCH_RISCV64)
            JARVIS_ASSERT(!entry_present(child_mid1[ib]));
#else
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *child_mid2 = reinterpret_cast<uint64_t *>(
                arch::HHDM_OFFSET + entry_table_child(child_mid1[i1]));
            JARVIS_ASSERT(!entry_present(child_mid2[ib]));
#endif
        }
    }

    // Restore + manual free (hand-made entry: bypass free_user_pages).
    PMM::free_page(huge_phys);
    PMM::free_page(orphan);
    blk_tab[ib] = 0;
    arch::ArchPageTable::tlb_flush(HUGE_VA);
    VMM::free_user_pages(child_pml4);
    PMM::free_page(child_pml4);
    VMM::free_user_pages(parent_pml4);
    PMM::free_page(parent_pml4);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: deep_copy OOM returns false and the partial child is fully
//          reclaimed by teardown (caller-destroys contract)
// Input: parent holds one user page; both pools drained (unified
//        refusal-loop drain); a measured number of tables freed back so
//        the copy installs tables then fails on the data page
// Expect: false; free_user_pages(child) reclaims the partial tables;
//         fully neutral counters
// Depends: kernel::memory::VMM::deep_copy_user_pages (issue #325-E)
JARVIS_TEST(fork_deep_copy_oom_reclaimed, "PRE: none | POST: none") {
    PMM::OOMHandler saved_oom = PMM::get_oom_handler();
    PMM::set_oom_handler(nullptr);
    auto oom_restore = ScopeGuard([&]() { PMM::set_oom_handler(saved_oom); });

    uint64_t parent_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(parent_pml4 != 0);
    uint64_t child_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(child_pml4 != 0);
    constexpr uint64_t VA = 0x40000000ULL;
    uint64_t data_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(data_phys != 0);
    VMM::map_page_in_pml4(VA, data_phys, true, false, parent_pml4);

    // Measure: run the copy once against a probe child to count the
    // allocations a single-page deep-copy needs (tables + data). The
    // count is arch-dependent (riscv64 materialises the inherited L0[0]
    // subtree, so a hard-coded number is wrong by construction).
    uint64_t probe_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(probe_pml4 != 0);
    const uint64_t probe_before = PMM::free_pages_ref();
    JARVIS_ASSERT(VMM::deep_copy_user_pages(parent_pml4, probe_pml4));
    const uint64_t probe_after = PMM::free_pages_ref();
    JARVIS_ASSERT(probe_after < probe_before);
    const uint64_t kAllocsNeeded = probe_before - probe_after;
    VMM::free_user_pages(probe_pml4);
    PMM::free_page(probe_pml4);

    arch::IrqGuard drain_guard;
    PMM::rebuild_free_list();
    uint64_t chain_head = 0;
    for (int settle = 0; settle < 3; ++settle) {
        size_t took = 0;
        uint64_t take_page = 0;
        size_t take_cap = 0;
        while ((take_page = PMM::alloc_page_table()) != 0) {
            JARVIS_ASSERT_FMT(take_cap < 100000,
                              "drain takes exceed window (cycling list?)");
            ++take_cap;
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *link = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                      take_page);
            *link = chain_head;
            chain_head = take_page;
            ++took;
        }
        if (took == 0) {
            break;
        }
    }
    // Free back all-but-one of the measured allocations, so the copy
    // installs tables then fails on the final (data) allocation.
    JARVIS_ASSERT(kAllocsNeeded > 0);
    for (size_t keep = 0; keep < kAllocsNeeded - 1; ++keep) {
        JARVIS_ASSERT(chain_head != 0);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *link = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                  chain_head);
        const uint64_t next_head = *link;
        PMM::free_page(chain_head);
        chain_head = next_head;
    }

    JARVIS_ASSERT(!VMM::deep_copy_user_pages(parent_pml4, child_pml4));

    VMM::free_user_pages(child_pml4);
    PMM::free_page(child_pml4);
    VMM::free_user_pages(parent_pml4);
    PMM::free_page(parent_pml4);
    while (chain_head != 0) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *link = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                  chain_head);
        const uint64_t next_head = *link;
        PMM::free_page(chain_head);
        chain_head = next_head;
    }
    JARVIS_TEST_PASS();
}

void register_pml4_clone_tests() {
    Logger::info("Registering bug #007 page table tests");
    JARVIS_REGISTER_TEST(pml4_clone_clears_user_entries);
    JARVIS_REGISTER_TEST(pml4_clone_kernel_entries_match);
    JARVIS_REGISTER_TEST(pml4_fork_user_entries_match);
    JARVIS_REGISTER_TEST(pml4_fork_no_child_corrupt_parent);
    JARVIS_REGISTER_TEST(pml4_deep_copy_no_alias);
    JARVIS_REGISTER_TEST(pml4_free_user_pages_shared_safe);
    JARVIS_REGISTER_TEST(pml4_dump_no_user_entries);
    JARVIS_REGISTER_TEST(fork_deep_copy_child_tables_independent);
    JARVIS_REGISTER_TEST(fork_free_user_pages_child_deepcopy);
    JARVIS_REGISTER_TEST(fork_page_table_shared_flag_absent);
    JARVIS_REGISTER_TEST(fork_deep_copy_flag_parity);
    JARVIS_REGISTER_TEST(fork_deep_copy_huge_fails_closed);
    JARVIS_REGISTER_TEST(fork_deep_copy_oom_reclaimed);
}
