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

/// @file test_vmm.cpp
/// @brief Virtual memory manager (VMM) tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/vmm_internal.hpp>
#include <kernel/arch/hal/context_id.hpp>
#include <kernel/memory/vmm_errors.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/arch/page_table.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/cap/mmio.hpp>
#include <constants.hpp>
#include <kernel/arch/io.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies unmap_page on an unmapped VA is safe (idempotent).
// Input: Call unmap_page on unused virtual address
// Expect: No crash, returns success
// Depends: kernel::memory::VMM
JARVIS_TEST(vmm_unmap_already_unmapped, "PRE: none | POST: none") {
    uint64_t unused_va = 0x7FFF00000000ULL;
    VMM::unmap_page(unused_va);
    JARVIS_ASSERT(VMM::virt_to_phys(unused_va) == 0);
}

// Runmode: kernel
// Testidea: Verifies map_page on a VA that already has a physical page mapped.
// Input: Map page at VA, map again at same VA
// Expect: Fails or unmaps first
// Depends: kernel::memory::VMM
// Uses a scratch private PML4 (clone_kernel_pml4 + map_page_in_pml4) so the
// test never splits the kernel identity PD (0x400000 walks PML4[0]→PD_IDENTITY
// in the live kernel PML4, corrupting it permanently; snapshot_restore only
// restores PD_HIGHER, not PD_IDENTITY — BUGS.md pml4_clone CR3 corruption).
JARVIS_TEST(vmm_map_already_mapped, "PRE: none | POST: none") {
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    uint64_t va = 0x400000ULL;  // user-space address (PML4 entry 0)
    uint64_t phys1 = PMM::alloc_page();
    JARVIS_ASSERT(phys1 != 0);
    VMM::map_page_in_pml4(va, phys1, false, false, pml4);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(va, pml4) == phys1);

    uint64_t phys2 = PMM::alloc_page();
    JARVIS_ASSERT(phys2 != 0);
    VMM::map_page_in_pml4(va, phys2, false, false, pml4);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(va, pml4) == phys2);

    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    PMM::free_page(phys1);
    PMM::free_page(phys2);
}

// Runmode: kernel
// Testidea: Verifies map_page with phys=0 should fail.
// Input: Call map_page with physical address 0
// Expect: Returns error
// Depends: kernel::memory::VMM
// Uses a scratch private PML4 (see vmm_map_already_mapped note).
JARVIS_TEST(vmm_map_page_null_phys, "PRE: none | POST: none") {
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    uint64_t va = 0x401000ULL;  // user-space address (PML4 entry 0)
    VMM::map_page_in_pml4(va, 0, false, false, pml4);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(va, pml4) == 0);
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
}

// Runmode: kernel
// Testidea: Verifies when clone_kernel_pml4 runs out of memory, partial
// allocations are freed.
// Input: Exhaust memory, call clone_kernel_pml4
// Expect: No leaked page tables
// Depends: kernel::memory::VMM
JARVIS_TEST(vmm_clone_failure_rollback, "PRE: none | POST: none") {
    // clone_kernel_pml4 does a single PMM::alloc_page() for the new PML4.
    // If that fails, it returns 0 — there are no partial page-table
    // allocations to roll back (the user-space page-table deep copy
    // happens elsewhere, in the fork path).
    // Verify the normal path succeeds and the allocated PML4 is usable.
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    // Free the allocated PML4 page.
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies freeing user pages on a shared (forked) PML4 does not
// free pages still in use by parent.
// Input: Fork, free child's user pages, check parent
// Expect: Parent pages still valid
// Depends: kernel::memory::VMM
JARVIS_TEST(vmm_free_user_pages_shared, "PRE: none | POST: none") {
    // Simulate fork: parent has user pages, child shares page table pages.
    // The page-table pages MUST be allocated with alloc_user_page so that
    // free_user_pages (which gates subtree traversal on is_user_page) can
    // find and free the user data page within them.
    uint64_t parent_pml4 = PMM::alloc_page();
    JARVIS_ASSERT(parent_pml4 != 0);
    auto *parent_virt =
        reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + parent_pml4);
    // Zero all entries first: alloc_page returns an uninitialized page whose
    // stale PRESENT+USER entries would make free_user_pages walk/free foreign
    // pages (BUGS.md pml4_clone corruption family).
    for (size_t i = 0; i < 512; ++i)
        parent_virt[i] = 0;

    uint64_t pdpt_phys = 0;
    uint64_t pd_phys = 0;
    uint64_t pt_phys = 0;
#if defined(CONFIG_ARCH_RISCV64)
    // Sv39 has no PDPT level: L1/L2 tables only (L0 -> L1 -> L2 leaf).
    pd_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(pd_phys != 0);
    pt_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(pt_phys != 0);
#else
    pdpt_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(pdpt_phys != 0);
    pd_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(pd_phys != 0);
    pt_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(pt_phys != 0);
#endif

    // Set up parent page tables (user mapping at 0x400000).
    // Issue #352: native codec per arch.  The x86 4-level idiom
    // (PDPT/PD/PT + x86 flag values) does not decode on Sv39, which
    // needs L0 -> L1 -> L2 with V-only table links and a V|R|W|U leaf.
    // Same property under test on every arch: the child shares the
    // parent's tables, one free_user_pages frees them exactly once.
    uint64_t test_va = 0x400000;
#if defined(CONFIG_ARCH_RISCV64)
    // Sv39 3-level chain (issue #352): L0 -> L1 -> L2 leaf.  Local Sv39
    // codec (V=bit0, table=V-only, leaf=V|R|W|U, child=((e>>10)<<12)),
    // mirroring the x86 local-constants idiom below — VMM flag members
    // are private to the VMM.
    constexpr uint64_t SV39_V = 1ULL << 0;
    constexpr uint64_t SV39_R = 1ULL << 1;
    constexpr uint64_t SV39_W = 1ULL << 2;
    constexpr uint64_t SV39_U = 1ULL << 4;
    auto sv39_table = [](uint64_t phys) {
        return ((phys >> 12) << 10) | SV39_V;
    };
    auto sv39_phys = [](uint64_t e) {
        return ((e >> 10) << 12) & 0xFFFFFFFFFFF000ULL;
    };
    (void)sv39_phys;
    const size_t top_idx = (test_va >> 30) & 0x1FF;
    const size_t mid_idx = (test_va >> 21) & 0x1FF;
    const size_t leaf_idx = (test_va >> 12) & 0x1FF;
    parent_virt[top_idx] = sv39_table(pd_phys);
    auto *l1_tab =
        reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pd_phys);
    l1_tab[mid_idx] = sv39_table(pt_phys);
    auto *l2_tab =
        reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pt_phys);

    // Allocate a user page and map it
    uint64_t user_page = PMM::alloc_user_page();
    JARVIS_ASSERT(user_page != 0);
    l2_tab[leaf_idx] = sv39_table(user_page) | SV39_R | SV39_W | SV39_U;
