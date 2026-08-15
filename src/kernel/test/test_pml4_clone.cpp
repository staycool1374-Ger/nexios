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
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/pmm.hpp>
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

enum : uint64_t {
    PML4_SHIFT = 39,
    PDPT_SHIFT = 30,
    PD_SHIFT = 21,
    PT_SHIFT = 12,
    PAGE_PRESENT = 1ULL << 0,
    PAGE_WRITE = 1ULL << 1,
    PAGE_USER = 1ULL << 2,
    PAGE_HUGE = 1ULL << 7,
    PAGE_NX = 1ULL << 63,
};

static void dbg_dump_pml4(uint64_t pml4_phys) {
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (pml4_phys & ~0xFFFULL));
    Logger::raw_write("PML4 at 0x");
    Logger::print_hex(pml4_phys);
    Logger::raw_write(":\n");

    for (int i = 0; i < 512; ++i) {
        if (!(pml4[i] & PAGE_PRESENT))
            continue;
        uint64_t phys = pml4[i] & ~0xFFFULL;
        Logger::raw_write("  [");
        Logger::print_dec(i);
        Logger::raw_write("] PDPT=0x");
        Logger::print_hex(phys);
        if (pml4[i] & PAGE_USER)
            Logger::raw_write(" US");
        if (pml4[i] & PAGE_WRITE)
            Logger::raw_write(" RW");
        Logger::raw_write("\n");

        if (i >= static_cast<int>(arch::PML4_USER_COUNT))
            continue;
        auto *pdpt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + phys);
        for (int j = 0; j < 512; ++j) {
            if (!(pdpt[j] & PAGE_PRESENT))
                continue;
            uint64_t pd_phys = pdpt[j] & ~0xFFFULL;
            uint64_t va_base = (static_cast<uint64_t>(i) << PML4_SHIFT) |
                               (static_cast<uint64_t>(j) << PDPT_SHIFT);
            Logger::raw_write("    [");
            Logger::print_dec(j);
            Logger::raw_write("] ");
            if (pdpt[j] & PAGE_HUGE) {
                Logger::raw_write("1GiB page phys=0x");
                Logger::print_hex(pd_phys);
            } else {
                Logger::raw_write("PD=0x");
                Logger::print_hex(pd_phys);
            }
            Logger::raw_write(" va=0x");
            Logger::print_hex(va_base);
            if (pdpt[j] & PAGE_USER)
                Logger::raw_write(" US");
            if (pdpt[j] & PAGE_WRITE)
                Logger::raw_write(" RW");
            Logger::raw_write("\n");

            if ((pdpt[j] & PAGE_HUGE) || !PMM::is_user_page(pd_phys))
                continue;
            auto *pd =
                reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pd_phys);
            for (int k = 0; k < 512; ++k) {
                if (!(pd[k] & PAGE_PRESENT))
                    continue;
                uint64_t pt_phys = pd[k] & ~0xFFFULL;
                Logger::raw_write("      [");
                Logger::print_dec(k);
                Logger::raw_write("] ");
                if (pd[k] & PAGE_HUGE) {
                    Logger::raw_write("2MiB page phys=0x");
                    Logger::print_hex(pt_phys);
                } else {
                    Logger::raw_write("PT=0x");
                    Logger::print_hex(pt_phys);
                }
                if (pd[k] & PAGE_USER)
                    Logger::raw_write(" US");
                if (pd[k] & PAGE_WRITE)
                    Logger::raw_write(" RW");
                Logger::raw_write("\n");

                if ((pd[k] & PAGE_HUGE) || !PMM::is_user_page(pt_phys))
                    continue;
                auto *pt =
                    reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pt_phys);
                for (int l = 0; l < 512; ++l) {
                    if (!(pt[l] & PAGE_PRESENT))
                        continue;
                    uint64_t leaf = pt[l] & ~0xFFFULL;
                    Logger::raw_write("        [");
                    Logger::print_dec(l);
                    Logger::raw_write("] page=0x");
                    Logger::print_hex(leaf);
                    if (pt[l] & PAGE_USER)
                        Logger::raw_write(" US");
                    if (pt[l] & PAGE_WRITE)
                        Logger::raw_write(" RW");
                    Logger::raw_write("\n");
                }
            }
        }
    }
}

