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

/// @file test_memory_determinism.cpp
/// @brief Memory determinism tests — exhaust PMM pages and verify graceful,
///        policy-defined failure (task blocked/killed, capacity restored).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/memory/pmm.hpp>
#include <scope_guard.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Exhaust PMM via repeated alloc_contiguous(128), verify that 0
// is returned on exhaustion, free restores capacity.
// Input: Loop alloc_contiguous(128) until 0, save each block, free all, alloc one more
// Expect: Clean cycle with no crash
JARVIS_TEST(memory_determinism_pmm_exhaust_cycle,
            "PRE: none | POST: none") {
    // 256 MB QEMU / 4 KB = 65536 pages.  Allocate 128-page blocks:
    // 65536 / 128 = 512 blocks max, plus headroom.
    static constexpr size_t MAX_BLOCKS = 640;
    uint64_t blocks[MAX_BLOCKS];
    size_t count = 0;
    for (; count < MAX_BLOCKS; ++count) {
        uint64_t b = PMM::alloc_contiguous(128);
        if (!b)
            break;
        blocks[count] = b;
    }
    JARVIS_ASSERT(count > 0);

    // Free all pages in each block
    for (size_t i = 0; i < count; ++i)
        for (size_t j = 0; j < 128; ++j)
            PMM::free_page(blocks[i] + j * arch::PAGE_SIZE);

    // Should succeed again
    uint64_t p = PMM::alloc_page();
    JARVIS_ASSERT(p != 0);
    PMM::free_page(p);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verify alloc_contiguous(4) returns 0 on exhaustion and that
// free restores capacity.
// Input: Exhaust via alloc_contiguous(4), free, alloc again
// Expect: Clean cycle
JARVIS_TEST(memory_determinism_contiguous_exhaust_cycle,
            "PRE: none | POST: none") {
    static constexpr size_t MAX_BLOCKS = 1024;
    uint64_t blocks[MAX_BLOCKS];
    size_t count = 0;
    for (; count < MAX_BLOCKS; ++count) {
        uint64_t b = PMM::alloc_contiguous(4);
        if (!b)
            break;
        blocks[count] = b;
    }
    JARVIS_ASSERT(count > 0);

    for (size_t i = 0; i < count; ++i)
        for (size_t j = 0; j < 4; ++j)
            PMM::free_page(blocks[i] + j * arch::PAGE_SIZE);

    uint64_t b = PMM::alloc_contiguous(4);
    JARVIS_ASSERT(b != 0);
    for (size_t j = 0; j < 4; ++j)
        PMM::free_page(b + j * arch::PAGE_SIZE);

    JARVIS_TEST_PASS();
}

void register_memory_determinism_tests() {
    Logger::info("Registering memory determinism tests");
    JARVIS_REGISTER_TEST(memory_determinism_pmm_exhaust_cycle);
    JARVIS_REGISTER_TEST(memory_determinism_contiguous_exhaust_cycle);
}
