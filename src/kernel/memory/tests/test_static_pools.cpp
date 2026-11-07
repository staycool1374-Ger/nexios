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
#include <kernel/memory/mempool.hpp>

using namespace kernel;

#if CONFIG_STATIC_POOLS_ONLY
JARVIS_TEST(static_pools_pmm_disabled_after_init, "PRE: none | POST: none") {
    PMM::mark_init_done();
    uint64_t page = PMM::alloc_page();
    JARVIS_ASSERT_EQ(0ULL, page);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(static_pools_contiguous_disabled_after_init,
            "PRE: none | POST: none") {
    PMM::mark_init_done();
    uint64_t pages = PMM::alloc_contiguous(4);
    JARVIS_ASSERT_EQ(0ULL, pages);
    JARVIS_TEST_PASS();
}
#endif

JARVIS_TEST(static_pools_mempool_reserve_success, "PRE: none | POST: none") {
    size_t before = MemPool::pool_free_count(0);
    errors::MemPoolError err = MemPool::reserve(0, 1);
    JARVIS_ASSERT(err == errors::MemPoolError::MEMPOOL_ERR_OK);
    size_t after = MemPool::pool_free_count(0);
    JARVIS_ASSERT(after == before - 1);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(static_pools_mempool_reserve_exhaustion,
            "PRE: none | POST: none") {
    size_t total = MemPool::pool_free_count(0);
    errors::MemPoolError err = MemPool::reserve(0, total + 1);
    JARVIS_ASSERT(err == errors::MemPoolError::MEMPOOL_ERR_OOM);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(static_pools_mempool_reserve_then_alloc,
            "PRE: none | POST: none") {
    errors::MemPoolError err = MemPool::reserve(1, 1);
    JARVIS_ASSERT(err == errors::MemPoolError::MEMPOOL_ERR_OK);
    void *p = MemPool::alloc(32);
    JARVIS_ASSERT(p != nullptr);
    if (p)
        MemPool::free(p);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(static_pools_mempool_reserve_all_then_alloc_fails,
            "PRE: none | POST: none") {
    size_t total = MemPool::pool_free_count(2);
    errors::MemPoolError err = MemPool::reserve(2, total);
    JARVIS_ASSERT(err == errors::MemPoolError::MEMPOOL_ERR_OK);
    void *p = MemPool::alloc(64);
    JARVIS_ASSERT(p == nullptr);
    JARVIS_TEST_PASS();
}

void register_static_pools_tests() {
    Logger::info("Registering static pools tests");
#if CONFIG_STATIC_POOLS_ONLY
    JARVIS_REGISTER_TEST(static_pools_pmm_disabled_after_init);
    JARVIS_REGISTER_TEST(static_pools_contiguous_disabled_after_init);
#endif
    JARVIS_REGISTER_TEST(static_pools_mempool_reserve_success);
    JARVIS_REGISTER_TEST(static_pools_mempool_reserve_exhaustion);
    JARVIS_REGISTER_TEST(static_pools_mempool_reserve_then_alloc);
    JARVIS_REGISTER_TEST(static_pools_mempool_reserve_all_then_alloc_fails);
}
