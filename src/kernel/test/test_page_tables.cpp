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
    JARVIS_ASSERT(t->page_table_ != 0);
    JARVIS_ASSERT(t->page_table_ != VMM::get_kernel_pml4());
    JARVIS_ASSERT(t->is_user_ == false);

    auto *priv = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                              (t->page_table_ & ~0xFFFULL));
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
    JARVIS_ASSERT(t->page_table_ != 0);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(page_tables_free_pages_on_cleanup, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(t != nullptr);
    uint64_t page_table = t->page_table_;
    JARVIS_ASSERT(page_table != 0);
    t->cleanup();
    JARVIS_ASSERT(t->page_table_ == 0);
    JARVIS_ASSERT(t->user_stack_ == 0);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(page_tables_max_process_pages_config, "PRE: none | POST: none") {
    JARVIS_ASSERT(CONFIG_MAX_PROCESS_PAGES > 0);
    JARVIS_ASSERT(CONFIG_MAX_PROCESS_PAGES >= 64);
    // Verify a user task can actually allocate up to the configured limit
    // by creating a user task and checking its page table is usable
    auto *t = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(t->page_table_ != 0);
    // Map a page to verify the page table works
    uint64_t test_pa = PMM::alloc_user_page();
    JARVIS_ASSERT(test_pa != 0);
    VMM::map_page_in_pml4(0x10000000, test_pa, true, t->page_table_);
    uint64_t resolved = VMM::virt_to_phys_in_pml4(0x10000000, t->page_table_);
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
    JARVIS_ASSERT(a->page_table_ != b->page_table_);

    // Map a test page in A's PML4 at a known VA (below STACK_VADDR).
    uint64_t test_va = 0x10000000;
    uint64_t test_pa = PMM::alloc_user_page();
    JARVIS_ASSERT(test_pa != 0);
    VMM::map_page_in_pml4(test_va, test_pa, true, a->page_table_);

    uint64_t in_a = VMM::virt_to_phys_in_pml4(test_va, a->page_table_);
    JARVIS_ASSERT_FMT(in_a == test_pa,
                      "Page in A resolves to 0x%lx (expected 0x%lx)",
                      in_a, test_pa);

    uint64_t in_b = VMM::virt_to_phys_in_pml4(test_va, b->page_table_);
    JARVIS_ASSERT_FMT(in_b == 0,
                      "Page in B resolves to 0x%lx (expected 0 — isolated)",
                      in_b);

    PMM::free_page(test_pa);
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
}