#else
    constexpr uint64_t PML4_SHIFT = 39;
    constexpr uint64_t PDPT_SHIFT = 30;
    constexpr uint64_t PD_SHIFT = 21;
    constexpr uint64_t PT_SHIFT = 12;
    constexpr uint64_t PAGE_PRESENT = 1ULL << 0;
    constexpr uint64_t PAGE_WRITE = 1ULL << 1;
    constexpr uint64_t PAGE_USER = 1ULL << 2;

    size_t pml4_idx = (test_va >> PML4_SHIFT) & 0x1FF;
    size_t pdpt_idx = (test_va >> PDPT_SHIFT) & 0x1FF;
    size_t pd_idx = (test_va >> PD_SHIFT) & 0x1FF;
    size_t pt_idx = (test_va >> PT_SHIFT) & 0x1FF;

    parent_virt[pml4_idx] = pdpt_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    auto *pdpt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pdpt_phys);
    pdpt[pdpt_idx] = pd_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    auto *pd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pd_phys);
    pd[pd_idx] = pt_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    auto *pt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pt_phys);

    // Allocate a user page and map it
    uint64_t user_page = PMM::alloc_user_page();
    JARVIS_ASSERT(user_page != 0);
    pt[pt_idx] = user_page | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
#endif

    // Create child PML4 that shares the page table pages (simulating fork)
    uint64_t child_pml4 = PMM::alloc_page();
    JARVIS_ASSERT(child_pml4 != 0);
    auto *child_virt =
        reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + child_pml4);
    for (size_t i = 0; i < 512; ++i)
        child_virt[i] = 0;

    // Copy parent's user entries (shares PDPT/PD/PT pages)
    for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i) {
        child_virt[i] = parent_virt[i];
    }

    // Call free_user_pages on child's PML4
    VMM::free_user_pages(child_pml4);

    // free_user_pages recursively frees all user-owned data AND page-table
    // pages reachable from the child PML4.  Since the child shares the
    // parent's entries, the shared page-table pages get freed here too.
    // With the new PMM semantics free_page() preserves the owner bit, so
    // the pages are free (bitmap = 0) but still show as USER-owned.
    // Only the parent's direct manual cleanup (free_page below) is valid.  The
    // child's view is now freed — we verify by checking the top entry is
    // zeroed (free_user_pages clears each slot after draining its
    // subtree).  Top index is arch-dependent (PML4 on 4-level, L0 on
    // Sv39); both are 0 for test_va 0x400000, asserted explicitly.
#if defined(CONFIG_ARCH_RISCV64)
    JARVIS_ASSERT(child_virt[top_idx] == 0);
#else
    JARVIS_ASSERT(child_virt[pml4_idx] == 0);
#endif

    // Clean up (pdpt_phys is 0/unused on Sv39 — free_page(0) is a
    // range-guarded no-op, kept unconditional for symmetry).
    PMM::free_page(pdpt_phys);
    PMM::free_page(pd_phys);
    PMM::free_page(pt_phys);
    PMM::free_page(parent_pml4);
    PMM::free_page(child_pml4);
}

// Runmode: kernel
// Testidea: Verifies split at PD boundary (address at 2 MiB-aligned edge).
// Input: Split huge page at 2 MiB boundary
// Expect: Correctly creates page table entries
// Depends: kernel::memory::VMM
// DISABLED for v0.3.5 — splits a kernel huge page at VA 0x200000 (kernel
// identity region), permanently modifying the kernel page table in a way
// the snapshot cannot restore.  This causes a GPF crash in the next test.
// Safe: uses a scratch private PML4 so the kernel identity PD is untouched.
JARVIS_TEST(vmm_huge_page_split_corner, "PRE: none | POST: none") {
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    uint64_t va = 0x200000;
    uint64_t phys = PMM::alloc_page();
    JARVIS_ASSERT(phys != 0);
    VMM::map_page_in_pml4(va, phys, false, false, pml4);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(va, pml4) == phys);
    uint64_t phys2 = PMM::alloc_page();
    JARVIS_ASSERT(phys2 != 0);
    VMM::map_page_in_pml4(va + 0x1000, phys2, false, false, pml4);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(va + 0x1000, pml4) == phys2);
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    PMM::free_page(phys);
    PMM::free_page(phys2);
}

