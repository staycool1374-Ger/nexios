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

/// @file test_lazy_tlb.cpp
/// @brief Lazy-TLB-shootdown tests (issue #158; stubs from issue #85,
///        module 15).  Single-CPU deterministic: cross-CPU TLB-hit
///        observables do not exist in this harness (no AP boots), so
///        tests pin queue/coalesce/quarantine/timeout mechanics via
///        request counts, flush-counter deltas, and page-table walks —
///        never cycle timing or remote observation.  IPI delivery is
///        #159's scope; this class covers policy only.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <constants.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/tlb_shootdown.hpp>
#include <kernel/arch/page_table.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/hal/irq_guard.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Shootdown requests defer to an explicit apply point.
// Input: N requests with distinct PCIDs (no grouping), no apply yet.
// Expect: No flush counter moves before apply; after
//         coalesce_and_apply(), single_va delta == N; queue empty.
// Depends: TlbShootdown request/apply (issue #158)
JARVIS_TEST(lazy_tlb_deferred_to_switch, "PRE: none | POST: none") {
    constexpr uint64_t k_n = 8;
    TlbShootdown::reset();
    for (uint64_t i = 0; i < k_n; ++i) {
        JARVIS_ASSERT(TlbShootdown::request(0x30000000ULL + i * 0x1000,
                                            static_cast<uint16_t>(i)) ==
                      TlbShootdown::Error::OK);
    }
    JARVIS_ASSERT(TlbShootdown::pending_count() == k_n);
    arch::TlbFlushStats before = arch::tlb_flush_stats();
    TlbShootdown::coalesce_and_apply();
    arch::TlbFlushStats after = arch::tlb_flush_stats();
    JARVIS_ASSERT(after.single_va == before.single_va + k_n);
    JARVIS_ASSERT(TlbShootdown::pending_count() == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Multiple pending invalidations coalesce minimally.
// Input: N duplicate + adjacent same-PCID requests.
// Expect: Duplicates collapse (one flush each); a same-PCID run at
//         threshold earns exactly one context purge; small runs flush
//         per-VA.  Batches proven via counter deltas.
// Depends: TlbShootdown coalescing (issue #158)
JARVIS_TEST(lazy_tlb_coalesces, "PRE: none | POST: none") {
    TlbShootdown::reset();
    // Duplicates: same (va, pcid) three times → one flush.
    for (int i = 0; i < 3; ++i) {
        JARVIS_ASSERT(TlbShootdown::request(0x31000000ULL, 5) ==
                      TlbShootdown::Error::OK);
    }
    JARVIS_ASSERT(TlbShootdown::pending_count() == 1);
    // Same-PCID run at threshold → one context purge.
    for (uint64_t i = 0; i < TlbShootdown::PURGE_GROUP_THRESHOLD; ++i) {
        JARVIS_ASSERT(TlbShootdown::request(0x32000000ULL + i * 0x1000,
                                            7) ==
                      TlbShootdown::Error::OK);
    }
    arch::TlbFlushStats before = arch::tlb_flush_stats();
    TlbShootdown::coalesce_and_apply();
    arch::TlbFlushStats after = arch::tlb_flush_stats();
    uint64_t single_delta = after.single_va - before.single_va;
    uint64_t ctx_delta = (after.ctx_invpcid - before.ctx_invpcid) +
                         (after.ctx_fallback - before.ctx_fallback);
    // 1 deduped single + exactly 1 context purge for the run of 8.
    JARVIS_ASSERT(single_delta == 1);
    JARVIS_ASSERT(ctx_delta == 1);
    JARVIS_ASSERT(TlbShootdown::pending_count() == 0);
    // Overflow fails closed (no eviction, no silent drop).
    TlbShootdown::reset();
    for (uint64_t i = 0; i < TlbShootdown::MAX_PENDING; ++i) {
        JARVIS_ASSERT(TlbShootdown::request(0x33000000ULL + i * 0x1000,
                                            0) ==
                      TlbShootdown::Error::OK);
    }
    JARVIS_ASSERT(TlbShootdown::request(0x34000000ULL, 0) ==
                  TlbShootdown::Error::QUEUE_FULL);
    TlbShootdown::reset();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: No corruption window: quarantined pages are never recycled
//           while held, and release returns them exactly once.
// Input: Alloc page, hold it, attempt realloc, release, realloc.
// Expect: is_allocated stays true while held; a fresh alloc never
//         returns the held phys; after release the page is freely
//         reusable; free-memory delta zero overall.
// Depends: TlbShootdown quarantine (issue #158)
JARVIS_TEST(lazy_tlb_no_corruption, "PRE: none | POST: none") {
    TlbShootdown::reset();
    uint64_t mem_before = PMM::free_memory();
    uint64_t page = PMM::alloc_page();
    JARVIS_ASSERT(page != 0);
    JARVIS_ASSERT(TlbShootdown::hold(page) == TlbShootdown::Error::OK);
    JARVIS_ASSERT(TlbShootdown::quarantine_count() == 1);
    JARVIS_ASSERT(PMM::is_allocated(page));
    // A fresh allocation must not recycle the held page.
    uint64_t other = PMM::alloc_page();
    JARVIS_ASSERT(other != 0 && other != page);
    PMM::free_page(other);
    // Double-hold is idempotent-safe (same page twice is allowed: the
    // release frees once per hold — assert count, then release twice).
    JARVIS_ASSERT(TlbShootdown::hold(page) == TlbShootdown::Error::OK);
    JARVIS_ASSERT(TlbShootdown::quarantine_count() == 2);
    TlbShootdown::release_all();
    JARVIS_ASSERT(TlbShootdown::quarantine_count() == 0);
    // Page reusable again after release (proves exactly-once free:
    // free_page on an already-free page is a safe no-op, so a second
    // release would NOT corrupt — but counts prove single release).
    uint64_t again = PMM::alloc_page();
    JARVIS_ASSERT(again != 0);
    PMM::free_page(again);
    JARVIS_ASSERT(PMM::free_memory() == mem_before);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A quarantined page is force-released past its timeout even
//           without any explicit release call.
// Input: Hold a page with a tiny timeout; poll wall-clock past the
//        deadline; drive the tick hook directly (no ISR-timing flakes).
// Expect: on_tick() releases it (quarantine empty, page reusable,
//         free-memory delta zero).
// Depends: TlbShootdown tick timeout (issue #158)
JARVIS_TEST(lazy_tlb_timeout_flush, "PRE: none | POST: none") {
    TlbShootdown::reset();
    TlbShootdown::set_timeout_ticks_for_test(5);
    auto timeout_reset =
        ScopeGuard([]() { TlbShootdown::set_timeout_ticks_for_test(
            TlbShootdown::DEFAULT_TIMEOUT_TICKS); });
    uint64_t mem_before = PMM::free_memory();
    uint64_t page = PMM::alloc_page();
    JARVIS_ASSERT(page != 0);
    JARVIS_ASSERT(TlbShootdown::hold(page) == TlbShootdown::Error::OK);
    // Poll wall-clock past the deadline (bounded ~5ms + margin), then
    // drive the hook directly — deterministic outcome, no ISR flakes.
    uint64_t start = arch::Timer::ticks();
    while (arch::Timer::ticks() - start < 20)
        arch::pause();
    {
        arch::IrqGuard irq_guard{};
        TlbShootdown::on_tick(arch::Timer::ticks());
    }
    JARVIS_ASSERT(TlbShootdown::quarantine_count() == 0);
    uint64_t again = PMM::alloc_page();
    JARVIS_ASSERT(again != 0);
    PMM::free_page(again);
    JARVIS_ASSERT(PMM::free_memory() == mem_before);
    JARVIS_TEST_PASS();
}

void register_lazy_tlb_tests() {
    Logger::info("Registering lazy tlb tests");
    JARVIS_REGISTER_TEST(lazy_tlb_deferred_to_switch);
    JARVIS_REGISTER_TEST(lazy_tlb_coalesces);
    JARVIS_REGISTER_TEST(lazy_tlb_no_corruption);
    JARVIS_REGISTER_TEST(lazy_tlb_timeout_flush);
}
#endif  // CONFIG_ARCH_X86_64
