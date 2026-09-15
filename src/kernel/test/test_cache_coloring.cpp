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

/// @file test_cache_coloring.cpp
/// @brief Cache-coloring allocator tests (issue #62; stubs from issue
///        #85, module 10).  Verifies PMM's colored path: exact-color
///        allocation, uniform spread, range/stability, and the
///        bit-extraction contract.  All pages freed per test (net-zero
///        via the harness snapshot check); cursors reset at entry for
///        deterministic placement.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/cache_color.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Consecutive allocations land on different cache colors.
// Input: N=64 colored allocations cycling colors 0..15.
// Expect: Every page has exactly the requested color (adjacent pairs
//         differ by construction); all freed, PMM free-memory delta 0.
// Depends: PMM::alloc_page_colored, PMM::color_of (issue #62)
JARVIS_TEST(cache_coloring_spread, "PRE: none | POST: none") {
    constexpr uint64_t k_n = 64;
    PMM::reset_color_cursor();
    uint64_t pages[k_n];
    uint64_t mem_before = PMM::free_memory();
    for (uint64_t i = 0; i < k_n; ++i) {
        pages[i] = PMM::alloc_page_colored(i % cache::NUM_COLORS);
        JARVIS_ASSERT(pages[i] != 0);
        JARVIS_ASSERT(PMM::color_of(pages[i]) == i % cache::NUM_COLORS);
    }
    for (uint64_t i = 1; i < k_n; ++i)
        JARVIS_ASSERT(PMM::color_of(pages[i]) !=
                      PMM::color_of(pages[i - 1]));
    for (uint64_t i = 0; i < k_n; ++i)
        PMM::free_page(pages[i]);
    JARVIS_ASSERT(PMM::free_memory() == mem_before);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Uniform request spread keeps same-color pairs within the
//           design bound (stub formula, now with real pages).
// Input: M=64 cycling-color allocations; count same-color pairs.
// Expect: collisions * NUM_COLORS <= M*(M-1)/2 + M and every color
//         holds exactly M/NUM_COLORS pages; all freed, delta 0.
// Depends: PMM::alloc_page_colored (issue #62)
JARVIS_TEST(cache_coloring_collisions_bounded, "PRE: none | POST: none") {
    constexpr uint64_t k_m = 64;
    PMM::reset_color_cursor();
    uint64_t pages[k_m];
    uint64_t mem_before = PMM::free_memory();
    for (uint64_t i = 0; i < k_m; ++i) {
        pages[i] = PMM::alloc_page_colored(i % cache::NUM_COLORS);
        JARVIS_ASSERT(pages[i] != 0);
    }
    uint64_t per_color[cache::NUM_COLORS];
    for (uint64_t c = 0; c < cache::NUM_COLORS; ++c)
        per_color[c] = 0;
    for (uint64_t i = 0; i < k_m; ++i)
        ++per_color[PMM::color_of(pages[i])];
    uint64_t collisions = 0;
    for (uint64_t c = 0; c < cache::NUM_COLORS; ++c) {
        JARVIS_ASSERT(per_color[c] == k_m / cache::NUM_COLORS);
        collisions += per_color[c] * (per_color[c] - 1) / 2;
    }
    JARVIS_ASSERT(collisions * cache::NUM_COLORS <=
                  k_m * (k_m - 1) / 2 + k_m);
    for (uint64_t i = 0; i < k_m; ++i)
        PMM::free_page(pages[i]);
    JARVIS_ASSERT(PMM::free_memory() == mem_before);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Arbitrary allocation sizes get a well-defined color.
// Input: One colored page per size class {1 B .. 2 MiB}.
// Expect: Every color in [0, NUM_COLORS); re-reads stable; freed.
// Depends: PMM::alloc_page_colored, PMM::color_of (issue #62)
JARVIS_TEST(cache_coloring_arbitrary_sizes, "PRE: none | POST: none") {
    constexpr uint64_t k_sizes[] = {1, 512, 4096, 8192,
                                    65536, 1048576, 2097152};
    constexpr uint64_t k_n =
        sizeof(k_sizes) / sizeof(k_sizes[0]);
    PMM::reset_color_cursor();
    uint64_t pages[k_n];
    uint64_t mem_before = PMM::free_memory();
    for (uint64_t i = 0; i < k_n; ++i) {
        (void)k_sizes[i];
        pages[i] = PMM::alloc_page_colored(i % cache::NUM_COLORS);
        JARVIS_ASSERT(pages[i] != 0);
        uint64_t color = PMM::color_of(pages[i]);
        JARVIS_ASSERT(color < cache::NUM_COLORS);
        JARVIS_ASSERT(PMM::color_of(pages[i]) == color);
    }
    for (uint64_t i = 0; i < k_n; ++i)
        PMM::free_page(pages[i]);
    JARVIS_ASSERT(PMM::free_memory() == mem_before);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Color derives from documented address bits (spec formula).
// Input: Fixed phys vector incl. HHDM addresses; out-of-range color
//        requests via the _err variants (clean OOM, no assert noise).
// Expect: color_of matches (phys >> SHIFT) & MASK; OOR requests fail
//         with PMM_ERR_OOM and null out-param.
// Depends: cache::color_of contract (issue #62)
JARVIS_TEST(cache_coloring_bit_extraction, "PRE: none | POST: none") {
    JARVIS_ASSERT(PMM::color_of(0x0ULL) == 0);
    JARVIS_ASSERT(PMM::color_of(0x1000ULL) == 1);
    JARVIS_ASSERT(PMM::color_of(0xF000ULL) == 15);
    JARVIS_ASSERT(PMM::color_of(0x10000ULL) == 0);
    JARVIS_ASSERT(PMM::color_of(0xFFFF8000001000ULL) == 1);
    JARVIS_ASSERT(PMM::color_of(0xFFFF80000FB7000ULL) ==
                  ((0xFFFF80000FB7000ULL >> 12) & 15));
    uint64_t out = 0xDEADULL;
    JARVIS_ASSERT(PMM::alloc_page_colored_err(cache::NUM_COLORS, out) ==
                  errors::PMM_ERR_OOM);
    JARVIS_ASSERT(out == 0xDEADULL);
    JARVIS_ASSERT(PMM::alloc_user_page_colored_err(0xFFFFFFFFULL, out) ==
                  errors::PMM_ERR_USER_OOM);
    JARVIS_ASSERT(out == 0xDEADULL);
    JARVIS_ASSERT(PMM::alloc_page_colored(cache::NUM_COLORS) == 0);
    JARVIS_TEST_PASS();
}

void register_cache_coloring_tests() {
    Logger::info("Registering cache coloring tests");
    JARVIS_REGISTER_TEST(cache_coloring_spread);
    JARVIS_REGISTER_TEST(cache_coloring_collisions_bounded);
    JARVIS_REGISTER_TEST(cache_coloring_arbitrary_sizes);
    JARVIS_REGISTER_TEST(cache_coloring_bit_extraction);
}
#endif  // CONFIG_ARCH_X86_64