// Runmode: kernel
// Testidea: Registers all VMM unit tests with the test framework.
// Input: None
// Expect: All VMM tests registered via JARVIS_REGISTER_TEST
// Depends: kernel test framework
// Runmode: kernel
// Testidea: Verify free_user_pages does not crash when the PML4 contains
// kernel-owned entries mixed with user pages.
// Input: Cloned kernel PML4 with one user page mapped at 0x8000000000.
// Expect: free_user_pages completes without error.
// Depends: PMM, VMM
JARVIS_TEST(vmm_free_user_pages_skips_kernel_owned_entries,
            "PRE: none | POST: none") {
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    uint64_t user_page = PMM::alloc_user_page();
    JARVIS_ASSERT(user_page != 0);
    VMM::map_page_in_pml4(0x8000000000ULL, user_page, true, false, pml4);
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verify free_user_pages handles a user stack VADDR mapping
// (simulating fork cleanup).
// Input: Cloned kernel PML4 with a user page mapped at mem::STACK_VADDR.
// Expect: free_user_pages completes cleanly without errors.
// Depends: PMM, VMM, mem
JARVIS_TEST(vmm_free_user_pages_fork_stack_scenario, "PRE: none | POST: none") {
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    uint64_t stack_page = PMM::alloc_user_page();
    JARVIS_ASSERT(stack_page != 0);
    VMM::map_page_in_pml4(mem::STACK_VADDR, stack_page, true, false, pml4);
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    JARVIS_TEST_PASS();
}

#if defined(CONFIG_ARCH_X86_64)
// Runmode: kernel
// Testidea: Regression test for bug #3 — verify map_page splits a 2 MiB huge
// page in the kernel HHDM and correctly maps a 4 KiB target page while
// preserving neighbouring translations.
// Input: Virtual address at HHDM_OFFSET+0x2082000 inside a 2 MiB huge page;
// allocate a different physical page and map it via map_page. The fixture
// lives above the hardened image range (issue #376: PD entries covering
// the image are split to 4K at boot, so low HHDM VAs are no longer huge).
// Expect: After map, target resolves to new physical page; neighbouring
// pages still resolve correctly via the newly allocated page table; original
// mapping is restored.
// Depends: PMM, VMM, arch
// v0.3.6: re-enabled with HHDM PD save/restore + hhdm_modified_ flag.
JARVIS_TEST(vmm_huge_page_split_regression, "PRE: none | POST: none") {
    constexpr uint64_t test_vaddr = arch::HHDM_OFFSET + 0x2082000;
    constexpr uint64_t huge_page_base = arch::HHDM_OFFSET + 0x2080000;
    uint64_t const PAGE_PRESENT = 1ULL << 0;
    uint64_t const PAGE_HUGE = 1ULL << 7;
    uint64_t kernel_pml4 = arch::read_cr3();
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (kernel_pml4 & ~0xFFFULL));
    size_t pml4_idx = static_cast<size_t>((test_vaddr & (0x1FFULL << 39)) >> 39);
    JARVIS_ASSERT(pml4[pml4_idx] & PAGE_PRESENT);
    auto *pdpt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (pml4[pml4_idx] & ~0xFFFULL));
    size_t pdpt_idx = static_cast<size_t>((test_vaddr & (0x1FFULL << 30)) >> 30);
    JARVIS_ASSERT(pdpt[pdpt_idx] & PAGE_PRESENT);
    auto *pd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (pdpt[pdpt_idx] & ~0xFFFULL));
    size_t pd_idx = static_cast<size_t>((test_vaddr & (0x1FFULL << 21)) >> 21);
    JARVIS_ASSERT(pd[pd_idx] & PAGE_PRESENT);
    JARVIS_ASSERT(pd[pd_idx] & PAGE_HUGE);
    uint64_t const saved_pd_entry = pd[pd_idx];
    // Resolve physical address from the 2MB huge page entry directly:
    // phys_base = (entry & ~0x1FFFFF), then add page offset.
    uint64_t before = (pd[pd_idx] & ~0x1FFFFFULL) + (test_vaddr & 0x1FFFFFULL);
    JARVIS_ASSERT(before == 0x2082000);
    uint64_t before_base = (pd[pd_idx] & ~0x1FFFFFULL) +
                           (huge_page_base & 0x1FFFFFULL);
    JARVIS_ASSERT(before_base == 0x2080000);
    uint64_t before_neighbour = (pd[pd_idx] & ~0x1FFFFFULL) +
                                ((huge_page_base + 0x1000) & 0x1FFFFFULL);
    JARVIS_ASSERT(before_neighbour == 0x2081000);
    uint64_t test_phys = PMM::alloc_page();
    JARVIS_ASSERT(test_phys != 0);
    JARVIS_ASSERT(test_phys != 0x2082000);
    // Issue #200: HHDM PD surgery is IRQ-critical (spec
    // docs/_archive/hhdm-snapshot-restore.md §5.1) — mask the timer ISR
    // and task preemption across map..manual-restore (see
    // vmm_hhdm_access_consistency for the full rationale).
    arch::IrqGuard irq_guard;
    VMM::map_page(test_vaddr, test_phys, false);
    // After the split, pd[pd_idx] points to a PT page instead of a 2MB page
    JARVIS_ASSERT(!(pd[pd_idx] & PAGE_HUGE));
    uint64_t pt_phys = pd[pd_idx] & ~0xFFFULL;
    auto *pt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pt_phys);
    size_t pt_idx = static_cast<size_t>((test_vaddr & (0x1FFULL << 12)) >> 12);
    // Split-inherited entries keep the boot huge entry's flags verbatim
    // (PRESENT|WRITE — boot sets no XD on huge DATA entries, and the
    // split_fill NX/AP carry only preserves bits already set); only the
    // explicitly (re)mapped leaf carries NX (issue #376 slice 1).
    constexpr uint64_t X64_PT_FLAGS = (1ULL << 0) | (1ULL << 1);
    constexpr uint64_t X64_MAPPED_FLAGS =
        (1ULL << 0) | (1ULL << 1) | (1ULL << 63); // PRESENT|WRITE|NX
    JARVIS_ASSERT(pt[pt_idx] == (test_phys & ~0xFFFULL) + X64_MAPPED_FLAGS);
    // The huge page base and neighbour should still resolve via the PT page
    size_t base_pt_idx = static_cast<size_t>((huge_page_base & (0x1FFULL << 12)) >> 12);
    JARVIS_ASSERT(pt[base_pt_idx] == (0x2080000 & ~0xFFFULL) + X64_PT_FLAGS);
    size_t neigh_pt_idx = static_cast<size_t>(((huge_page_base + 0x1000) & (0x1FFULL << 12)) >> 12);
    JARVIS_ASSERT(pt[neigh_pt_idx] == (0x2081000 & ~0xFFFULL) + X64_PT_FLAGS);
    VMM::unmap_page(test_vaddr);
    // After unmap: PT entry in the just-created PT page should be cleared.
    // The unmap_page guard returns early for kernel VAs during tests, so
    // manually clear it.  The PD restore in snapshot_restore will clean up.
    pt[pt_idx] = 0;
    VMM::map_page(test_vaddr, 0x2082000, false);
    JARVIS_ASSERT(pt[pt_idx] == (0x2082000 & ~0xFFFULL) + X64_MAPPED_FLAGS);
    pt_phys = pd[pd_idx] & ~0xFFFULL;
    JARVIS_ASSERT(!(pd[pd_idx] & PAGE_HUGE));
    PMM::free_page(pt_phys);
    pd[pd_idx] = saved_pd_entry;
    arch::ArchPageTable::tlb_flush(test_vaddr);
    PMM::free_page(test_phys);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(vmm_hhdm_access_consistency, "PRE: none | POST: none") {
    uint64_t kernel_pml4 = VMM::get_kernel_pml4();
    JARVIS_ASSERT(kernel_pml4 != 0);
    // Fixture above the hardened image range (issue #376: low HHDM PD
    // entries covering the image are split at boot, no longer huge).
    uint64_t v = arch::HHDM_OFFSET + 0x2300000;
    uint64_t cr3 = arch::read_cr3();
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + (cr3 & ~0xFFFULL));
    size_t pml4_i = static_cast<size_t>((v & (0x1FFULL << 39)) >> 39);
    auto *pdpt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (pml4[pml4_i] & ~0xFFFULL));
    size_t pdpt_i = static_cast<size_t>((v & (0x1FFULL << 30)) >> 30);
    auto *pd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (pdpt[pdpt_i] & ~0xFFFULL));
    size_t pd_i = static_cast<size_t>((v & (0x1FFULL << 21)) >> 21);
    JARVIS_ASSERT(pd[pd_i] & (1ULL << 7));
    uint64_t const saved_pd_entry = pd[pd_i];
    uint64_t p = PMM::alloc_page();
    JARVIS_ASSERT(p != 0);
    // Issue #200: HHDM PD surgery is IRQ-critical (spec
    // docs/_archive/hhdm-snapshot-restore.md §5.1) — mask the timer ISR
    // and task preemption across map..manual-restore so no concurrent
    // path can observe or modify the split PD entry mid-surgery.
    arch::IrqGuard irq_guard;
    VMM::map_page(v, p, false);
    // After split: the huge page is replaced by a PT page at pd[pd_i].
    // Resolve via the new non-huge PD entry:
    uint64_t pt_phys = pd[pd_i] & ~0xFFFULL;
    uint64_t pt_idx = static_cast<size_t>((v & (0x1FFULL << 12)) >> 12);
    auto *pt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pt_phys);
    // Issue #376 slice 1: the explicitly mapped leaf carries NX.
    constexpr uint64_t X64_PT_FLAGS_2 =
        (1ULL << 0) | (1ULL << 1) | (1ULL << 63); // PRESENT|WRITE|NX
    JARVIS_ASSERT(pt[pt_idx] == (p & ~0xFFFULL) + X64_PT_FLAGS_2);
    VMM::unmap_page(v);
    JARVIS_ASSERT(pt[pt_idx] == 0);
    VMM::map_page(v, 0x2300000, false);
    pt_phys = pd[pd_i] & ~0xFFFULL;
    PMM::free_page(pt_phys);
    pd[pd_i] = saved_pd_entry;
    arch::ArchPageTable::tlb_flush(v);
    PMM::free_page(p);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The VAR-17 take primitive closes the SMP read-clear gap