// Runmode: kernel
// Testidea: Verifies clone_kernel_pml4() has zero entries in user range 0-255.
// Input: Call clone_kernel_pml4(), inspect entries 0-255
// Expect: All user entries are zero (no identity-map leak)
// Depends: kernel::memory::VMM
JARVIS_TEST(pml4_clone_clears_user_entries, "PRE: none | POST: none") {
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);

    auto *virt =
        reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + (pml4 & ~0xFFFULL));
    for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i) {
        if (virt[i] != 0) {
            PMM::free_page(pml4);
            JARVIS_FAIL("LEAK: entry %u = 0x%x", i, virt[i]);
        }
    }

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
            PMM::free_page(pml4);
            JARVIS_FAIL("MISMATCH entry %u: new=0x%x kernel=0x%x", i,
                        new_virt[i], kern_virt[i]);
        }
    }

    PMM::free_page(pml4);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Simulates fork PML4 setup: parent has user entries, child PML4
// copies them.
// Input: Create parent PML4 with user mapping, create child PML4 copying
// user entries
// Expect: Child's user entries match parent's, kernel entries match kernel PML4
// Depends: kernel::memory::VMM, PMM
JARVIS_TEST(pml4_fork_user_entries_match, "PRE: none | POST: none") {
    uint64_t parent_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(parent_pml4 != 0);
    uint64_t child_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(child_pml4 != 0);

    // Set up a USER-owned mapping at virtual address 0x400000 (typical ELF
    // base) via map_page_in_pml4 — table pages become USER-owned so
    // free_user_pages reclaims them.
    constexpr uint64_t TEST_VA = 0x400000;
    uint64_t user_page = PMM::alloc_user_page();
    JARVIS_ASSERT(user_page != 0);
    VMM::map_page_in_pml4(TEST_VA, user_page, true, parent_pml4);

    // v0.4.0 MP-7 fork semantics: deep copy, never shared tables.
    JARVIS_ASSERT(VMM::deep_copy_user_pages(parent_pml4, child_pml4));

    size_t pml4_idx = (TEST_VA >> PML4_SHIFT) & 0x1FF;
    size_t pdpt_idx = (TEST_VA >> PDPT_SHIFT) & 0x1FF;
    JARVIS_ASSERT(pml4_idx < arch::PML4_USER_COUNT);

    auto *parent_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                     (parent_pml4 & ~0xFFFULL));
    auto *child_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                    (child_pml4 & ~0xFFFULL));
    auto *kern_virt = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (VMM::get_kernel_pml4() & ~0xFFFULL));

    // User entries are present in both (deep copy preserved the mapping).
    JARVIS_ASSERT(child_virt[pml4_idx] & PAGE_PRESENT);
    // But the table pages differ: the child owns its own PDPT/PD/PT chain.
    JARVIS_ASSERT((child_virt[pml4_idx] & ~0xFFFULL) !=
                  (parent_virt[pml4_idx] & ~0xFFFULL));
    auto *child_pdpt = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (child_virt[pml4_idx] & ~0xFFFULL));
    auto *parent_pdpt = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (parent_virt[pml4_idx] & ~0xFFFULL));
    JARVIS_ASSERT((child_pdpt[pdpt_idx] & ~0xFFFULL) !=
                  (parent_pdpt[pdpt_idx] & ~0xFFFULL));

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
// Input: Create parent PML4, fork child, map a new page in child's PML4
// Expect: Parent's PML4 unchanged, child's PML4 has the new mapping
// Depends: kernel::memory::VMM, PMM
JARVIS_TEST(pml4_fork_no_child_corrupt_parent, "PRE: none | POST: none") {
    uint64_t parent_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(parent_pml4 != 0);

    // Simulate fork: child copies parent's user entries
    auto *parent_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                     (parent_pml4 & ~0xFFFULL));
    uint64_t kernel_pml4 = VMM::get_kernel_pml4();
    auto *kern_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                   (kernel_pml4 & ~0xFFFULL));

    uint64_t child_pml4 = PMM::alloc_page();
    JARVIS_ASSERT(child_pml4 != 0);
    auto *child_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                    (child_pml4 & ~0xFFFULL));

    for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i)
        child_virt[i] = parent_virt[i];
    for (size_t i = arch::PML4_KERNEL_START; i < arch::PML4_ENTRIES; ++i)
        child_virt[i] = kern_virt[i];

    // Snapshot parent's user entries before child modifies
    uint64_t parent_snapshot[256];
    for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i)
        parent_snapshot[i] = parent_virt[i];

    // Map a page in the CHILD's PML4 (note: the kid uses child_pml4)
    uint64_t child_va = 0x7FFF00000000ULL;
    uint64_t child_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(child_phys != 0);
    VMM::map_page_in_pml4(child_va, child_phys, true, child_pml4);

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

    // Free page-table pages allocated by map_page_in_pml4 before freeing the
    // PML4
    VMM::free_user_pages(child_pml4);
    PMM::free_page(child_phys);
    PMM::free_page(child_pml4);
    PMM::free_page(parent_pml4);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-7 — verifies deep_copy_user_pages() produces a
