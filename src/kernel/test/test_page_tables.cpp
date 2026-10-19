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

#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/page_table.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <scope_guard.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>

using namespace kernel;

JARVIS_TEST(page_tables_alloc_from_pool, "PRE: none | POST: none") {
    uint64_t pt_page = PMM::alloc_page_table();
    JARVIS_ASSERT(pt_page != 0);
    JARVIS_ASSERT(pt_page % arch::PAGE_SIZE == 0);
    PMM::free_page(pt_page);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(page_tables_pool_multiple_allocs, "PRE: none | POST: none") {
    uint64_t pt1 = PMM::alloc_page_table();
    JARVIS_ASSERT(pt1 != 0);
    uint64_t pt2 = PMM::alloc_page_table();
    JARVIS_ASSERT(pt2 != 0);
    JARVIS_ASSERT(pt1 != pt2);
    PMM::free_page(pt1);
    PMM::free_page(pt2);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(page_tables_pool_size_configured, "PRE: none | POST: none") {
    JARVIS_ASSERT(CONFIG_PAGE_TABLE_POOL_SIZE > 0);
    JARVIS_ASSERT(CONFIG_PAGE_TABLE_POOL_SIZE >= 256);
    // Verify the pool actually has the configured number of pages available
    uint64_t count = 0;
    uint64_t pages[256];
    for (size_t i = 0; i < 256; ++i) {
        pages[i] = PMM::alloc_page_table();
        if (pages[i] == 0)
            break;
        count++;
    }
    JARVIS_ASSERT_FMT(count >= 256, "Page-table pool has %lu pages, expected >= 256", count);
    for (size_t i = 0; i < count; ++i)
        PMM::free_page(pages[i]);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: v0.4.0 MP-1 — every kernel task owns a private kernel-half PML4:
// non-zero, distinct from the boot kernel PML4, kernel half present, user
// half empty, and reclaimed by cleanup().
// Input: Create a kernel task; inspect page_table_; cleanup.
// Expect: page_table_ != 0 && != kernel PML4; user entries 0..255 zero;
// kernel entries match the kernel PML4; cleanup frees the PML4 page.
// Depends: kernel::memory::VMM, PMM
JARVIS_TEST(page_tables_kernel_task_private_pml4, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    auto cleanup = ScopeGuard([&]() {
        t->cleanup();
        delete t;
    });
    JARVIS_ASSERT(t->get_page_table() != 0);
    JARVIS_ASSERT(t->get_page_table() != VMM::get_kernel_pml4());
    JARVIS_ASSERT(t->is_user_ == false);

    auto *priv = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (t->get_page_table() & ~0xFFFULL));
    auto *kern = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (VMM::get_kernel_pml4() & ~0xFFFULL));
    for (size_t i = 0; i < arch::PML4_USER_COUNT; ++i) {
        JARVIS_ASSERT(priv[i] == 0);
    }
    for (size_t i = arch::PML4_KERNEL_START; i < arch::PML4_ENTRIES; ++i) {
        JARVIS_ASSERT(priv[i] == kern[i]);
    }
    JARVIS_TEST_PASS();
}

JARVIS_TEST(page_tables_user_task_page_table_set, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(t != nullptr);
    auto cleanup = ScopeGuard([&]() {
        t->cleanup();
        delete t;
    });
    JARVIS_ASSERT(t->get_page_table() != 0);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(page_tables_free_pages_on_cleanup, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(t != nullptr);
    uint64_t page_table = t->get_page_table();
    JARVIS_ASSERT(page_table != 0);
    t->cleanup();
    JARVIS_ASSERT(t->get_page_table() == 0);
    JARVIS_ASSERT(t->user_stack_ == 0);
    delete t; // free the TCB block (cleanup only tears down resources)
    JARVIS_TEST_PASS();
}

JARVIS_TEST(page_tables_max_process_pages_config, "PRE: none | POST: none") {
    JARVIS_ASSERT(CONFIG_MAX_PROCESS_PAGES > 0);
    JARVIS_ASSERT(CONFIG_MAX_PROCESS_PAGES >= 64);
    // Verify a user task can actually allocate up to the configured limit
    // by creating a user task and checking its page table is usable
    auto *t = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(t->get_page_table() != 0);
    // Map a page to verify the page table works
    uint64_t test_pa = PMM::alloc_user_page();
    JARVIS_ASSERT(test_pa != 0);
    VMM::map_page_in_pml4(0x10000000, test_pa, true, false, t->get_page_table());
    uint64_t resolved = VMM::virt_to_phys_in_pml4(0x10000000, t->get_page_table());
    JARVIS_ASSERT_EQ(test_pa, resolved);
    PMM::free_page(test_pa);
    t->cleanup();
    delete t;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Exhaust the page-table pool and verify alloc returns 0.
// Input: Call PMM::alloc_page_table() until exhaustion.
// Expect: Returns 0 when pool is exhausted (no panic/crash).
// Depends: PMM page-table pool
JARVIS_TEST(page_tables_pool_exhaustion, "PRE: none | POST: none") {
    uint64_t pages[256];
    uint64_t n = 0;
    for (; n < 256; ++n) {
        pages[n] = PMM::alloc_page_table();
        if (pages[n] == 0)
            break;
    }
    JARVIS_ASSERT_FMT(n > 0, "Page-table pool empty at test start");
    // Exhaustion is expected eventually — no assertion on exact count.
    // Free all allocated pages.
    for (uint64_t i = 0; i < n; ++i)
        PMM::free_page(pages[i]);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Two user tasks have independent page tables — mapping a
// page in one task's PML4 does not appear in the other's.
// Input: Create two user tasks, map a page in task A's PML4,
//        verify it does NOT resolve in task B's PML4.
// Expect: target_phys != 0 in A, 0 in B.
// Depends: VMM::map_page_in_pml4, VMM::virt_to_phys_in_pml4
JARVIS_TEST(page_tables_isolation, "PRE: none | POST: none") {
    auto *a = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    auto *b = TaskControlBlock::create_user([]() {}, 6, 10, 32_KiB);
    JARVIS_ASSERT(a != nullptr && b != nullptr);
    auto cleanup = ScopeGuard([&]() {
        a->cleanup();
        delete a;
        b->cleanup();
        delete b;
    });
    JARVIS_ASSERT(a->get_page_table() != b->get_page_table());

    // Map a test page in A's PML4 at a known VA (below STACK_VADDR).
    uint64_t test_va = 0x10000000;
    uint64_t test_pa = PMM::alloc_user_page();
    JARVIS_ASSERT(test_pa != 0);
    VMM::map_page_in_pml4(test_va, test_pa, true, false, a->get_page_table());

    uint64_t in_a = VMM::virt_to_phys_in_pml4(test_va, a->get_page_table());
    JARVIS_ASSERT_FMT(in_a == test_pa,
                      "Page in A resolves to 0x%lx (expected 0x%lx)",
                      in_a, test_pa);

    uint64_t in_b = VMM::virt_to_phys_in_pml4(test_va, b->get_page_table());
    JARVIS_ASSERT_FMT(in_b == 0,
                      "Page in B resolves to 0x%lx (expected 0 — isolated)",
                      in_b);

    PMM::free_page(test_pa);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: map_page_err validates alignment and maps through try
// Input: misaligned VA/PA; HHDM scratch VA + kernel page
// Expect: INVALID_ADDR on misalignment; OK + leaf resolve + clean unmap
//         (x86_64; other arches validation-only — kernel-table split
//         restore is per-arch, and only x86_64 carries the manual
//         PD save/restore pattern)
// Depends: kernel::memory::VMM::map_page_try
JARVIS_TEST(map_page_err_validation_and_success, "PRE: none | POST: none") {
    constexpr uint64_t SCRATCH_VA = arch::HHDM_OFFSET + 0x2000000ULL;
    uint64_t phys = PMM::alloc_page();
    JARVIS_ASSERT(phys != 0);

    JARVIS_ASSERT(VMM::map_page_err(SCRATCH_VA + 1, phys, false) ==
                  errors::VMM_ERR_INVALID_ADDR);
    JARVIS_ASSERT(VMM::map_page_err(SCRATCH_VA, phys + 1, false) ==
                  errors::VMM_ERR_INVALID_ADDR);

#if defined(CONFIG_ARCH_X86_64)
    // Manual PD save/restore (cross_arch doctrine): the snapshot rewind
    // does not free split table pages, so the test restores the boot
    // huge entry itself and frees the split PT (zero PMM delta).
    // Table-entry → phys must mask the XD bit too (boot sets XD on
    // higher-half tables; & ~0xFFF keeps bit 63 and walks into garbage —
    // same mask as cross_frame_mask in test_cross_arch.cpp).
    constexpr uint64_t kX86TableFrameMask = 0x000FFFFFFFFFF000ULL;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (VMM::get_kernel_pml4() &
                                               kX86TableFrameMask));
    const size_t pml4_idx = arch::ArchPageTable::pml4_index(SCRATCH_VA);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pdpt = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (pml4[pml4_idx] & kX86TableFrameMask));
    const size_t pdpt_idx = arch::ArchPageTable::pdpt_index(SCRATCH_VA);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (pdpt[pdpt_idx] &
                                             kX86TableFrameMask));
    const size_t pd_idx = arch::ArchPageTable::pd_index(SCRATCH_VA);
    constexpr uint64_t kX86HugeBit = 1ULL << 7;
    const uint64_t boot_entry = pd[pd_idx];
    JARVIS_ASSERT((boot_entry & kX86HugeBit) != 0);
    {
        // Issue #200: HHDM PD surgery is IRQ-critical.
        arch::IrqGuard irq_guard;
        auto &probe_rt = kernel::test::ResourceTracker::instance();
        kernel::test::ResourceCounters probe0{};
        probe_rt.capture(probe0);
        JARVIS_ASSERT(VMM::map_page_err(SCRATCH_VA, phys, false) ==
                      errors::VMM_ERR_OK);
        kernel::test::ResourceCounters probe1{};
        probe_rt.capture(probe1);
        JARVIS_ASSERT_FMT(probe1.pmm_pages_used == probe0.pmm_pages_used + 1,
                          "map must allocate exactly 1 table page (%lu -> %lu)",
                          probe0.pmm_pages_used, probe1.pmm_pages_used);
        JARVIS_ASSERT_EQ(phys, VMM::virt_to_phys(SCRATCH_VA));
        VMM::unmap_page(SCRATCH_VA);
        const uint64_t split_pt = pd[pd_idx] & kX86TableFrameMask;
        JARVIS_ASSERT_FMT(PMM::is_allocated(split_pt),
                          "split PT 0x%lx must be a live page", split_pt);
        pd[pd_idx] = boot_entry;
        arch::ArchPageTable::tlb_flush(SCRATCH_VA);
        PMM::free_page(split_pt);
        JARVIS_ASSERT_FMT(!PMM::is_allocated(split_pt),
                          "split PT 0x%lx must be free after free_page",
                          split_pt);
        kernel::test::ResourceCounters probe2{};
        probe_rt.capture(probe2);
        JARVIS_ASSERT_FMT(probe2.pmm_pages_used == probe0.pmm_pages_used,
                          "restore must return to pre-map count (%lu -> %lu)",
                          probe0.pmm_pages_used, probe2.pmm_pages_used);
    }
    JARVIS_ASSERT_EQ(boot_entry, pd[pd_idx]);
    // Restoring the boot huge entry re-maps the VA through the huge page
    // (boot state) — it must resolve to the huge frame, not zero.
    constexpr uint64_t kX86HugeFrameMask = 0x000FFFFFFFE00000ULL;
    const uint64_t huge_base = boot_entry & kX86HugeFrameMask;
    JARVIS_ASSERT_EQ(huge_base + (SCRATCH_VA & 0x1FFFFFULL),
                     VMM::virt_to_phys(SCRATCH_VA));
#endif
    PMM::free_page(phys);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: map_page_err surfaces walk OOM as PAGE_ALLOC, writing nothing
// Input: both PMM pools drained exactly (same technique as
//        mmio_map_err_rollback_oom); kernel-table map at an UNMAPPED
//        HHDM VA past the boot window (fresh tables needed: first
//        alloc already fails, so nothing is installed)
// Expect: PAGE_ALLOC; VA still unmapped; pools fully restored
// Depends: kernel::memory::VMM::map_page_try, kernel::memory::PMM pools
JARVIS_TEST(map_page_err_oom_writes_nothing, "PRE: none | POST: none") {
    // Deterministic OOM: null the OOM-killer for the drain+probe window
    // (test_resource_exhaustion doctrine) — otherwise the handler kills
    // user tasks mid-drain and the reaper frees pages under the probe.
    PMM::OOMHandler saved_oom = PMM::get_oom_handler();
    PMM::set_oom_handler(nullptr);
    auto oom_restore = ScopeGuard([&]() { PMM::set_oom_handler(saved_oom); });
    uint64_t held_phys = PMM::alloc_page();
    JARVIS_ASSERT(held_phys != 0);
    // Past the 128 MiB boot HHDM window: no tables exist here on any
    // arch, so the walk fails on the very first table allocation with
    // zero side effects (entry assert guards the premise per arch).
    constexpr uint64_t OOM_VA = arch::HHDM_OFFSET + 0x10000000ULL;
    JARVIS_ASSERT_EQ(0ULL, VMM::virt_to_phys(OOM_VA));

    // Unified refusal-loop drain: alloc_page_table serves the table pool
    // first, then falls back to alloc_page (free list + bitmap scan over
    // the whole window), so ONE loop dries every pool bitmap-wide.
    // List-driven (never bitmap-exact): immune to snapshot-rewind
    // staleness and SMP racer refills.  Settle rounds repeat until a
    // full pass takes nothing; links live in the pages (zero BSS).
    // Rebuild free lists from the bitmap FIRST: without a re-sync the
    // pool list can hold snapshot-rewind staleness that spins an
    // unbounded drain (filed as PMM follow-up; the rebuild bounds it).
    PMM::rebuild_free_list();
    uint64_t chain_head = 0;
    // Issue #200 + determinism: IRQs off shrinks the racer window to the
    // AP tick (harness timeout is host-side).
    arch::IrqGuard drain_guard;
    for (int settle = 0; settle < 3; ++settle) {
        size_t took = 0;
        uint64_t take_page = 0;
        // Hang guard: the window holds < 32768 pages; more takes means a
        // cycling free list (fail loud, never stall the harness).
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
    // Zero-consumption dryness probes: both allocators must refuse now.
    JARVIS_ASSERT_FMT(PMM::alloc_page() == 0,
                      "general pool not dry after drain");
    JARVIS_ASSERT_FMT(PMM::alloc_page_table() == 0,
                      "table pool not dry after drain");

    JARVIS_ASSERT(VMM::map_page_err(OOM_VA, held_phys, false) ==
                  errors::VMM_ERR_PAGE_ALLOC);
    JARVIS_ASSERT_EQ(0ULL, VMM::virt_to_phys(OOM_VA));

    while (chain_head != 0) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *link = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                  chain_head);
        const uint64_t next_head = *link;
        PMM::free_page(chain_head);
        chain_head = next_head;
    }
    PMM::free_page(held_phys);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: runtime kernel-half maps are non-executable (issue #376
//           slice 1 — x86 encode_kernel_leaf carries NX; riscv never
//           sets X; aarch64 sets UXN+PXN in this path).
// Input: kernel-table map at a scratch HHDM VA (fresh split, restored
//        after like map_page_err_validation_and_success).
// Expect: pte_is_executable_in_pml4() false while mapped; VA unmapped
//         after; PMM delta zero.
// Depends: VMM::map_page_err, VMM::pte_is_executable_in_pml4
JARVIS_TEST(kernel_half_runtime_map_nx, "PRE: none | POST: none") {
    // Same scratch VA as map_page_err_validation_and_success (proven 2 MiB
    // boot huge entry; tests run sequentially with snapshot restore between).
    constexpr uint64_t SCRATCH_VA = arch::HHDM_OFFSET + 0x2000000ULL;
    uint64_t phys = PMM::alloc_page();
    JARVIS_ASSERT(phys != 0);
#if defined(CONFIG_ARCH_X86_64)
    // Same PD save/restore as map_page_err_validation_and_success: the
    // scratch VA sits in a boot 2 MiB huge entry; the split PT is freed
    // manually (snapshot rewind does not free split tables).
    constexpr uint64_t kX86TableFrameMask = 0x000FFFFFFFFFF000ULL;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pml4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (VMM::get_kernel_pml4() &
                                               kX86TableFrameMask));
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pdpt = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET +
        (pml4[arch::ArchPageTable::pml4_index(SCRATCH_VA)] &
         kX86TableFrameMask));
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pd = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET +
        (pdpt[arch::ArchPageTable::pdpt_index(SCRATCH_VA)] &
         kX86TableFrameMask));
    const size_t pd_idx = arch::ArchPageTable::pd_index(SCRATCH_VA);
    constexpr uint64_t kX86HugeBit = 1ULL << 7;
    const uint64_t boot_entry = pd[pd_idx];
    JARVIS_ASSERT((boot_entry & kX86HugeBit) != 0);
    {
        arch::IrqGuard irq_guard;
        JARVIS_ASSERT(VMM::map_page_err(SCRATCH_VA, phys, false) ==
                      errors::VMM_ERR_OK);
        JARVIS_ASSERT(!VMM::pte_is_executable_in_pml4(
            SCRATCH_VA, VMM::get_kernel_pml4()));
        VMM::unmap_page(SCRATCH_VA);
        const uint64_t split_pt = pd[pd_idx] & kX86TableFrameMask;
        pd[pd_idx] = boot_entry;
        arch::ArchPageTable::tlb_flush(SCRATCH_VA);
        PMM::free_page(split_pt);
    }
    JARVIS_ASSERT_EQ(boot_entry, pd[pd_idx]);
#else
    JARVIS_ASSERT(VMM::map_page_err(SCRATCH_VA, phys, false) ==
                  errors::VMM_ERR_OK);
    JARVIS_ASSERT(!VMM::pte_is_executable_in_pml4(SCRATCH_VA,
                                                  VMM::get_kernel_pml4()));
    VMM::unmap_page(SCRATCH_VA);
#endif
    PMM::free_page(phys);
    JARVIS_TEST_PASS();
}

extern "C" {
// Linker image bounds (4K-aligned starts; see linker/linker_*.ld).
// Mirrors the integrity.hpp _text_start idiom.
extern uint64_t _text_start[];
extern uint64_t _rodata_start[];
extern uint64_t _data_start[];
extern uint64_t _bss_end[];
}

// Runmode: kernel
// Testidea: boot-established kernel-half image permissions are W^X
//           (issue #376 epic remainder, VMM::harden_kernel_half): text RX,
//           rodata RO+NX, data/bss RW+NX. Sample VAs come straight from the
//           linker symbols; arch dispatch lives in the inspectors.
// Input: live kernel PML4 + section VAs.
// Expect: text RO + executable (also the inspector's positive control);
//         rodata RO + non-executable; data/bss non-RO + non-executable.
//         No mapping changes (walk-only), PMM delta zero.
// Depends: harden_kernel_half, pte_is_ro_in_pml4,
//          pte_is_executable_in_pml4.
// Note: the x86 kslot window (RW+NX via map_kstack_page) has no stable VA
// in the test harness (HHDM fallback when in_test_harness), so no slot
// assert here — every x86 task creation post-harden maps its stack NX,
// and a faulting stack would triple-fault the suite instantly.
JARVIS_TEST(kernel_half_boot_wx_pins, "PRE: none | POST: none") {
    const uint64_t pml4 = VMM::get_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const uint64_t text_va = reinterpret_cast<uint64_t>(_text_start);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const uint64_t rodata_va = reinterpret_cast<uint64_t>(_rodata_start);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const uint64_t data_va = reinterpret_cast<uint64_t>(_data_start);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const uint64_t bss_va = reinterpret_cast<uint64_t>(_bss_end) - 8;

    bool exec = false;
    JARVIS_ASSERT_FMT(VMM::pte_is_ro_in_pml4(text_va, pml4, &exec),
                      "text not RO");
    JARVIS_ASSERT_FMT(exec, "text not executable");
    JARVIS_ASSERT_FMT(VMM::pte_is_ro_in_pml4(rodata_va, pml4, &exec),
                      "rodata not RO");
    JARVIS_ASSERT_FMT(!exec, "rodata executable");
    JARVIS_ASSERT_FMT(!VMM::pte_is_ro_in_pml4(data_va, pml4, nullptr),
                      "data RO");
    JARVIS_ASSERT_FMT(!VMM::pte_is_executable_in_pml4(data_va, pml4),
                      "data executable");
    JARVIS_ASSERT_FMT(!VMM::pte_is_ro_in_pml4(bss_va, pml4, nullptr),
                      "bss RO");
    JARVIS_ASSERT_FMT(!VMM::pte_is_executable_in_pml4(bss_va, pml4),
                      "bss executable");
    JARVIS_TEST_PASS();
}

void register_page_tables_tests() {
    Logger::info("Registering page tables tests");
    JARVIS_REGISTER_TEST(page_tables_alloc_from_pool);
    JARVIS_REGISTER_TEST(page_tables_pool_multiple_allocs);
    JARVIS_REGISTER_TEST(page_tables_pool_size_configured);
    JARVIS_REGISTER_TEST(page_tables_kernel_task_private_pml4);
    JARVIS_REGISTER_TEST(page_tables_user_task_page_table_set);
    JARVIS_REGISTER_TEST(page_tables_free_pages_on_cleanup);
    JARVIS_REGISTER_TEST(page_tables_max_process_pages_config);
    JARVIS_REGISTER_TEST(page_tables_pool_exhaustion);
    JARVIS_REGISTER_TEST(page_tables_isolation);
    JARVIS_REGISTER_TEST(map_page_err_validation_and_success);
    JARVIS_REGISTER_TEST(map_page_err_oom_writes_nothing);
    JARVIS_REGISTER_TEST(kernel_half_runtime_map_nx);
    JARVIS_REGISTER_TEST(kernel_half_boot_wx_pins);
}