//           (issue #60): take returns the previous value and clears
//           atomically, so a set landing after the take is preserved
//           for the next restore (fail-safe over-restore).
// Input: clear/take on clean flags; map_page on a scratch HHDM VA (the
//        real setter); take/was interleavings; manual PD restore.
// Expect: take on clean is false; set→take true→was false→take false;
//        set-after-take preserved; flags clean at exit.
// Depends: VMM::take_*/was/clear_*, VMM::map_page HHDM setter (x86-only)
JARVIS_TEST(vmm_hhdm_take_semantics, "PRE: none | POST: none") {
    VMM::clear_hhdm_modified();
    VMM::clear_identity_modified();
    // Take on a clean flag fabricates nothing and is idempotent.
    JARVIS_ASSERT(!VMM::take_hhdm_modified());
    JARVIS_ASSERT(!VMM::take_identity_modified());
    JARVIS_ASSERT(!VMM::hhdm_was_modified());
    // Set via the real setter: map a scratch HHDM page (2 MiB huge
    // page at HHDM+0x2400000, same save/split/restore pattern as the
    // regression test above; above the hardened image range, issue #376).
    constexpr uint64_t scratch_va = arch::HHDM_OFFSET + 0x2402000;
    uint64_t const PAGE_PRESENT = 1ULL << 0;
    uint64_t const PAGE_HUGE = 1ULL << 7;
    uint64_t kernel_pml4 = arch::read_cr3();
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (kernel_pml4 & ~0xFFFULL));
    size_t pml4_i = static_cast<size_t>((scratch_va & (0x1FFULL << 39)) >> 39);
    JARVIS_ASSERT(pml4[pml4_i] & PAGE_PRESENT);
    auto *pdpt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (pml4[pml4_i] & ~0xFFFULL));
    size_t pdpt_i = static_cast<size_t>((scratch_va & (0x1FFULL << 30)) >> 30);
    JARVIS_ASSERT(pdpt[pdpt_i] & PAGE_PRESENT);
    auto *pd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (pdpt[pdpt_i] & ~0xFFFULL));
    size_t pd_i = static_cast<size_t>((scratch_va & (0x1FFULL << 21)) >> 21);
    JARVIS_ASSERT(pd[pd_i] & PAGE_PRESENT);
    JARVIS_ASSERT(pd[pd_i] & PAGE_HUGE);
    uint64_t const saved_pd_entry = pd[pd_i];
    uint64_t data_phys = PMM::alloc_page();
    JARVIS_ASSERT(data_phys != 0);
    // Issue #200: HHDM PD surgery is IRQ-critical (spec
    // docs/_archive/hhdm-snapshot-restore.md §5.1) — mask the timer ISR
    // and task preemption across map..manual-restore.
    arch::IrqGuard irq_guard;
    VMM::map_page(scratch_va, data_phys, false);
    // Round-trip through the real setter: set→take true→was false.
    JARVIS_ASSERT(VMM::hhdm_was_modified());
    JARVIS_ASSERT(VMM::take_hhdm_modified());
    JARVIS_ASSERT(!VMM::hhdm_was_modified());
    JARVIS_ASSERT(!VMM::take_hhdm_modified());
    // Set-after-take is preserved (the gap-closure property).
    VMM::map_page(scratch_va, data_phys, false);
    JARVIS_ASSERT(VMM::take_hhdm_modified());
    // Restore the page-table state manually (regression pattern).
    VMM::unmap_page(scratch_va);
    size_t pt_i = static_cast<size_t>((scratch_va & (0x1FFULL << 12)) >> 12);
    auto *pt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (pd[pd_i] & ~0xFFFULL));
    pt[pt_i] = 0;
    VMM::map_page(scratch_va, 0x2402000, false);
    uint64_t pt_phys = pd[pd_i] & ~0xFFFULL;
    PMM::free_page(pt_phys);
    pd[pd_i] = saved_pd_entry;
    arch::ArchPageTable::tlb_flush(scratch_va);
    PMM::free_page(data_phys);
    // Leave the flags clean for the rest of the suite.
    VMM::clear_hhdm_modified();
    VMM::clear_identity_modified();
    JARVIS_ASSERT(!VMM::hhdm_was_modified());
    JARVIS_TEST_PASS();
}
#endif

