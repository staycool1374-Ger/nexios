/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
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

/// @file test_pmm.cpp
/// @brief Physical memory manager tests.

#include <test.hpp>
#include <logger.hpp>
#include <constants.hpp>
#include <kernel/memory/pmm.hpp>

using namespace kernel;

JARVIS_TEST(pmm_alloc_free, "PRE: none | POST: none") {
    uint64_t before = PMM::free_memory();
    uint64_t p1 = PMM::alloc_page();
    JARVIS_ASSERT(p1 != 0);
    JARVIS_ASSERT(PMM::free_memory() == before - 4096);
    PMM::free_page(p1);
    JARVIS_ASSERT(PMM::free_memory() == before);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(pmm_alloc_contiguous, "PRE: none | POST: none") {
    uint64_t before = PMM::free_memory();
    uint64_t pages = PMM::alloc_contiguous(4);
    JARVIS_ASSERT(pages != 0);
    JARVIS_ASSERT(PMM::free_memory() <= before - 4 * 4096);
    for (size_t i = 0; i < 4; ++i)
        PMM::free_page(pages + i * 4096);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(pmm_user_alloc, "PRE: none | POST: none") {
    uint64_t p = PMM::alloc_user_page();
    JARVIS_ASSERT(p != 0);
    JARVIS_ASSERT(PMM::is_user_page(p));
    PMM::free_page(p);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(pmm_total_memory, "PRE: none | POST: none") {
    uint64_t total = PMM::total_memory();
    JARVIS_ASSERT(total > 0);
    // Total memory should be at least the PMM window size (128 MiB default)
    JARVIS_ASSERT(total >= arch::HHDM_WINDOW_SIZE);
    // Free memory should be less than or equal to total
    JARVIS_ASSERT(PMM::free_memory() <= total);
    // Free memory should be positive (we have at least some free pages)
    JARVIS_ASSERT(PMM::free_memory() > 0);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(pmm_alloc_page_table, "PRE: none | POST: none") {
    uint64_t pt = PMM::alloc_page_table();
    JARVIS_ASSERT(pt != 0);
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_ASSERT(pt < 128ULL * 1024 * 1024);
#endif
    PMM::free_page(pt);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: PMM allocatable-window geometry helper computes the clamped
//           [base,end) page range for synthetic memory layouts.
// Input: compute_window_pages on x86-style (base 0) and DTB-style
//        (base 0x40000000) layouts, including end-clamped and degenerate
//        layouts.
// Expect: x86 base 0 with a 64 MiB span -> {0, 16384}; aarch64 virt layout
//         (base 0x40000000, RAM up to 0x50000000) -> {262144, 294912};
//         small RAM clamps end to total pages; base beyond the span yields
//         an empty (base == end) window.
// Depends: kernel/memory/pmm.hpp
JARVIS_TEST(pmm_window_geometry, "PRE: none | POST: none") {
    // x86 layout: RAM at 0, 64 MiB bitmap span -> window clamped to 16384
    // pages (span smaller than the 128 MiB window).
    PMM::WindowPages x86 = PMM::compute_window_pages(64_MiB, 0);
    JARVIS_ASSERT_EQ(x86.base_page, 0u);
    JARVIS_ASSERT_EQ(x86.end_page, 16384u);
    // aarch64 QEMU virt layout: RAM [0x40000000, 0x50000000), full 128 MiB
    // window fits inside the span.
    PMM::WindowPages virt =
        PMM::compute_window_pages(0x50000000ULL, 0x40000000ULL);
    JARVIS_ASSERT_EQ(virt.base_page, 262144u);
    JARVIS_ASSERT_EQ(virt.end_page, 294912u);
    // End clamped when RAM ends inside the window.
    PMM::WindowPages small =
        PMM::compute_window_pages(0x44000000ULL, 0x40000000ULL);
    JARVIS_ASSERT_EQ(small.base_page, 262144u);
    JARVIS_ASSERT_EQ(small.end_page, 278528u);
    // Degenerate: window base beyond the bitmap span -> empty window.
    PMM::WindowPages empty = PMM::compute_window_pages(0x1000ULL, 0x40000000ULL);
    JARVIS_ASSERT_EQ(empty.base_page, empty.end_page);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Live PMM window state matches the running architecture's MMU-
//           mapped RAM (guards the arch-specific window base at runtime).
// Input: PMM::window_base_page()/window_end_page() after PMM init.
// Expect: window non-empty; x86_64 base_page == 0 and end == min(window,
//         total - base) pages; x86_64 base_page == 0; aarch64
//         base_page == 0x40000000 / PAGE_SIZE.
// Depends: kernel/memory/pmm.hpp, constants.hpp
JARVIS_TEST(pmm_window_live_state, "PRE: PMM init | POST: none") {
    uint64_t win_base = PMM::window_base_page();
    uint64_t win_end = PMM::window_end_page();
    JARVIS_ASSERT(win_end > win_base);
    uint64_t total_pages = PMM::total_memory() / arch::PAGE_SIZE;
    uint64_t window_pages = arch::HHDM_WINDOW_SIZE / arch::PAGE_SIZE;
    uint64_t want_end =
        win_base + ((window_pages < total_pages - win_base)
                        ? window_pages
                        : (total_pages - win_base));
    JARVIS_ASSERT_EQ(win_end, want_end);
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_ASSERT_EQ(win_base, 0u);
#elif defined(CONFIG_ARCH_AARCH64)
    JARVIS_ASSERT_EQ(win_base, 0x40000000ULL / arch::PAGE_SIZE);
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Every page the PMM hands out lies inside the allocatable window.
// Input: alloc_page() and alloc_contiguous(4) after PMM init.
// Expect: single page and 4-page block both within [window_base,
//         window_end) physical bounds; everything freed again (net-zero
//         PMM delta).
// Depends: kernel/memory/pmm.hpp
JARVIS_TEST(pmm_alloc_within_window, "PRE: none | POST: none") {
    uint64_t base_pa = PMM::window_base_page() * arch::PAGE_SIZE;
    uint64_t end_pa = PMM::window_end_page() * arch::PAGE_SIZE;
    uint64_t page = PMM::alloc_page();
    JARVIS_ASSERT(page != 0);
    JARVIS_ASSERT(page >= base_pa);
    JARVIS_ASSERT(page < end_pa);
    uint64_t block = PMM::alloc_contiguous(4);
    JARVIS_ASSERT(block != 0);
    JARVIS_ASSERT(block >= base_pa);
    JARVIS_ASSERT(block + 4 * arch::PAGE_SIZE <= end_pa);
    PMM::free_page(page);
    for (size_t idx = 0; idx < 4; ++idx)
        PMM::free_page(block + idx * arch::PAGE_SIZE);
    JARVIS_TEST_PASS();
}

void register_pmm_tests() {
    Logger::info("Registering PMM tests");
    JARVIS_REGISTER_TEST(pmm_alloc_free);
    JARVIS_REGISTER_TEST(pmm_alloc_contiguous);
    JARVIS_REGISTER_TEST(pmm_user_alloc);
    JARVIS_REGISTER_TEST(pmm_total_memory);
    JARVIS_REGISTER_TEST(pmm_alloc_page_table);
    JARVIS_REGISTER_TEST(pmm_window_geometry);
    JARVIS_REGISTER_TEST(pmm_window_live_state);
    JARVIS_REGISTER_TEST(pmm_alloc_within_window);
}