// no-alias address space: the child's data frame AND its table pages
// (PDPT/PD/PT) all differ from the parent's, and a write through the
// child's frame does not disturb the parent's frame.
// Input: Parent PML4 via clone_kernel_pml4; map a USER-owned data page at
// TEST_VA via map_page_in_pml4 (user=true so the table pages are also
// USER-owned and reclaimed by free_user_pages); deep_copy_user_pages into a
// fresh child PML4.
// Expect: virt_to_phys_in_pml4(TEST_VA, child) != parent phys; writing 0xA5
// into the child frame via HHDM leaves the parent frame at its original
// 0x5A; the child's PDPT page differs from the parent's.
// Depends: kernel::memory::VMM, PMM
JARVIS_TEST(pml4_deep_copy_no_alias, "PRE: none | POST: none") {
    uint64_t parent_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(parent_pml4 != 0);
    uint64_t child_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(child_pml4 != 0);

    constexpr uint64_t TEST_VA = 0x400000;
    uint64_t parent_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(parent_phys != 0);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *parent_frame =
        reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + parent_phys);
    parent_frame[0] = 0x5A;
    VMM::map_page_in_pml4(TEST_VA, parent_phys, true, parent_pml4);

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

    // Table pages must differ too: the PML4 entries point to distinct PDPT
    // pages.
    auto *parent_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                     (parent_pml4 & ~0xFFFULL));
    auto *child_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                    (child_pml4 & ~0xFFFULL));
    size_t pml4_idx = (TEST_VA >> PML4_SHIFT) & 0x1FF;
    JARVIS_ASSERT((parent_virt[pml4_idx] & ~0xFFFULL) !=
                  (child_virt[pml4_idx] & ~0xFFFULL));

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

    constexpr uint64_t va = 0x10000000;
    uint64_t user_page = PMM::alloc_user_page();
    JARVIS_ASSERT(user_page != 0);
    VMM::map_page_in_pml4(va, user_page, true, parent_pml4);

    JARVIS_ASSERT(VMM::deep_copy_user_pages(parent_pml4, child_pml4));

    uint64_t parent_leaf = VMM::virt_to_phys_in_pml4(va, parent_pml4);
    uint64_t child_leaf = VMM::virt_to_phys_in_pml4(va, child_pml4);
    JARVIS_ASSERT(parent_leaf == user_page);
    JARVIS_ASSERT(child_leaf != 0);
    JARVIS_ASSERT(child_leaf != parent_leaf);

    // Tear down the CHILD address space completely.
    VMM::free_user_pages(child_pml4);
    PMM::free_page(child_pml4);

    // The child's VA must be gone (its USER-owned tables + data reclaimed).
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(va, child_pml4) == 0);

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
        if (virt[i] & PAGE_PRESENT) {
            PMM::free_page(pml4);
            JARVIS_FAIL("UNEXPECTED PRESENT entry %u = 0x%x", i, virt[i]);
        }
    }

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
// Input: parent kernel task (prio 11, is_user_ set) with a USER-owned data
// page mapped at PROBE_VA; the dispatched lambda calls TaskControlBlock::
// clone() (real fork path), then walks every present user entry of both
// address spaces at all four levels.
// Expect: every user table phys differs at PML4/PDPT/PD/PT; the child data
// leaf is a fresh copy (child_leaf != parent_leaf); kernel entries mirror
// the kernel PML4; the parent mapping survives the child's teardown.
// Depends: kernel::TaskControlBlock, kernel::Scheduler, kernel::memory::VMM
JARVIS_TEST(fork_deep_copy_child_tables_independent, "PRE: none | POST: none") {
    static uint64_t g_ran = 0;
    static uint64_t g_all_indep = 0;
    static uint64_t g_leaf_diff = 0;
    static uint64_t g_parent_ok = 0;
    constexpr uint64_t PROBE_VA = 0x20000000;

    auto *parent = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            uint64_t regs[22] = {};
            regs[17] = 0x1000;
            regs[18] = arch::SEG_USER_CODE;
            regs[19] = arch::RFLAGS_DEFAULT;
            regs[20] = 0x80000000;
            regs[21] = arch::SEG_USER_DATA;

            auto *child = TaskControlBlock::clone(regs);
            if (child == nullptr)
                return;
            g_all_indep = 1;
            g_leaf_diff = 0;

            auto *pv = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                    (self->page_table_ & ~0xFFFULL));
            auto *cv = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                    (child->page_table_ & ~0xFFFULL));
            auto *kv = reinterpret_cast<uint64_t *>(
                arch::HHDM_OFFSET + (VMM::get_kernel_pml4() & ~0xFFFULL));

            for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i) {
                uint64_t p_ent = pv[i];
                uint64_t c_ent = cv[i];
                if (!(p_ent & PAGE_PRESENT)) {
                    if (c_ent & PAGE_PRESENT)
                        g_all_indep = 0;
                    continue;
                }
                if (!(c_ent & PAGE_PRESENT) ||
                    (c_ent & ~0xFFFULL) == (p_ent & ~0xFFFULL)) {
                    g_all_indep = 0;
                    continue;
                }
                auto *ppdpt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                           (p_ent & ~0xFFFULL));
                auto *cpdpt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                           (c_ent & ~0xFFFULL));
                for (size_t j = 0; j < 512; ++j) {
                    uint64_t p_pd = ppdpt[j];
                    uint64_t c_pd = cpdpt[j];
                    if (!(p_pd & PAGE_PRESENT)) {
                        if (c_pd & PAGE_PRESENT)
                            g_all_indep = 0;
                        continue;
                    }
                    if ((p_pd & PAGE_HUGE) || (c_pd & PAGE_HUGE)) {
                        if (!(c_pd & PAGE_PRESENT) ||
                            (c_pd & ~0xFFFULL) == (p_pd & ~0xFFFULL)) {
                            g_all_indep = 0;
                        }
                        continue;
                    }
                    if ((c_pd & ~0xFFFULL) == (p_pd & ~0xFFFULL)) {
                        g_all_indep = 0;
                        continue;
                    }
                    auto *ppd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                             (p_pd & ~0xFFFULL));
                    auto *cpd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                             (c_pd & ~0xFFFULL));
                    for (size_t k = 0; k < 512; ++k) {
                        uint64_t p_pt = ppd[k];
                        uint64_t c_pt = cpd[k];
                        if (!(p_pt & PAGE_PRESENT)) {
                            if (c_pt & PAGE_PRESENT)
                                g_all_indep = 0;
                            continue;
                        }
                        if ((p_pt & PAGE_HUGE) || (c_pt & PAGE_HUGE)) {
                            if (!(c_pt & PAGE_PRESENT) ||
                                (c_pt & ~0xFFFULL) == (p_pt & ~0xFFFULL)) {
                                g_all_indep = 0;
                            }
                            continue;
                        }
                        if ((c_pt & ~0xFFFULL) == (p_pt & ~0xFFFULL)) {
                            g_all_indep = 0;
                            continue;
                        }
                        auto *ppt = reinterpret_cast<uint64_t *>(
                            arch::HHDM_OFFSET + (p_pt & ~0xFFFULL));
                        auto *cpt = reinterpret_cast<uint64_t *>(
                            arch::HHDM_OFFSET + (c_pt & ~0xFFFULL));
                        for (size_t l = 0; l < 512; ++l) {
                            uint64_t p_leaf = ppt[l];
                            uint64_t c_leaf = cpt[l];
                            if (!(p_leaf & PAGE_PRESENT)) {
                                if (c_leaf & PAGE_PRESENT)
                                    g_all_indep = 0;
                                continue;
                            }
                            if (!(c_leaf & PAGE_PRESENT) ||
                                (c_leaf & ~0xFFFULL) == (p_leaf & ~0xFFFULL)) {
                                g_all_indep = 0;
                            }
                        }
                    }
                }
            }

            // Kernel entries mirror the kernel PML4 in the child.
            for (size_t i = arch::PML4_KERNEL_START; i < arch::PML4_ENTRIES; ++i) {
                if (cv[i] != kv[i])
                    g_all_indep = 0;
            }

            uint64_t p_leaf =
                VMM::virt_to_phys_in_pml4(PROBE_VA, self->page_table_);
            uint64_t c_leaf =
                VMM::virt_to_phys_in_pml4(PROBE_VA, child->page_table_);
            if (c_leaf == 0 || c_leaf == p_leaf) {
                g_leaf_diff = 0;
            } else {
                g_leaf_diff = 1;
            }

            child->cleanup();
            delete child;

            // Parent mapping survives the child's teardown.
            if (VMM::virt_to_phys_in_pml4(PROBE_VA, self->page_table_) ==
                p_leaf) {
                g_parent_ok = 1;
            } else {
                g_parent_ok = 0;
            }
            g_ran = 1;
        },
        11, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->is_user_ = true;           // simulate a user parent for clone()
    parent->user_stack_ = 0x80000000;  // mark as user-like for clone path
    parent->user_stack_size_ = 32_KiB; // clone() needs a real size to succeed
    uint64_t phys = PMM::alloc_user_page();
    JARVIS_ASSERT(phys != 0);
    VMM::map_page_in_pml4(PROBE_VA, phys, true, parent->page_table_);

    Scheduler::add_task(*parent);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(parent);
    // The parent self-terminated and is owned by the zombie list.  Reclaim it
    // BEFORE asserting so a failure cannot leak the parent TCB.
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(1ULL, g_ran);
    JARVIS_ASSERT_EQ(1ULL, g_all_indep);
    JARVIS_ASSERT_EQ(1ULL, g_leaf_diff);
    JARVIS_ASSERT_EQ(1ULL, g_parent_ok);
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
    static uint64_t g_ran = 0;
    static uint64_t g_leaf_diff = 0;
    static uint64_t g_parent_entry_ok = 0;
    static uint64_t g_parent_leaf_ok = 0;
    constexpr uint64_t PROBE_VA = 0x20001000;

    uint64_t free_before = PMM::free_pages_ref();

    auto *parent = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            uint64_t regs[22] = {};
            regs[17] = 0x1000;
            regs[18] = arch::SEG_USER_CODE;
            regs[19] = arch::RFLAGS_DEFAULT;
            regs[20] = 0x80000000;
            regs[21] = arch::SEG_USER_DATA;

            auto *child = TaskControlBlock::clone(regs);
            if (child == nullptr)
                return;
            size_t pml4_idx = (PROBE_VA >> PML4_SHIFT) & 0x1FF;
            auto *pv = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                    (self->page_table_ & ~0xFFFULL));
            uint64_t parent_pdpt = pv[pml4_idx] & ~0xFFFULL;
            uint64_t parent_leaf =
                VMM::virt_to_phys_in_pml4(PROBE_VA, self->page_table_);
            uint64_t child_leaf =
                VMM::virt_to_phys_in_pml4(PROBE_VA, child->page_table_);
            if (child_leaf == 0 || child_leaf == parent_leaf) {
                g_leaf_diff = 0;
            } else {
                g_leaf_diff = 1;
            }

            // Tear down the child: cleanup() frees its deep-copied tables
            // AND the copied data page (free_user_pages semantics).
            child->cleanup();
            delete child;

            // Parent survives: PML4->PDPT entry unchanged, VA still resolves.
            if ((pv[pml4_idx] & ~0xFFFULL) == parent_pdpt) {
                g_parent_entry_ok = 1;
            } else {
                g_parent_entry_ok = 0;
            }
            if (VMM::virt_to_phys_in_pml4(PROBE_VA, self->page_table_) ==
                parent_leaf) {
                g_parent_leaf_ok = 1;
            } else {
                g_parent_leaf_ok = 0;
            }
            g_ran = 1;
        },
        11, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->is_user_ = true;           // simulate a user parent for clone()
    parent->user_stack_ = 0x80000000;  // mark as user-like for clone path
    parent->user_stack_size_ = 32_KiB; // clone() needs a real size to succeed
    uint64_t phys = PMM::alloc_user_page();
    JARVIS_ASSERT(phys != 0);
    VMM::map_page_in_pml4(PROBE_VA, phys, true, parent->page_table_);

    Scheduler::add_task(*parent);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(parent);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(1ULL, g_ran);
    JARVIS_ASSERT_EQ(1ULL, g_leaf_diff);
    JARVIS_ASSERT_EQ(1ULL, g_parent_entry_ok);
    JARVIS_ASSERT_EQ(1ULL, g_parent_leaf_ok);
    // Full cycle returns to the PMM baseline: no leak, no double-free.
    JARVIS_ASSERT_FMT(PMM::free_pages_ref() == free_before,
                      "PMM delta %ld pages after fork deep-copy teardown",
                      (long)(PMM::free_pages_ref() - free_before));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-7 — TaskControlBlock::page_table_shared_ no longer