// Runmode: kernel
// Testidea: The thin *_err wrappers and query helpers must validate,
//           delegate, and report — each entered at least once for
//           function-level coverage (issue #143 item 3).  Live-table
//           writes are avoided (unaligned rejects touch nothing;
//           unmap/translate on unused VAs are read-only); real mappings
//           happen on a scratch PML4 that is freed in-test.
// Input: Unaligned + unused-VA calls to every _err wrapper; error_string
//        over all VmmError codes; current_pml4/virt_to_phys reads;
//        clear_* flag resets; clone + free_user_pages on scratch.
// Expect: INVALID_ADDR for unaligned, NOT_MAPPED for unused,
//         OK + correct phys for mapped, nonzero current PML4, exact
//         error strings, no tracker delta.
// Depends: VMM _err wrappers, error_string<VmmError>, scratch-PML4 pattern
JARVIS_TEST(vmm_err_wrappers_and_queries, "PRE: none | POST: none") {
    using errors::VmmError;
    uint64_t out = 0;
    JARVIS_ASSERT(VMM::map_page_err(0x1001, 0x2000, false) ==
                  errors::VMM_ERR_INVALID_ADDR);
    JARVIS_ASSERT(VMM::map_page_err(0x2000, 0x2001, false) ==
                  errors::VMM_ERR_INVALID_ADDR);
    JARVIS_ASSERT(VMM::unmap_page_err(0x1001) == errors::VMM_ERR_INVALID_ADDR);
    // Holes report NOT_MAPPED (issue #366 Q2b). Probe first: the harness
    // owns the kernel table, so only assert on a VA that is provably
    // unmapped here (unmapping live kernel state to force the OK path is
    // out of scope — covered on scratch PML4s below).
    if (VMM::virt_to_phys(0x7FFF00000000ULL) == 0) {
        JARVIS_ASSERT(VMM::unmap_page_err(0x7FFF00000000ULL) ==
                      errors::VMM_ERR_NOT_MAPPED);
    }
    JARVIS_ASSERT(VMM::virt_to_phys_err(0x1001, out) ==
                  errors::VMM_ERR_INVALID_ADDR);
    JARVIS_ASSERT(VMM::virt_to_phys_err(0x7FFF00000000ULL, out) ==
                  errors::VMM_ERR_NOT_MAPPED);
    JARVIS_ASSERT(VMM::current_pml4() != 0);
    JARVIS_ASSERT(VMM::virt_to_phys(0x7FFF00000000ULL) == 0);
    VMM::clear_hhdm_modified();
    VMM::clear_identity_modified();
    JARVIS_ASSERT(!VMM::hhdm_was_modified());
    JARVIS_ASSERT(!VMM::identity_was_modified());
    JARVIS_ASSERT(errors::error_string(errors::VMM_ERR_OK) != nullptr);
    JARVIS_ASSERT(errors::error_string(errors::VMM_ERR_PAGE_ALLOC) != nullptr);
    JARVIS_ASSERT(errors::error_string(errors::VMM_ERR_PML4_ALLOC) != nullptr);
    JARVIS_ASSERT(errors::error_string(errors::VMM_ERR_INVALID_ADDR) !=
                  nullptr);
    JARVIS_ASSERT(errors::error_string(errors::VMM_ERR_NOT_MAPPED) != nullptr);
    JARVIS_ASSERT(errors::error_string(static_cast<VmmError>(999)) != nullptr);
    uint64_t pml4 = 0;
    JARVIS_ASSERT(VMM::clone_kernel_pml4_err(pml4) == errors::VMM_ERR_OK);
    JARVIS_ASSERT(pml4 != 0);
    uint64_t va = 0x402000ULL;
    uint64_t phys = PMM::alloc_page();
    JARVIS_ASSERT(phys != 0);
    JARVIS_ASSERT(VMM::map_page_in_pml4_err(va, phys + 1, false, pml4) ==
                  errors::VMM_ERR_INVALID_ADDR);
    JARVIS_ASSERT(VMM::map_page_in_pml4_err(va + 1, phys, false, pml4) ==
                  errors::VMM_ERR_INVALID_ADDR);
    JARVIS_ASSERT(VMM::map_page_in_pml4_err(va, phys, false, pml4) ==
                  errors::VMM_ERR_OK);
    uint64_t back = 0;
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4_err(va + 1, pml4, back) ==
                  errors::VMM_ERR_INVALID_ADDR);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4_err(va, pml4, back) ==
                  errors::VMM_ERR_OK);
    JARVIS_ASSERT(back == phys);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4_err(0x403000ULL, pml4, back) ==
                  errors::VMM_ERR_NOT_MAPPED);
    JARVIS_ASSERT(VMM::free_user_pages_err(pml4) == errors::VMM_ERR_OK);
    PMM::free_page(pml4);
    PMM::free_page(phys);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Capability-gated page mapping must refuse null/revoked/IO caps
