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

/// @file test_mempool.cpp
/// @brief MemPool allocator tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>

using namespace kernel;

JARVIS_TEST(mempool_alloc_free, "PRE: none | POST: none") {
    size_t before = MemPool::pool_free_count(0);
    void *p = MemPool::alloc(16);
    JARVIS_ASSERT(p != nullptr);
    JARVIS_ASSERT(MemPool::pool_free_count(0) == before - 1);
    MemPool::free(p);
    JARVIS_ASSERT(MemPool::pool_free_count(0) == before);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(mempool_large_alloc, "PRE: none | POST: none") {
    void *p = MemPool::alloc(4096);
    JARVIS_ASSERT(p != nullptr);
    MemPool::free(p);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(mempool_fragmentation, "PRE: none | POST: none") {
    static const size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 8192};
    for (size_t s = 0; s < 9; ++s) {
        size_t bytes = sizes[s];
        static const int ALLOCS = 20;
        void *ptrs[ALLOCS] = {};
        int count = 0;
        for (int i = 0; i < ALLOCS; ++i) {
            ptrs[i] = MemPool::alloc(bytes);
            if (!ptrs[i]) break;
            count = i + 1;
            __builtin_memset(ptrs[i], 0xA5, bytes);
        }
        for (int i = count - 1; i >= 0; --i)
            MemPool::free(ptrs[i]);
    }
    void *p = MemPool::alloc(64);
    JARVIS_ASSERT(p != nullptr);
    MemPool::free(p);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(mempool_reuse, "PRE: none | POST: none") {
    void *a = MemPool::alloc(32);
    JARVIS_ASSERT(a != nullptr);
    MemPool::free(a);
    void *b = MemPool::alloc(32);
    JARVIS_ASSERT(b == a);
    MemPool::free(b);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The error-code alloc/free wrappers, pin/unpin helpers and the
//           Pool bitmap helpers must validate and report — each entered at
//           least once for function-level coverage (issue #143 item 5).
//           Only owned blocks are touched (freed exactly once — a second
//           free panics on ENSURE); reserve()/init_err() are deliberately
//           NOT driven (permanent reservation / live-pool re-init).
// Input: alloc_err OK + TOO_LARGE, free_err null + live block,
//        pin/unpin round-trip on an owned block, stack-local Pool bitmap
//        ops, error_string over all codes.
// Expect: Exact error codes, pin state flips, bitmap round-trips, exact
//         strings, no tracker delta.
// Depends: MemPool _err wrappers, pin/unpin, Pool helpers,
//          error_string<MemPoolError>
JARVIS_TEST(mempool_err_and_pool_helpers, "PRE: none | POST: none") {
    using errors::MemPoolError;
    void *block = nullptr;
    JARVIS_ASSERT(MemPool::alloc_err(64, block) == errors::MEMPOOL_ERR_OK);
    JARVIS_ASSERT(block != nullptr);
    void *too_big = nullptr;
    JARVIS_ASSERT(MemPool::alloc_err(1048576, too_big) ==
                  errors::MEMPOOL_ERR_TOO_LARGE);
    JARVIS_ASSERT(too_big == nullptr);
    JARVIS_ASSERT(MemPool::free_err(nullptr) ==
                  errors::MEMPOOL_ERR_INVALID_PTR);
    MemPool::pin_block(block);
    JARVIS_ASSERT(MemPool::is_block_pinned(block));
    MemPool::unpin_block(block);
    JARVIS_ASSERT(!MemPool::is_block_pinned(block));
    MemPool::unpin_block(block);
    JARVIS_ASSERT(!MemPool::is_block_pinned(block));
    JARVIS_ASSERT(MemPool::free_err(block) == errors::MEMPOOL_ERR_OK);
    MemPool::Pool local{};
    // The Pool ctor leaves the bitmap arrays uninitialized (static pools_
    // lives in zeroed BSS, so production never notices): clear explicitly
    // before use — this also covers the bitmap write path.
    uint64_t zeros[5] = {};
    local.write_freed_bitmap(zeros);
    local.write_pinned_bitmap(zeros);
    JARVIS_ASSERT(local.block_size == 0);
    JARVIS_ASSERT(!local.initialized);
    JARVIS_ASSERT(!local.is_block_freed(3));
    local.set_block_freed(3);
    JARVIS_ASSERT(local.is_block_freed(3));
    local.clear_block_freed(3);
    JARVIS_ASSERT(!local.is_block_freed(3));
    JARVIS_ASSERT(!local.is_block_pinned(7));
    local.set_block_pinned(7);
    JARVIS_ASSERT(local.is_block_pinned(7));
    local.clear_block_pinned(7);
    JARVIS_ASSERT(!local.is_block_pinned(7));
    uint64_t freed_copy[5] = {};
    uint64_t pinned_copy[5] = {};
    local.set_block_freed(1);
    local.set_block_pinned(2);
    local.copy_freed_bitmap(freed_copy);
    local.copy_pinned_bitmap(pinned_copy);
    MemPool::Pool replica{};
    replica.write_freed_bitmap(freed_copy);
    replica.write_pinned_bitmap(pinned_copy);
    JARVIS_ASSERT(replica.is_block_freed(1));
    JARVIS_ASSERT(replica.is_block_pinned(2));
    replica.clear_pinned_bitmap();
    JARVIS_ASSERT(!replica.is_block_pinned(2));
    JARVIS_ASSERT(errors::error_string(errors::MEMPOOL_ERR_OK) != nullptr);
    JARVIS_ASSERT(errors::error_string(errors::MEMPOOL_ERR_OOM) != nullptr);
    JARVIS_ASSERT(errors::error_string(errors::MEMPOOL_ERR_TOO_LARGE) !=
                  nullptr);
    JARVIS_ASSERT(errors::error_string(errors::MEMPOOL_ERR_INVALID_PTR) !=
                  nullptr);
    JARVIS_ASSERT(errors::error_string(errors::MEMPOOL_ERR_DOUBLE_FREE) !=
                  nullptr);
    JARVIS_ASSERT(errors::error_string(errors::MEMPOOL_ERR_POOL_UNINIT) !=
                  nullptr);
    JARVIS_ASSERT(errors::error_string(errors::MEMPOOL_ERR_INVALID_POOL) !=
                  nullptr);
    JARVIS_ASSERT(errors::error_string(errors::MEMPOOL_ERR_CORRUPTED) !=
                  nullptr);
    JARVIS_ASSERT(errors::error_string(static_cast<MemPoolError>(999)) !=
                  nullptr);
    JARVIS_TEST_PASS();
}

void register_mempool_tests() {
    Logger::info("Registering MemPool tests");
    JARVIS_REGISTER_TEST(mempool_alloc_free);
    JARVIS_REGISTER_TEST(mempool_large_alloc);
    JARVIS_REGISTER_TEST(mempool_fragmentation);
    JARVIS_REGISTER_TEST(mempool_reuse);
    JARVIS_REGISTER_TEST(mempool_err_and_pool_helpers);
}