// exists (compile-time static_assert) and a real clone() produces a child
// whose page_table_ is a fresh deep copy (never equal to the parent's PML4).
// Input: a REAL dispatched kernel task (prio 11) with a user-like TCB calls
// clone(); capture the child's page_table_.
// Expect: build succeeds with the field removed (static_assert passes);
// child->page_table_ != 0 and != parent->page_table_; the child stack
// exists; teardown leaves zero task delta.
// Depends: kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(fork_page_table_shared_flag_absent, "PRE: none | POST: none") {
    static uint64_t g_child_pt = 0;
    static uint64_t g_child_stack = 0;
    static uint64_t g_child_ok = 0;
    static uint64_t g_parent_pt = 0;

    auto *parent = TaskControlBlock::create(
        []() {
            uint64_t regs[22] = {};
            regs[17] = 0x1000;
            regs[18] = arch::SEG_USER_CODE;
            regs[19] = arch::RFLAGS_DEFAULT;
            regs[20] = 0x80000000;
            regs[21] = arch::SEG_USER_DATA;

            auto *child = TaskControlBlock::clone(regs);
            if (child == nullptr)
                return;
            g_child_pt = child->page_table_;
            g_child_stack = child->user_stack_;
            g_child_ok = (g_child_pt != 0 && g_child_stack != 0) ? 1 : 0;
            child->cleanup();
            delete child;
        },
        11, 10);
    JARVIS_ASSERT(parent != nullptr);
    g_parent_pt = parent->page_table_;
    parent->is_user_ = true;           // simulate a user parent for clone()
    parent->user_stack_ = 0x80000000;  // mark as user-like for clone path
    parent->user_stack_size_ = 32_KiB; // clone() needs a real size to succeed
    Scheduler::add_task(*parent);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(parent);
    // The parent self-terminated and is owned by the zombie list.  Reclaim it
    // BEFORE asserting so a failure cannot leak the parent TCB.
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(1ULL, g_child_ok);
    JARVIS_ASSERT(g_child_pt != 0);
    JARVIS_ASSERT(g_child_pt != g_parent_pt);
    JARVIS_ASSERT(g_child_stack != 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all PML4 clone / fork page table tests.
// Input: None
// Expect: All tests registered
// Depends: test framework
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
}
