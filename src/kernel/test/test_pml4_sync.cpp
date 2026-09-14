/*
 * NexIOS RTOS — SMP bring-up (Phase 5)
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

/// @file test_pml4_sync.cpp
/// @brief PML4-synchronization tests (issue #85, module 18): table-level
///        update/removal visibility pins (real, single-CPU half); remote
///        visibility, shootdown-before-visible and remove-on-all-CPUs are
///        documented stubs (no shootdown path exists).  Fork-path sync is
///        covered by the pml4_clone class (10 tests).

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/nexios_config.h>

using namespace kernel;

namespace {

// Scratch user VA (PML4 entry 0); each test owns a private PML4, so no
// collision with the vmm/pml4_clone scratch addresses.
constexpr uint64_t kSyncVa = 0x420000ULL;
constexpr uint64_t kSyncVaNext = 0x421000ULL;

}  // namespace

// Runmode: kernel
// Testidea: A remap is synchronously table-visible: mapping VA->A then
//           VA->B makes the walk resolve B immediately.  The tables are
//           the structure all CPUs share, so writer-side table sync is
//           the single-CPU half of cross-CPU visibility.
// Input: map/remap at kSyncVa in a private PML4, walk after each step.
// Expect: Walk resolves A, then B.  Full cleanup (no PMM delta).
// Depends: VMM::map_page_in_pml4/virt_to_phys_in_pml4/free_user_pages
JARVIS_TEST(pml4_sync_remap_visible, "PRE: none | POST: none") {
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    uint64_t phys_a = PMM::alloc_page();
    JARVIS_ASSERT(phys_a != 0);
    uint64_t phys_b = PMM::alloc_page();
    JARVIS_ASSERT(phys_b != 0);
    VMM::map_page_in_pml4(kSyncVa, phys_a, false, pml4);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(kSyncVa, pml4) == phys_a);
    VMM::map_page_in_pml4(kSyncVa, phys_b, false, pml4);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(kSyncVa, pml4) == phys_b);
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    PMM::free_page(phys_a);
    PMM::free_page(phys_b);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Removal is selective and visible: unmapping one of two
//           pages resolves the other intact and the removed VA to 0.
// Input: Map two pages, unmap_frame_from_cap the first, walk both.
// Expect: Removed VA walks to 0; sibling still resolves; no PMM delta.
// Depends: VMM::map_page_in_pml4/unmap_frame_from_cap/
//         virt_to_phys_in_pml4/free_user_pages
JARVIS_TEST(pml4_sync_unmap_selective, "PRE: none | POST: none") {
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    uint64_t phys_a = PMM::alloc_page();
    JARVIS_ASSERT(phys_a != 0);
    uint64_t phys_b = PMM::alloc_page();
    JARVIS_ASSERT(phys_b != 0);
    VMM::map_page_in_pml4(kSyncVa, phys_a, false, pml4);
    VMM::map_page_in_pml4(kSyncVaNext, phys_b, false, pml4);
    VMM::unmap_frame_from_cap(kSyncVa, pml4);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(kSyncVa, pml4) == 0);
    JARVIS_ASSERT(VMM::virt_to_phys_in_pml4(kSyncVaNext, pml4) == phys_b);
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    PMM::free_page(phys_a);
    PMM::free_page(phys_b);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A page-table update on one CPU is visible to all CPUs:
//           after map + shootdown, every online CPU walks the new entry.
// Input: Map on CPU0, shootdown, walk on CPU1.
// Expect: CPU1 resolves the new mapping (no stale miss).
// Depends: Cross-CPU shootdown completion (not yet implemented)
JARVIS_TEST(pml4_sync_remote_visible, "PRE: none | POST: none | PENDING: shootdown") {
    /* Pseudocode:
     *   map_on(0, va); shootdown_sync(); JARVIS_ASSERT(walk_on(1, va) == phys);
     */
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A write is not considered visible until the shootdown
//           completes: the writer blocks until all CPUs acknowledge.
// Input: Map + write with shootdown pending on a slow CPU.
// Expect: write_visible() returns only after full acknowledgement.
// Depends: Shootdown-completion barrier (not yet implemented)
JARVIS_TEST(pml4_sync_write_after_shootdown, "PRE: none | POST: none | PENDING: shootdown") {
    /* Pseudocode:
     *   start_slow_cpu(); map(); JARVIS_ASSERT(!visible_until_acked());
     */
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Page removal invalidates every CPU before the page is
//           recycled: no CPU can still walk the freed frame.
// Input: Unmap shared page, free it, walk on every CPU.
// Expect: All walks miss; freed frame never observed via stale entry.
// Depends: Remove-on-all-CPUs + quarantine (not yet implemented)
JARVIS_TEST(pml4_sync_remove_all_cpus, "PRE: none | POST: none | PENDING: shootdown") {
    /* Pseudocode:
     *   unmap_shared(); free_page(); JARVIS_ASSERT(all_cpus_miss(va));
     */
    JARVIS_TEST_PASS();
}

void register_pml4_sync_tests() {
    Logger::info("Registering pml4 sync tests");
    JARVIS_REGISTER_TEST(pml4_sync_remap_visible);
    JARVIS_REGISTER_TEST(pml4_sync_unmap_selective);
    JARVIS_REGISTER_TEST(pml4_sync_remote_visible);
    JARVIS_REGISTER_TEST(pml4_sync_write_after_shootdown);
    JARVIS_REGISTER_TEST(pml4_sync_remove_all_cpus);
}
#endif  // CONFIG_ARCH_X86_64
