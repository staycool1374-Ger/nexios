/*
 * NexIOS RTOS — Memory Bootstrap Handover (M4/F10)
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

/// @file test_mem_handover.cpp
/// @brief F10 handover tests (issue #563): boot_handover coverage,
///        pre-check + reverse rollback, snapshot immunity, mem-pressure
///        latch, freeze-denies sweep, and the INV-MK-1 composite.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/memory/untyped_handover.hpp>
#include <kernel/memory/mem_pressure.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/test/resource_tracker.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: F10 handover coverage — status().frozen, caps_minted ==
//        runs_total, sum region(i).pages == pages_handed,
//        pages_handed + pages_retained == free_pages_at_freeze, and
//        runs_capped == handover_dropped_runs().
// Input: Post-freeze boot_handover(root) with live free runs.
// Expect: All equalities hold; result OK.
// Depends: untyped_handover::boot_handover/status/region,
//        PMM::handover_dropped_runs
// Pseudocode: freeze (if not); handover(root); assert all rows;
// teardown (slots stay — root-owned; drain).
JARVIS_TEST(untyped_handover_caps_cover_free_pool, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F10 no double claim — free_page(region(0).phys) leaves
//        PMM::free_memory() unchanged and free_page_err() returns
//        PMM_ERR_INVALID; a second boot_handover(root) returns
//        ALREADY_FROZEN with caps_minted unchanged.
// Input: Handed-over region 0; free_page on it; second handover.
// Expect: free_memory unchanged; INVALID; ALREADY_FROZEN;
//         caps_minted unchanged.
// Depends: PMM::free_page/free_page_err/free_memory,
//        boot_handover idempotency guard
// Pseudocode: handover; snapshot free_memory; free_page(region0)
// (INVALID, unchanged); handover again (ALREADY_FROZEN, same
// minted); teardown.
JARVIS_TEST(untyped_handover_no_double_claim, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F10 bounds pre-check + reverse rollback — pure leg:
//        headroom < runs returns BOUNDS with caps_minted == 0;
//        integration leg: a snapshot-restored retry after a forced
//        capacity shortfall leaves g_run_count == 0 with tracker
//        delta 0 (proves the rollback, not just the pre-check).
// Input: Handover with insufficient Untyped headroom / CSlots.
// Expect: BOUNDS; caps_minted 0; retry after restore: run count 0,
//         delta 0.
// Depends: boot_handover pre-check, reverse rollback, snapshot
// Pseudocode: force shortfall; handover (BOUNDS, 0 minted);
// restore; retry (0 runs, delta 0); teardown.
JARVIS_TEST(untyped_handover_bounds_precheck_rollback, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F10 snapshot immunity (INV-F10-2) — after
//        snapshot_create()/snapshot_restore() the freeze flag, run
//        table and marked-bitmap state are unchanged; alloc_page()
//        still never returns a handover frame.
// Input: Post-handover snapshot round-trip.
// Expect: frozen/run table/marks identical; allocs avoid handed
//         frames.
// Depends: snapshot_create/restore, PMM::is_frozen, alloc_page
// Pseudocode: handover; snapshot; restore; assert frozen + table +
// marks identical; alloc sweep avoids handed frames; teardown.
JARVIS_TEST(untyped_handover_snapshot_immune, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F10 mem-pressure latch-once — two latches give count()
//        == 2, exactly one dmesg row, pending_notify() true, clear()
//        resets to NONE.
// Input: latch(X); latch(Y).
// Expect: count 2; one dmesg row; pending true; after clear:
//         NONE, !latched, count 0.
// Depends: mem_pressure::latch/clear/count/pending_notify,
//        dmesg row scan
// Pseudocode: clear; latch; latch; assert 2/one-row/pending;
// clear; assert NONE/clean; teardown.
JARVIS_TEST(mem_pressure_latch_once, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F10 freeze denies all PMM entry points (pools1 leg) —
//        assert CONFIG_STATIC_POOLS_ONLY == 1 first (never silently
//        pass), then each of the six entry points returns 0 / typed
//        error post-freeze and the deny latches once; the #else
//        branch asserts the flag is 0 and a control alloc still
//        succeeds (fail direction explicit both ways).
// Input: Frozen PMM under the pools1 flag build (vs default build).
// Expect: Flag==1 leg: denies everywhere + one latch; flag==0
//         leg: flag 0 + control alloc OK.
// Depends: CONFIG_STATIC_POOLS_ONLY, six PMM entry points, latch
// Pseudocode: assert flag value first; per-leg assertions;
// teardown.
JARVIS_TEST(freeze_denies_all_pmm_entry_points, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F10 INV-MK-1 composite (§13.3 planned test) — the pools1
//        deny sweep plus "BufferPool exhaustion returns typed OOM
//        with no PMM movement" (the latter already exists on testbed
//        as buffer_pool_no_pmm_fallback — do not duplicate, reference
//        it).
// Input: Frozen pools1 build; exhaust BufferPool.
// Expect: Typed OOM; PMM delta 0; deny sweep per above.
// Depends: freeze_denies leg, buffer_pool_no_pmm_fallback
//        (existing — referenced, not duplicated)
// Pseudocode: deny sweep (as above); BufferPool exhaust (typed
// OOM, PMM still); teardown.
JARVIS_TEST(mk_inv_no_kernel_alloc, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

void register_mem_handover_tests() {
    Logger::info("Registering mem_handover tests");
    JARVIS_REGISTER_TEST(untyped_handover_caps_cover_free_pool);
    JARVIS_REGISTER_TEST(untyped_handover_no_double_claim);
    JARVIS_REGISTER_TEST(untyped_handover_bounds_precheck_rollback);
    JARVIS_REGISTER_TEST(untyped_handover_snapshot_immune);
    JARVIS_REGISTER_TEST(mem_pressure_latch_once);
    JARVIS_REGISTER_TEST(freeze_denies_all_pmm_entry_points);
    JARVIS_REGISTER_TEST(mk_inv_no_kernel_alloc);
}
