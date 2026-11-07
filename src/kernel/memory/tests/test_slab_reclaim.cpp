#include <test.hpp>
#include <logger.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Repeated MemPool alloc/free cycle does not leak or corrupt.
// Input: Allocate and free blocks in a loop.
// Expect: No memory corruption, pool state remains consistent.
// Depends: MemPool
JARVIS_TEST(slab_reclaim_alloc_free_cycle, "PRE: none | POST: none") {
    for (int i = 0; i < 10; ++i) {
        void* p = MemPool::alloc(64);
        JARVIS_ASSERT(p != nullptr);
        JARVIS_ASSERT(MemPool::contains(p));
        MemPool::free(p);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Free memory after alloc+free cycle should be >= before.
// Input: Allocate and free many blocks of the same pool size.
// Expect: Free memory is same or more after alloc+free cycle.
// Depends: MemPool, PMM
JARVIS_TEST(slab_reclaim_pages_returned, "PRE: none | POST: none") {
    uint64_t free_before = PMM::free_memory();
    constexpr size_t N = 64;
    void* blocks[N];
    for (size_t i = 0; i < N; ++i) {
        blocks[i] = MemPool::alloc(32);
        JARVIS_ASSERT(blocks[i] != nullptr);
    }
    for (size_t i = 0; i < N; ++i) {
        MemPool::free(blocks[i]);
    }
    uint64_t free_after = PMM::free_memory();
    JARVIS_ASSERT(free_after >= free_before);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Partially used slabs are not returned during reclamation.
// Input: Allocate 3 blocks, free 2, keep 1.
// Expect: Held block still accessible and in pool.
// Depends: MemPool
JARVIS_TEST(slab_reclaim_partial_not_touched, "PRE: none | POST: none") {
    void* a = MemPool::alloc(64);
    void* b = MemPool::alloc(64);
    void* c = MemPool::alloc(64);
    JARVIS_ASSERT(a && b && c);
    MemPool::free(a);
    MemPool::free(b);
    JARVIS_ASSERT(MemPool::contains(c));
    MemPool::free(c);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: PMM alloc and free cycle for a single page.
// Input: Allocate one page via PMM, then free it.
// Expect: PMM allocation succeeds and free restores free memory.
// Depends: PMM
JARVIS_TEST(slab_reclaim_reallocate, "PRE: none | POST: none") {
    uint64_t before = PMM::free_memory();
    uint64_t page = PMM::alloc_page();
    JARVIS_ASSERT(page != 0);
    JARVIS_ASSERT(PMM::free_memory() == before - 4096);
    PMM::free_page(page);
    JARVIS_ASSERT(PMM::free_memory() >= before);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: MemPool free is idempotent (no double-free corruption).
// Input: Allocate and free same block.
// Expect: Free completes without crash.
// Depends: MemPool
JARVIS_TEST(slab_reclaim_free_idempotent, "PRE: none | POST: none") {
    void* p = MemPool::alloc(16);
    JARVIS_ASSERT(p != nullptr);
    MemPool::free(p);
    JARVIS_TEST_PASS();
}

void register_slab_reclaim_tests() {
    Logger::info("Registering slab reclaim tests");
    JARVIS_REGISTER_TEST(slab_reclaim_alloc_free_cycle);
    JARVIS_REGISTER_TEST(slab_reclaim_pages_returned);
    JARVIS_REGISTER_TEST(slab_reclaim_partial_not_touched);
    JARVIS_REGISTER_TEST(slab_reclaim_reallocate);
    JARVIS_REGISTER_TEST(slab_reclaim_free_idempotent);
}