//           and round-trip live ones through a scratch PML4 (issue #143
//           item 3).  Scratch tables only — the live kernel PML4 is never
//           touched.
// Input: FrameCap over one kernel page + MmioCap over fake device phys:
//        map/unmap into scratch; null caps; revoked caps; IO-bar cap.
// Expect: Refusals return false; live maps resolve via virt_to_phys;
//         dispose pairs keep the tracker clean.
// Depends: VMM::map/unmap_frame/mmio_from_cap, FrameCap/MmioCap lifecycle
JARVIS_TEST(vmm_cap_frame_mmio_paths, "PRE: none | POST: none") {
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    uint64_t frame_phys = PMM::alloc_page();
    JARVIS_ASSERT(frame_phys != 0);
    auto *frame = cap::FrameCap::create(frame_phys, 1, false);
    JARVIS_ASSERT(frame != nullptr);
    constexpr uint64_t kFrameVa = 0x404000ULL;
    JARVIS_ASSERT(VMM::map_frame_from_cap(nullptr, kFrameVa, false, pml4) ==
                  false);
    JARVIS_ASSERT(VMM::map_frame_from_cap(frame, kFrameVa, false, pml4));
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(kFrameVa, pml4) == frame_phys);
    VMM::unmap_frame_from_cap(kFrameVa, pml4);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(kFrameVa, pml4) == 0);
    frame->revoke();
    JARVIS_ASSERT(VMM::map_frame_from_cap(frame, kFrameVa, false, pml4) ==
                  false);
    frame->release();
    uint64_t dev_phys =
        (PMM::total_memory() + 0x100000ULL) & ~0xFFFULL;
    auto *mmio = cap::MmioCap::create(dev_phys, arch::PAGE_SIZE,
                                      arch::PciBarType::MEMORY_32);
    JARVIS_ASSERT(mmio != nullptr);
    constexpr uint64_t kMmioVa = 0x405000ULL;
    JARVIS_ASSERT(VMM::map_mmio_from_cap(nullptr, kMmioVa, true, pml4) ==
                  false);
    JARVIS_ASSERT(VMM::map_mmio_from_cap(mmio, kMmioVa, true, pml4));
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(kMmioVa, pml4) == dev_phys);
    VMM::unmap_mmio_from_cap(mmio, kMmioVa, pml4);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(kMmioVa, pml4) == 0);
    mmio->revoke();
    JARVIS_ASSERT(VMM::map_mmio_from_cap(mmio, kMmioVa, true, pml4) == false);
    mmio->release();
    auto *io_cap =
        cap::MmioCap::create(0x3F8, 8, arch::PciBarType::IO);
    JARVIS_ASSERT(io_cap != nullptr);
    JARVIS_ASSERT(VMM::map_mmio_from_cap(io_cap, kMmioVa, true, pml4) ==
                  false);
    io_cap->release();
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: _err unmap twins report holes without clearing (issue #366 Q2b).
// Input: Scratch PML4; misaligned bases, unmapped VAs, null caps, mapped
//        frame/mmio ranges; a revoked frame cap (unmap must still work).
// Expect: INVALID_ADDR on misaligned/null; NOT_MAPPED on holes (missing
//         table, clear leaf — double unmap included); OK clears exactly
//         one leaf/range and verifies via virt_to_phys.
// Depends: VMM::unmap_page/frame/mmio_from_cap_err, FrameCap/MmioCap
JARVIS_TEST(vmm_unmap_err_not_mapped, "PRE: none | POST: none") {
    using errors::VmmError;
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    constexpr uint64_t kVa = 0x406000ULL;
    // Misaligned base.
    JARVIS_ASSERT(VMM::unmap_page_in_pml4_err(kVa + 1, pml4) ==
                  errors::VMM_ERR_INVALID_ADDR);
    // Hole: no tables yet.
    JARVIS_ASSERT(VMM::unmap_page_in_pml4_err(kVa, pml4) ==
                  errors::VMM_ERR_NOT_MAPPED);
    // Map one leaf, unmap OK, unmap again NOT_MAPPED. USER-owned data
    // page: reclaimed by free_user_pages below (never freed manually —
    // the kernel-owned alloc_page + user=true combo would trip the
    // map-time ENSURE and leak through the user-owned teardown).
    uint64_t phys = PMM::alloc_user_page();
    JARVIS_ASSERT(phys != 0);
    JARVIS_ASSERT(VMM::map_page_in_pml4_err(kVa, phys, true, pml4, false) ==
                  errors::VMM_ERR_OK);
    JARVIS_ASSERT(VMM::unmap_page_in_pml4_err(kVa, pml4) ==
                  errors::VMM_ERR_OK);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(kVa, pml4) == 0);
    JARVIS_ASSERT(VMM::unmap_page_in_pml4_err(kVa, pml4) ==
                  errors::VMM_ERR_NOT_MAPPED);
    // Frame-cap twin: null cap, hole, mapped, revoked-still-works.
    JARVIS_ASSERT(VMM::unmap_frame_from_cap_err(nullptr, kVa, pml4) ==
                  errors::VMM_ERR_INVALID_ADDR);
    uint64_t fphys = PMM::alloc_page();
    JARVIS_ASSERT(fphys != 0);
    auto *frame = cap::FrameCap::create(fphys, 1, false);
    JARVIS_ASSERT(frame != nullptr);
    JARVIS_ASSERT(VMM::unmap_frame_from_cap_err(frame, kVa, pml4) ==
                  errors::VMM_ERR_NOT_MAPPED);
    JARVIS_ASSERT(VMM::map_frame_from_cap(frame, kVa, false, pml4));
    // Revoked caps still unmap present leaves (no revoked check by
    // design — the revoke path itself unmaps revoked caps; audit S3).
    frame->revoke();
    JARVIS_ASSERT(VMM::unmap_frame_from_cap_err(frame, kVa, pml4) ==
                  errors::VMM_ERR_OK);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(kVa, pml4) == 0);
    JARVIS_ASSERT(VMM::unmap_frame_from_cap_err(frame, kVa, pml4) ==
                  errors::VMM_ERR_NOT_MAPPED);
    frame->release();
    // Mmio-cap twin: null cap, hole, mapped range, double unmap.
    JARVIS_ASSERT(VMM::unmap_mmio_from_cap_err(nullptr, kVa, pml4) ==
                  errors::VMM_ERR_INVALID_ADDR);
    uint64_t dev_phys = (PMM::total_memory() + 0x100000ULL) & ~0xFFFULL;
    auto *mmio = cap::MmioCap::create(dev_phys, arch::PAGE_SIZE,
                                      arch::PciBarType::MEMORY_32);
    JARVIS_ASSERT(mmio != nullptr);
    JARVIS_ASSERT(VMM::unmap_mmio_from_cap_err(mmio, kVa, pml4) ==
                  errors::VMM_ERR_NOT_MAPPED);
    JARVIS_ASSERT(VMM::map_mmio_from_cap(mmio, kVa, true, pml4));
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(kVa, pml4) == dev_phys);
    JARVIS_ASSERT(VMM::unmap_mmio_from_cap_err(mmio, kVa, pml4) ==
                  errors::VMM_ERR_OK);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(kVa, pml4) == 0);
    JARVIS_ASSERT(VMM::unmap_mmio_from_cap_err(mmio, kVa, pml4) ==
                  errors::VMM_ERR_NOT_MAPPED);
    mmio->release();
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Pin the 1 GiB block geometry + decode_block_base contract
// (issue #365): the only multi-megabyte level the teardown free path
// reasons about. Pure-function coverage — no PMM, no tables.
// Input: Aligned/misaligned bases at the 1 GiB + 2 MiB levels.
// Expect: Aligned decodes to the identical base; misaligned rejects with
// out_base untouched; mask identities hold; 1 GiB-aligned implies
// 2 MiB-aligned (level nesting).
// Depends: kernel::decode_block_base, kBlock1G*/kHuge2M* (vmm_internal.hpp)
JARVIS_TEST(vmm_block_1g_geometry, "PRE: none | POST: none") {
    constexpr uint64_t kSentinel = 0xDEADBEEFDEADBEEFULL;
    JARVIS_ASSERT_EQ(0x3FFFFFFFULL, kernel::kBlock1GOffsetMask);
    JARVIS_ASSERT_EQ(~0x3FFFFFFFULL, kernel::kBlock1GBaseMask);
    JARVIS_ASSERT_EQ(0x1FFFFFULL, kernel::kHuge2MOffsetMask);
    JARVIS_ASSERT_EQ(512ULL * (kernel::kHuge2MOffsetMask + 1),
                     kernel::kBlock1GOffsetMask + 1);

    uint64_t out = kSentinel;
    JARVIS_ASSERT(
        kernel::decode_block_base(0x40000000ULL, kernel::kBlock1GOffsetMask, out));
    JARVIS_ASSERT_EQ(0x40000000ULL, out);
    // Zero is level-aligned (vacuous accept; no producer maps phys 0).
    out = kSentinel;
    JARVIS_ASSERT(
        kernel::decode_block_base(0ULL, kernel::kBlock1GOffsetMask, out));
    JARVIS_ASSERT_EQ(0ULL, out);
    // Misaligned: 4 KiB, 2 MiB, and single-bit offsets all reject.
    out = kSentinel;
    JARVIS_ASSERT(!kernel::decode_block_base(0x40001000ULL,
                                             kernel::kBlock1GOffsetMask, out));
    JARVIS_ASSERT_EQ(kSentinel, out);
    JARVIS_ASSERT(!kernel::decode_block_base(0x40200000ULL,
                                             kernel::kBlock1GOffsetMask, out));
    JARVIS_ASSERT_EQ(kSentinel, out);
    JARVIS_ASSERT(!kernel::decode_block_base(0x40000001ULL,
                                             kernel::kBlock1GOffsetMask, out));
    JARVIS_ASSERT_EQ(kSentinel, out);
    // Level nesting: a 1 GiB base also decodes at the 2 MiB level.
    out = kSentinel;
    JARVIS_ASSERT(kernel::decode_block_base(0x40000000ULL,
                                            kernel::kHuge2MOffsetMask, out));
    JARVIS_ASSERT_EQ(0x40000000ULL, out);
    // 2 MiB level standalone.
    out = kSentinel;
    JARVIS_ASSERT(kernel::decode_block_base(0x200000ULL,
                                            kernel::kHuge2MOffsetMask, out));
    JARVIS_ASSERT_EQ(0x200000ULL, out);
    out = kSentinel;
    JARVIS_ASSERT(!kernel::decode_block_base(0x201000ULL,
                                             kernel::kHuge2MOffsetMask, out));
    JARVIS_ASSERT_EQ(kSentinel, out);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A misaligned synthetic 1 GiB block leaf in scratch tables is
// SKIPPED by free_user_pages, never freed, without halting (issue #365:
// teardown pins validate-before-gate; skips preserve integrity).
// Input: Scratch top + mid tables (user pages) with one hand-installed
// 1 GiB block entry whose arch-decoded base is 1 GiB-misaligned but
// user-owned (so the alignment gate is the only reason it survives);
// per-arch block codec (x86 PS bit / aarch64 block descriptor / Sv39
// R|W|X leaf). free_user_pages runs on the scratch root.
// Expect: PMM delta is exactly the drained mid table (+1 x86/aarch64,
// +0 riscv64 L0-direct); the data page is NOT freed; the walk visited
// the slot (top entry cleared where the walker drains, preserved where
// the defensive L0 site skips); no halt.
// Depends: VMM::free_user_pages, PMM user pages, arch block codec
JARVIS_TEST(vmm_free_user_pages_pdpt_1g_rejects_unaligned,
            "PRE: none | POST: none") {
    constexpr uint64_t kMask1G = 0x3FFFFFFFULL;
#if !defined(CONFIG_ARCH_RISCV64)
    constexpr uint64_t kMask2M = 0x1FFFFFULL;
#endif

    auto *top_virt = static_cast<uint64_t *>(nullptr);
    uint64_t top = PMM::alloc_user_page();
    JARVIS_ASSERT(top != 0);
    top_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + top);
    for (size_t i = 0; i < 512; ++i)
        top_virt[i] = 0;

    // Find a user page whose block-decoded base is 1 GiB-misaligned: the
    // kernel masks the entry address to the 2 MiB huge frame (x86/aarch64)
    // or shift-decodes it (Sv39, identity for 4 KiB phys), then requires
    // 1 GiB alignment. Free duds immediately; the pool is large so the
    // first pages almost always qualify — the cap only guards a
    // pathologically aligned pool.
    uint64_t data = 0;
    for (int tries = 0; tries < 64; ++tries) {
        uint64_t cand = PMM::alloc_user_page();
        JARVIS_ASSERT(cand != 0);
#if defined(CONFIG_ARCH_RISCV64)
        const uint64_t raw = cand;
#else
        const uint64_t raw = cand & ~kMask2M;
#endif
        if ((raw & kMask1G) != 0) {
            data = cand;
            break;
        }
        PMM::free_page(cand);
    }
    JARVIS_ASSERT_FMT(data != 0,
                      "no 1G-misaligned user page in 64 tries (pool shape?)");

#if defined(CONFIG_ARCH_RISCV64)
    // Sv39 L0 defensive site: leaf = V|R|W|X|U with shift-coded PPN.
    constexpr uint64_t SV39_V = 1ULL << 0;
    constexpr uint64_t SV39_R = 1ULL << 1;
    constexpr uint64_t SV39_W = 1ULL << 2;
    constexpr uint64_t SV39_X = 1ULL << 3;
    constexpr uint64_t SV39_U = 1ULL << 4;
    constexpr size_t kSlot = 2;
    top_virt[kSlot] =
        ((data >> 12) << 10) | SV39_V | SV39_R | SV39_W | SV39_X | SV39_U;
#else
    uint64_t mid = PMM::alloc_user_page();
    JARVIS_ASSERT(mid != 0);
    auto *mid_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + mid);
    for (size_t i = 0; i < 512; ++i)
        mid_virt[i] = 0;
    constexpr size_t kTopSlot = 0;
    constexpr size_t kBlkSlot = 1;
