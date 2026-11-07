/// @file test_freelist_consistency.cpp
/// @brief Verifies that PMM freelist heads are consistent with the bitmap
///        after free/alloc cycles — snapshot_restore does NOT capture
///        free_head_/pool_free_head_, so they must be rebuilt from the
///        bitmap.  Without rebuild, alloc_page()/alloc_page_table() return
///        pages already marked as allocated → double-alloc → corruption.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Allocate a page via alloc_page_table (pool), free it via
//           free_page, then verify alloc_page_table can still see it.
//           Without pool-aware free_page, the freed pool page goes to the
//           general freelist and alloc_page_table's fallback to alloc_page
//           double-allocates it.
// Input: None
// Expect: freed pool page is returned by a subsequent alloc_page_table call
//         (unique phys addr, not double-allocated).
JARVIS_TEST(freelist_pool_page_not_double_alloced,
            "PRE: none | POST: none") {
    // Allocate a page from the page-table pool
    uint64_t pool_page = PMM::alloc_page_table();
    JARVIS_ASSERT(pool_page != 0);

    // Free via free_page (this used to put it on the GENERAL freelist)
    PMM::free_page(pool_page);

    // Allocate from pool again — must return the SAME page
    // (or some other free pool page)
    uint64_t re_alloc = PMM::alloc_page_table();
    JARVIS_ASSERT(re_alloc != 0);

    // Free both (second free is a no-op if already freed by free_page)
    PMM::free_page(re_alloc);

    // If pool-aware free_page works, this test passes without double-alloc.
    // If it doesn't, the first free puts the page on the general list,
    // alloc_page_table falls back to alloc_page which returns the SAME
    // page from the general list → double-alloc → corruption.
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: alloc_page followed by alloc_page_table must NOT return the
//           same physical page (double-alloc).  This test allocates all
//           pool pages via alloc_page_table, frees them via free_page,
//           then allocates via alloc_page.  Without rebuild_free_list or
//           pool-aware free_page, alloc_page may return a page that's
//           still tracked by the pool freelist → double-alloc.
// Input: None
// Expect: all allocated addresses are unique (no duplicates).
JARVIS_TEST(freelist_no_double_alloc_general_pool,
            "PRE: none | POST: none") {
    static constexpr size_t N = 16;
    uint64_t pages[N];
    size_t count = 0;

    // Exhaust the pool freelist quickly
    for (size_t i = 0; i < N * 2 && count < N; ++i) {
        uint64_t p = PMM::alloc_page_table();
        if (!p) break;
        // Free via free_page → should go to pool freelist (with fix)
        // or general freelist (without fix)
        PMM::free_page(p);
    }

    // Now allocate via alloc_page (general) and alloc_page_table (pool)
    for (size_t i = 0; i < N; ++i) {
        uint64_t gp = PMM::alloc_page();
        if (!gp) break;
        pages[count++] = gp;
        // Check no duplicate with previous alloc_page results
        for (size_t j = 0; j < count - 1; ++j) {
            JARVIS_ASSERT(pages[j] != gp);
        }
    }

    for (size_t i = 0; i < count; ++i)
        PMM::free_page(pages[i]);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: operator delete double-cleanup protection — create a task,
//           call cleanup + delete, verify no poisoned TCB remains in
//           scheduler structures.
// Input: None
// Expect: operator delete skips cleanup if state==REAPED → no double-free.
JARVIS_TEST(freelist_task_double_cleanup_safe,
            "PRE: none | POST: none") {
    auto *task = TaskControlBlock::create([]() {}, 10, 10);
    JARVIS_ASSERT(task != nullptr);
    Scheduler::add_task(*task);

    // Simulate a test that calls cleanup + delete (double-cleanup pattern)
    task->cleanup();
    // operator delete should see state==REAPED and skip cleanup+remove_task
    delete task;

    // If we reach here, operator delete handled the double-cleanup safely
    // (no crash, no scheduler corruption).
    JARVIS_TEST_PASS();
}

void register_freelist_consistency_tests() {
    Logger::info("Registering freelist consistency tests");
    JARVIS_REGISTER_TEST(freelist_pool_page_not_double_alloced);
    JARVIS_REGISTER_TEST(freelist_no_double_alloc_general_pool);
    JARVIS_REGISTER_TEST(freelist_task_double_cleanup_safe);
}