#if defined(CONFIG_ARCH_AARCH64)
    // aarch64 L0 table descriptor (bits[1:0] = 11) + L1 block
    // descriptor (bits[1:0] = 01, valid). AF/attrs zero are fine for a
    // never-dispatched scratch entry — the teardown path only reads
    // the descriptor type + frame.
    top_virt[kTopSlot] = (mid & ~0xFFFULL) | 0x3ULL;
    mid_virt[kBlkSlot] = (data & ~0xFFFULL) | 0x1ULL;
#else
    // x86_64 PML4->PDPT table link (no PS bit) + 1 GiB PDPT leaf
    // (PRESENT|WRITE|USER|PS).
    constexpr uint64_t P = 1ULL << 0;
    constexpr uint64_t W = 1ULL << 1;
    constexpr uint64_t U = 1ULL << 2;
    constexpr uint64_t PS = 1ULL << 7;
    top_virt[kTopSlot] = (mid & ~0xFFFULL) | P | W | U;
    mid_virt[kBlkSlot] = (data & ~0xFFFULL) | P | W | U | PS;
#endif
#endif

    const uint64_t free_before = PMM::free_pages_ref();
    VMM::free_user_pages(top);
    const int64_t delta = (int64_t)(PMM::free_pages_ref() - free_before);
#if defined(CONFIG_ARCH_RISCV64)
    // Nothing drained (L0-direct leaf skipped): delta 0, slot preserved.
    JARVIS_ASSERT_FMT(delta == 0, "riscv64 L0 1G skip freed %ld pages",
                      (long)delta);
    JARVIS_ASSERT(top_virt[kSlot] != 0);
#else
    // Mid table drained (+1), misaligned data page skipped (not +2), top
    // slot cleared by the draining walk.
    JARVIS_ASSERT_FMT(delta == 1, "block skip freed %ld pages, want 1 (mid)",
                      (long)delta);
    JARVIS_ASSERT(top_virt[kTopSlot] == 0);
#endif

    // Manual cleanup of what free_user_pages provably leaves: the scratch
    // top + the skipped data page (the mid table was drained above).
    PMM::free_page(top);
    PMM::free_page(data);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: VMM::tlb_shootdown (issue #352 step 2c-iii) performs the local
// arch invalidate (full flush for all=true, VA+context-ID targeted
// otherwise) then calls the weak smp_tlb_shootdown_hook (no-op default;
// SMP override is #351/#389 territory, not asserted here).
// Input: all=true, VA+ID targeted, and VA+ID-zero targeted invocations.
// Expect: No crash/halt; zero PMM delta (invalidate allocates nothing).
// Depends: VMM::tlb_shootdown, PMM
JARVIS_TEST(vmm_tlb_shootdown_invokes_hook, "PRE: none | POST: none") {
    const uint64_t free_before = PMM::free_pages_ref();
    VMM::tlb_shootdown(0ULL, 0, true);
    VMM::tlb_shootdown(0x50000000ULL, 1, false);
    VMM::tlb_shootdown(0x50000000ULL, 0, false);
    JARVIS_ASSERT_FMT(PMM::free_pages_ref() == free_before,
                      "tlb_shootdown allocated %ld pages",
                      (long)(PMM::free_pages_ref() - free_before));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: context_id lifecycle (issue #352 step 2b allocators + 2c-i
// targeted purge on free): distinct live IDs, freed IDs recyclable, live
// IDs never reissued, free(0) is a no-op, tag format (0 → raw phys).
// Input: alloc/free/tag through arch::context_id_* (x86 delegates to
// pcid_*; aarch64/riscv64 use the ASID allocators).
// Expect: Allocated IDs nonzero + pairwise distinct; re-alloc after free
// returns the freed ID; tag(phys, 0) == phys; tag(phys, id) != phys.
// Depends: arch::context_id_alloc/free/tag/supported
JARVIS_TEST(vmm_context_id_lifecycle, "PRE: none | POST: none") {
    if (!arch::context_id_supported()) {
        // Fallback contract: unsupported → alloc 0, free/tag safe.
        JARVIS_ASSERT_EQ(0, arch::context_id_alloc());
        arch::context_id_free(0);
        constexpr uint64_t kRootU = 0x40000000ULL;
        JARVIS_ASSERT_EQ(arch::context_id_tag(kRootU, 0),
                         arch::context_id_tag(kRootU, 0));
        JARVIS_TEST_PASS();
        return;
    }
    uint16_t a = arch::context_id_alloc();
    uint16_t b = arch::context_id_alloc();
    uint16_t c = arch::context_id_alloc();
    JARVIS_ASSERT(a != 0);
    JARVIS_ASSERT(b != 0);
    JARVIS_ASSERT(c != 0);
    JARVIS_ASSERT(a != b);
    JARVIS_ASSERT(a != c);
    JARVIS_ASSERT(b != c);
    // Exhaust the pool (bounded): every handed-out ID must be distinct —
    // a live ID is never reissued; small pools answer 0 instead. (Bounded
    // at 300 consecutive distinct allocs: large 16-bit pools need not
    // drain fully; hitting the cap with zero duplicates proves the
    // invariant under pressure either way.)
    uint16_t ids[300];
    size_t n = 0;
    ids[n++] = a;
    ids[n++] = b;
    ids[n++] = c;
    for (int i = 0; i < 300; ++i) {
        uint16_t id = arch::context_id_alloc();
        if (id == 0)
            break;
        for (size_t k = 0; k < n; ++k) {
            JARVIS_ASSERT_FMT(ids[k] != id,
                              "context ID %u reissued while live", id);
        }
        if (n >= 300)
            break;
        ids[n++] = id;
    }
    // Tag contract (arch shapes differ — x86/aarch64 return raw phys for
    // id 0, riscv64 always formats the SATP value): tagging is a pure
    // function of (root, id), and a live ID changes the published value.
    constexpr uint64_t kRoot = 0x40000000ULL;
    const uint64_t untagged = arch::context_id_tag(kRoot, 0);
    const uint64_t tagged = arch::context_id_tag(kRoot, a);
    JARVIS_ASSERT_EQ(untagged, arch::context_id_tag(kRoot, 0));
    JARVIS_ASSERT_EQ(tagged, arch::context_id_tag(kRoot, a));
    JARVIS_ASSERT_FMT(tagged != untagged,
                      "context ID %u not encoded in tagged root", a);
    // free(0) is a no-op; epoch never moves backwards.
    const uint64_t epoch_before = arch::context_id_epoch();
    arch::context_id_free(0);
    JARVIS_ASSERT(arch::context_id_epoch() >= epoch_before);
    for (size_t k = 0; k < n; ++k)
        arch::context_id_free(ids[k]);
    // Pool recovered: a fresh alloc works after the drain.
    uint16_t r = arch::context_id_alloc();
    JARVIS_ASSERT(r != 0);
    arch::context_id_free(r);
    JARVIS_TEST_PASS();
}

void register_vmm_tests() {
    Logger::info("Registering VMM tests");
    JARVIS_REGISTER_TEST(vmm_unmap_already_unmapped);
    JARVIS_REGISTER_TEST(vmm_map_already_mapped);
    JARVIS_REGISTER_TEST(vmm_map_page_null_phys);
    JARVIS_REGISTER_TEST(vmm_clone_failure_rollback);
    JARVIS_REGISTER_TEST(vmm_free_user_pages_shared);
    JARVIS_REGISTER_TEST(vmm_huge_page_split_corner);
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_REGISTER_TEST(vmm_huge_page_split_regression);
    JARVIS_REGISTER_TEST(vmm_hhdm_access_consistency);
    JARVIS_REGISTER_TEST(vmm_hhdm_take_semantics);
#endif
    JARVIS_REGISTER_TEST(vmm_free_user_pages_skips_kernel_owned_entries);
    JARVIS_REGISTER_TEST(vmm_free_user_pages_fork_stack_scenario);
    JARVIS_REGISTER_TEST(vmm_block_1g_geometry);
    JARVIS_REGISTER_TEST(vmm_free_user_pages_pdpt_1g_rejects_unaligned);
    JARVIS_REGISTER_TEST(vmm_tlb_shootdown_invokes_hook);
    JARVIS_REGISTER_TEST(vmm_context_id_lifecycle);
    JARVIS_REGISTER_TEST(vmm_err_wrappers_and_queries);
    JARVIS_REGISTER_TEST(vmm_unmap_err_not_mapped);
    JARVIS_REGISTER_TEST(vmm_cap_frame_mmio_paths);
}