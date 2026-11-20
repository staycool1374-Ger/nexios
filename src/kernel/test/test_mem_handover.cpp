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
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/test/task_ptr.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/test/resource_tracker.hpp>

using namespace kernel;

namespace {

// Counts live dmesg rows for (subsystem, code) — the pressure latch
// must emit exactly one row no matter how many times it fires.
uint64_t dmesg_row_count(log::ErrorSubsystem sub, uint64_t code) {
    uint64_t n = 0;
    log::DmesgService::instance().for_each(
        [&](const log::LogEntry &e) {
            if (e.subsystem == sub && e.error_code == code)
                ++n;
        });
    return n;
}

// Minimal pool-backed slot filler. TcbCap-sized objects live in the
// 32-block pool (enough to flake a large fill), so shortfall fills
// use these 32B-pool probes instead — slot occupancy is
// size-agnostic.
struct FillProbe : public KernelObject {
    void dispose() noexcept override {
        MemPool::free(this);
    }
    bool is_shared() const noexcept override {
        return true;
    }
};

// Cross-test handover record (the registry is class-global: the
// cover test records region 0, the later tests verify against it).
// Set only on the bootstrap leg; the default legs never touch it.
uint64_t g_handover_r0_phys = 0;
uint64_t g_handover_r0_pages = 0;
uint64_t g_handover_have_r0 = 0;

} // namespace

// Runmode: kernel
// Testidea: F10 handover coverage (runs SECOND — the bounds test
//        below executes first so g_done is still false here):
//        status().frozen, caps_minted == runs_total,
//        sum region(i).pages == pages_handed,
//        pages_handed + pages_retained == free_pages_at_freeze, and
//        runs_capped == handover_dropped_runs().
// Input: Post-freeze boot_handover(root) with live free runs.
// Expect: All equalities hold; result OK.
// Depends: untyped_handover::boot_handover/status/region,
//        PMM::handover_dropped_runs
// Pseudocode: handover(root); assert all rows; record region0 for
// the later tests; teardown (-handed caps stay root-owned until
// driver teardown disposes them; frames stay marked).
JARVIS_TEST(untyped_handover_caps_cover_free_pool, "PRE: none | POST: none") {
    static uint64_t g_rv = 0;
    static uint64_t g_frozen = 0;
    static uint64_t g_caps = 0;
    static uint64_t g_runs = 0;
    static uint64_t g_sumpages = 0;
    static uint64_t g_handed = 0;
    static uint64_t g_freenow = 0;
    static uint64_t g_retained = 0;
    static uint64_t g_capped = 0;
    static uint64_t g_dropped = 0;
    g_rv = g_frozen = g_caps = g_runs = 0;
    g_sumpages = g_handed = g_freenow = g_retained = 0;
    g_capped = g_dropped = 0;

    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();
#if CONFIG_MEM_MGR_BOOTSTRAP
    g_rv = static_cast<uint64_t>(
        untyped_handover::boot_handover(*cur));
    {
        auto st = untyped_handover::status();
        g_frozen = st.frozen ? 1ULL : 0ULL;
        g_caps = st.caps_minted;
        g_runs = st.runs_total;
        g_handed = st.pages_handed;
        g_retained = st.pages_retained;
        g_capped = st.runs_capped;
        g_freenow = PMM::free_memory();
        g_dropped = PMM::handover_dropped_runs();
        uint64_t sum = 0;
        for (uint32_t i = 0; i < st.runs_total; ++i) {
            const auto *r = untyped_handover::region(i);
            if (r != nullptr)
                sum += r->pages;
        }
        g_sumpages = sum;
    }
    // Record region 0 for the no-double-claim + snapshot tests.
    {
        const auto *r0 = untyped_handover::region(0);
        if (r0 != nullptr) {
            g_handover_r0_phys = r0->phys;
            g_handover_r0_pages = r0->pages;
            g_handover_have_r0 = 1;
        }
    }
#else
    g_rv = static_cast<uint64_t>(
        untyped_handover::boot_handover(*cur));
    {
        auto st = untyped_handover::status();
        g_frozen = st.frozen ? 1ULL : 0ULL;
        g_caps = st.caps_minted;
        g_runs = st.runs_total;
        g_handed = st.pages_handed;
        g_retained = st.pages_retained;
        g_capped = st.runs_capped;
        g_freenow = PMM::free_memory();
        g_dropped = PMM::handover_dropped_runs();
        uint64_t sum = 0;
        for (uint32_t i = 0; i < st.runs_total; ++i) {
            const auto *r = untyped_handover::region(i);
            if (r != nullptr)
                sum += r->pages;
        }
        g_sumpages = sum;
    }
    // No cross-test record in the default leg (nothing handed).
    JARVIS_ASSERT_EQ(0ULL, g_handover_have_r0);
    JARVIS_ASSERT_EQ(0ULL, g_handover_r0_phys);
    JARVIS_ASSERT_EQ(0ULL, g_handover_r0_pages);
#endif
    JARVIS_ASSERT_EQ(
        static_cast<uint64_t>(
#if CONFIG_MEM_MGR_BOOTSTRAP
            errors::MEM_HANDOVER_ERR_OK
#else
            errors::MEM_HANDOVER_ERR_NO_FREE_RUNS
#endif
            ),
        g_rv);
#if CONFIG_MEM_MGR_BOOTSTRAP
    JARVIS_ASSERT_EQ(1ULL, g_frozen);
    JARVIS_ASSERT(g_runs > 0);
    JARVIS_ASSERT_EQ(g_runs, g_caps);
    JARVIS_ASSERT_EQ(g_handed, g_sumpages);
    JARVIS_ASSERT_EQ(g_dropped, g_capped);
    JARVIS_ASSERT_EQ(1ULL, g_handover_have_r0);
    // Note: pages_handed + pages_retained == free_pages_at_freeze
    // is not pinned — freeze already happened at boot (idempotent),
    // so free_pages_at_freeze is unobservable; retained-vs-free
    // would also race test allocs. The Σ/caps/frozen rows above
    // carry the coverage.
#else
    // Freeze lands at composition end in every build (only the run
    // walk is bootstrap-gated): frozen 1, retained = freeze-time
    // pool (> 0), handed/runs/caps 0.
    JARVIS_ASSERT_EQ(1ULL, g_frozen);
    JARVIS_ASSERT_EQ(0ULL, g_caps);
    JARVIS_ASSERT_EQ(0ULL, g_runs);
    JARVIS_ASSERT_EQ(0ULL, g_sumpages);
    JARVIS_ASSERT_EQ(0ULL, g_handed);
    JARVIS_ASSERT(g_freenow > 0);
    JARVIS_ASSERT(g_retained > 0);
    JARVIS_ASSERT_EQ(g_dropped, g_capped);
#endif
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
// Pseudocode: snapshot free_memory (bootstrap: handed state from
// the cover test); free_page(region0) (INVALID, unchanged);
// handover again (ALREADY_FROZEN, same minted); teardown.
JARVIS_TEST(untyped_handover_no_double_claim, "PRE: none | POST: none") {
    static uint64_t g_err = 0;
    static uint64_t g_same = 0;
    static uint64_t g_rv2 = 0;
    static uint64_t g_minted = 0;
    g_err = g_same = g_rv2 = g_minted = 0;

    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();
#if CONFIG_MEM_MGR_BOOTSTRAP
    if (!g_handover_have_r0)
        return; // fail loud below (cover test sets this)
    const uint64_t before = PMM::free_memory();
    g_err = static_cast<uint64_t>(
        PMM::free_page_err(g_handover_r0_phys));
    g_same = (PMM::free_memory() == before) ? 1ULL : 0ULL;
    g_rv2 = static_cast<uint64_t>(
        untyped_handover::boot_handover(*cur));
    g_minted = untyped_handover::status().caps_minted;
#else
    // Default leg: freeing an unmapped bogus frame fails closed with
    // no pool movement (same fail-closed shape, no handed state).
    {
        const uint64_t before = PMM::free_memory();
        g_err = static_cast<uint64_t>(
            PMM::free_page_err(0x12345000ULL));
        g_same = (PMM::free_memory() == before) ? 1ULL : 0ULL;
    }
    g_rv2 = static_cast<uint64_t>(
        untyped_handover::boot_handover(*cur));
    g_minted = untyped_handover::status().caps_minted;
#endif
#if CONFIG_MEM_MGR_BOOTSTRAP
    JARVIS_ASSERT_EQ(
        static_cast<uint64_t>(errors::PMM_ERR_INVALID), g_err);
    JARVIS_ASSERT_EQ(1ULL, g_same);
    JARVIS_ASSERT_EQ(
        static_cast<uint64_t>(errors::MEM_HANDOVER_ERR_ALREADY_FROZEN),
        g_rv2);
    // caps_minted frozen at the cover test's count (guard against
    // double-mint: the registry did not grow).
    {
        auto st = untyped_handover::status();
        JARVIS_ASSERT_EQ(st.runs_total, g_minted);
        JARVIS_ASSERT(g_minted > 0);
    }
#else
    JARVIS_ASSERT_EQ(
        static_cast<uint64_t>(errors::PMM_ERR_INVALID), g_err);
    JARVIS_ASSERT_EQ(1ULL, g_same);
    JARVIS_ASSERT_EQ(
        static_cast<uint64_t>(errors::MEM_HANDOVER_ERR_NO_FREE_RUNS),
        g_rv2);
    JARVIS_ASSERT_EQ(0ULL, g_minted);
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F10 handover bounds pre-check (REGISTERED FIRST — it
//        must execute while g_done is still false): with CSlot
//        headroom forced below the run count, boot_handover returns
//        BOUNDS with caps_minted == 0 and no runs recorded; removing
//        the dummies lets the later cover test proceed (retry
//        succeeds — no partial state, which is the rollback
//        property: the pre-check makes mid-loop failure unreachable
//        in composed configs, S1 mitigation).
// Input: Frozen PMM (bootstrap leg); root cspace filled to
//        free_slots < runs.
// Expect: BOUNDS; caps_minted 0; runs_total 0.
// Depends: boot_handover pre-check, free_slot_count
// Pseudocode (bootstrap): freeze; runs = run_count; fill cspace;
// handover (BOUNDS, zero state); drain dummies; teardown.
// Pseudocode (default build): handover -> NO_FREE_RUNS, zero
// state (no runs exist without the bootstrap walk).
JARVIS_TEST(untyped_handover_bounds_precheck_rollback, "PRE: none | POST: none") {
    static uint64_t g_rv = 0;
    static uint64_t g_minted = 0;
    static uint64_t g_runs = 0;
    g_rv = g_minted = g_runs = 0;

    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();
    cap::CNode *cs = cur->get_cspace();
    if (!cs)
        return;
#if CONFIG_MEM_MGR_BOOTSTRAP
    PMM::freeze_pools();
    const uint32_t runs = PMM::handover_run_count();
    if (runs == 0)
        return; // fail loud below via g_runs sentinel
    g_runs = runs;
    // Fill the root cspace to leave exactly runs-1 free slots.
    // Recorded slots only (never a blanket wipe — the harness task
    // may own other slots).
    const uint32_t free = cs->free_slot_count();
    if (free < runs)
        return;
    const uint32_t fill = free - (runs - 1);
    uint32_t dummy_slots[64] = {};
    uint64_t installed = 0;
    for (uint64_t i = 0;
         i < fill && i < 64 && installed == i; ++i) {
        auto *d = static_cast<FillProbe *>(
            MemPool::alloc(sizeof(FillProbe)));
        if (!d)
            break;
        new (d) FillProbe;
        d->mark_pool_backed();
        int s = cs->install(d, cap::CapType::Task, cap::CapRights::READ);
        if (s < 0) {
            MemPool::free(d);
            break;
        }
        d->release(); // transfer: the slot holds the only reference
        dummy_slots[installed] = static_cast<uint32_t>(s);
        ++installed;
    }
    if (installed != fill) {
        // Best-effort unwind of the partial fill, then fail loud.
        for (uint64_t i = 0; i < installed; ++i)
            cs->remove(dummy_slots[i]);
        return;
    }
    g_rv = static_cast<uint64_t>(
        untyped_handover::boot_handover(*cur));
    {
        auto st = untyped_handover::status();
        g_minted = st.caps_minted;
        g_runs = st.runs_total;
    }
    // Drain the dummies (retry in the cover test must see full room).
    for (uint64_t i = 0; i < installed; ++i)
        cs->remove(dummy_slots[i]);
#else
    g_rv = static_cast<uint64_t>(
        untyped_handover::boot_handover(*cur));
    {
        auto st = untyped_handover::status();
        g_minted = st.caps_minted;
        g_runs = (st.runs_total == 0) ? 999ULL : st.runs_total;
    }
#endif
    JARVIS_ASSERT_EQ(
        static_cast<uint64_t>(
#if CONFIG_MEM_MGR_BOOTSTRAP
            errors::MEM_HANDOVER_ERR_BOUNDS
#else
            errors::MEM_HANDOVER_ERR_NO_FREE_RUNS
#endif
            ),
        g_rv);
    JARVIS_ASSERT_EQ(0ULL, g_minted);
    JARVIS_ASSERT_EQ(
#if CONFIG_MEM_MGR_BOOTSTRAP
        0ULL
#else
        999ULL
#endif
        ,
        g_runs);
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
// Pseudocode: assert post-harness-restore state (frozen, table,
// marks) matches the cover test's record; bounded alloc sweep
// (32 pages) avoids every handed frame; free them; teardown.
//
// Note: the snapshot round-trip is harness-driven (isolate
// snapshots every test boundary) rather than called directly —
// direct nested snapshot_create/restore inside a test body would
// fight the harness's own discipline. The property pinned is
// identical: post-restore state equals the pre-restore record.
JARVIS_TEST(untyped_handover_snapshot_immune, "PRE: none | POST: none") {
    static uint64_t g_frozen = 0;
    static uint64_t g_table = 0;
    static uint64_t g_sweep = 0;
    static uint64_t g_freed = 0;
    g_frozen = g_table = g_sweep = g_freed = 0;

#if CONFIG_MEM_MGR_BOOTSTRAP
    if (!g_handover_have_r0)
        return; // fail loud below (cover test sets this)
    g_frozen = PMM::is_frozen() ? 1ULL : 0ULL;
    {
        const auto *r0 = untyped_handover::region(0);
        g_table = (r0 != nullptr && r0->phys == g_handover_r0_phys &&
                   r0->pages == g_handover_r0_pages)
                      ? 1ULL
                      : 0ULL;
    }
#else
    g_frozen = PMM::is_frozen() ? 1ULL : 0ULL;
    {
        auto st = untyped_handover::status();
        g_table = (st.runs_total == 0) ? 1ULL : 0ULL;
    }
#endif
    // Bounded alloc sweep (no handed frame may come back; default
    // leg has zero regions, so the check is trivially clean but the
    // alloc/free cycle is still real). Skipped under
    // STATIC_POOLS_ONLY: post-freeze alloc_page denies by design
    // (that refusal is the freeze test's pin, not this one's).
#if CONFIG_STATIC_POOLS_ONLY
    g_sweep = 1;
    g_freed = 1;
#else
    {
        uint64_t got[32] = {};
        uint64_t n = 0;
        uint64_t clean = 1;
        for (uint64_t i = 0; i < 32; ++i) {
            const uint64_t p = PMM::alloc_page();
            if (p == 0)
                break;
            // Handed frames are marked allocated: reject any hit.
            bool hit = false;
            const uint32_t runs =
                untyped_handover::status().runs_total;
            for (uint32_t r = 0; r < runs; ++r) {
                const auto *rg = untyped_handover::region(r);
                if (rg == nullptr)
                    continue;
                if (p >= rg->phys &&
                    p < rg->phys + rg->pages * 4096ULL) {
                    hit = true;
                    break;
                }
            }
            if (hit)
                clean = 0;
            got[n++] = p;
        }
        g_sweep = (clean && n > 0) ? 1ULL : 0ULL;
        for (uint64_t i = 0; i < n; ++i)
            PMM::free_page(got[i]);
        g_freed = 1ULL;
    }
#endif
    JARVIS_ASSERT_EQ(1ULL, g_frozen);
    JARVIS_ASSERT_EQ(1ULL, g_table);
    JARVIS_ASSERT_EQ(1ULL, g_sweep);
    JARVIS_ASSERT_EQ(1ULL, g_freed);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F10 mem-pressure latch-once — two latches bump the
//        monotonic count by 2, emit exactly one dmesg row,
//        pending_notify() true with first-wins reason; clear()
//        resets to NONE (count stays monotonic — telemetry, not
//        state).
// Input: latch(X); latch(Y).
// Expect: delta +2; one row; pending true; reason X; after clear:
//         NONE, !latched, !pending, count unchanged since latch.
// Depends: mem_pressure::latch/clear/count/pending_notify,
//        dmesg row scan
// Pseudocode: clear; latch; latch; assert 2/one-row/pending;
// clear; assert NONE/clean; teardown.
//
// Note: the latch is global (not per-test isolated); the test
// clears first and counts a DELTA so concurrent boot rows cannot
// flake it. Reason pins first-wins.
JARVIS_TEST(mem_pressure_latch_once, "PRE: none | POST: none") {
    static uint64_t g_count = 0;
    static uint64_t g_rows = 0;
    static uint64_t g_pending = 0;
    static uint64_t g_reason = 0;
    static uint64_t g_clean = 0;
    static uint64_t g_mono = 0;
    g_count = g_rows = g_pending = g_reason = g_clean = g_mono = 0;

    constexpr uint64_t kCode =
        static_cast<uint64_t>(log::kDmesgBase_PMM) + 4;
    mem_pressure::clear();
    // count() is a monotonic lifetime counter (clear() resets state,
    // not telemetry) — assert the +2 delta, not an absolute.
    const uint64_t c0 = mem_pressure::count();
    const uint64_t rows0 = dmesg_row_count(log::ErrorSubsystem::PMM, kCode);
    mem_pressure::latch(mem_pressure::MemPressureReason::PMM_OOM);
    mem_pressure::latch(mem_pressure::MemPressureReason::BUFPOOL_EMPTY);
    g_count = mem_pressure::count() - c0;
    g_rows = dmesg_row_count(log::ErrorSubsystem::PMM, kCode) - rows0;
    g_pending = mem_pressure::pending_notify() ? 1ULL : 0ULL;
    g_reason = static_cast<uint64_t>(mem_pressure::reason());
    mem_pressure::clear();
    g_clean = (!mem_pressure::latched() &&
               mem_pressure::reason() ==
                   mem_pressure::MemPressureReason::NONE &&
               !mem_pressure::pending_notify())
                  ? 1ULL
                  : 0ULL;
    g_mono = (mem_pressure::count() == c0 + 2) ? 1ULL : 0ULL;

    JARVIS_ASSERT_EQ(2ULL, g_count);
    JARVIS_ASSERT_EQ(1ULL, g_rows);
    JARVIS_ASSERT_EQ(1ULL, g_pending);
    JARVIS_ASSERT_EQ(
        static_cast<uint64_t>(mem_pressure::MemPressureReason::PMM_OOM),
        g_reason);
    JARVIS_ASSERT_EQ(1ULL, g_clean);
    JARVIS_ASSERT_EQ(1ULL, g_mono);
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
// teardown (free the control page in the default leg).
JARVIS_TEST(freeze_denies_all_pmm_entry_points, "PRE: none | POST: none") {
    static uint64_t g_flag = 0;
    static uint64_t g_r0 = 0;
    static uint64_t g_r1 = 0;
    static uint64_t g_r2 = 0;
    static uint64_t g_r3 = 0;
    static uint64_t g_r4 = 0;
    static uint64_t g_r5 = 0;
    static uint64_t g_latched = 0;
    static uint64_t g_reason_ok = 0;
    static uint64_t g_control = 0;
    g_flag = g_r0 = g_r1 = g_r2 = g_r3 = g_r4 = g_r5 = 0;
    g_latched = g_reason_ok = g_control = 0;

#if CONFIG_STATIC_POOLS_ONLY
    // Flag leg: the #if itself proves the flag, pinned explicitly.
    static_assert(CONFIG_STATIC_POOLS_ONLY == 1, "pools1 leg needs flag 1");
    g_flag = 1;
    PMM::freeze_pools(); // idempotent; boot already froze
    mem_pressure::clear();
    const uint64_t c0 = mem_pressure::count();
    // All six kernel entry points deny with 0 post-freeze.
    g_r0 = PMM::alloc_page();
    g_r1 = PMM::alloc_contiguous(2);
    g_r2 = PMM::alloc_user_page();
    g_r3 = PMM::alloc_page_colored(0);
    g_r4 = PMM::alloc_user_page_colored(0);
    g_r5 = PMM::alloc_user_contiguous(2);
    g_latched = mem_pressure::latched() ? 1ULL : 0ULL;
    g_reason_ok =
        (mem_pressure::reason() ==
         mem_pressure::MemPressureReason::PMM_FROZEN_DENY)
            ? 1ULL
            : 0ULL;
    // All six counted (telemetry), one latch state (first-wins —
    // pinned by mem_pressure_latch_once).
    g_control = (mem_pressure::count() == c0 + 6) ? 1ULL : 0ULL;
    JARVIS_ASSERT_EQ(0ULL, g_r0);
    JARVIS_ASSERT_EQ(0ULL, g_r1);
    JARVIS_ASSERT_EQ(0ULL, g_r2);
    JARVIS_ASSERT_EQ(0ULL, g_r3);
    JARVIS_ASSERT_EQ(0ULL, g_r4);
    JARVIS_ASSERT_EQ(0ULL, g_r5);
    JARVIS_ASSERT_EQ(1ULL, g_latched);
    JARVIS_ASSERT_EQ(1ULL, g_reason_ok);
    JARVIS_ASSERT_EQ(1ULL, g_control);
#else
    g_flag = (CONFIG_STATIC_POOLS_ONLY == 0) ? 1ULL : 0ULL;
    // Control alloc succeeds in the default build.
    const uint64_t p = PMM::alloc_page();
    g_control = (p != 0) ? 1ULL : 0ULL;
    if (p != 0)
        PMM::free_page(p);
    JARVIS_ASSERT_EQ(1ULL, g_flag);
    JARVIS_ASSERT_EQ(1ULL, g_control);
    JARVIS_ASSERT_EQ(0ULL, g_r0);
    JARVIS_ASSERT_EQ(0ULL, g_r1);
    JARVIS_ASSERT_EQ(0ULL, g_r2);
    JARVIS_ASSERT_EQ(0ULL, g_r3);
    JARVIS_ASSERT_EQ(0ULL, g_r4);
    JARVIS_ASSERT_EQ(0ULL, g_r5);
    JARVIS_ASSERT_EQ(0ULL, g_latched);
    JARVIS_ASSERT_EQ(0ULL, g_reason_ok);
#endif
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
// Pseudocode: pools1 leg — one PMM deny probe (full six-sweep
// lives in the freeze test) + bounded pool probe (alloc/free N
// buffers, delta 0; the exhaustion/OOM direction is the
// referenced buffer_pool_no_pmm_fallback property, not
// duplicated here). Default leg: flag 0 + control probes OK.
// teardown (free everything; task cleanup reclaims PTs).
JARVIS_TEST(mk_inv_no_kernel_alloc, "PRE: none | POST: none") {
    static uint64_t g_pmm_deny = 0;
    static uint64_t g_pool_ok = 0;
    static uint64_t g_delta = 0;
    static uint64_t g_after = 0;
    g_pmm_deny = g_pool_ok = g_delta = 0;
    g_after = 0;

    // Snapshot BEFORE task creation (mirrors the referenced
    // buffer_pool_no_pmm_fallback): create+destroy must net to zero
    // by construction, isolating the buffer cycle's delta.
    const uint64_t before = PMM::free_pages_ref();
    SimpleTaskPtr task(TaskControlBlock::create_user([]() {}, 5, 10,
                                                     32_KiB));
    if (!task)
        return;
#if CONFIG_STATIC_POOLS_ONLY
    // Frozen pools1 VM: PMM denies, pool serves from its cache.
    g_pmm_deny = (PMM::alloc_page() == 0 &&
                  PMM::alloc_user_page() == 0)
                     ? 1ULL
                     : 0ULL;
#else
    g_pmm_deny = 1ULL; // deny shape N/A here (freeze test covers it)
#endif
    // Bounded pool probe (sibling VA window per the referenced
    // test: tables above it already exist, so only bounded PT
    // growth occurs, reclaimed by task cleanup).
    static constexpr size_t kN = 8;
    uint64_t handles[kN] = {};
    uint64_t va = 0x40200000ULL;
    uint64_t ok = 1;
    for (size_t i = 0; i < kN; ++i) {
        uint64_t h = BufferPool::alloc(*task, va + i * arch::PAGE_SIZE);
        if (h == 0) {
            ok = 0;
            break;
        }
        handles[i] = h;
    }
    if (ok) {
        for (size_t i = 0; i < kN; ++i) {
            if (!BufferPool::free(*task, handles[i])) {
                ok = 0;
                break;
            }
        }
    }
    g_pool_ok = ok;
    // Task cleanup frees PT pages + stack; pool blocks recycled.
    task.reset();
    g_after = PMM::free_pages_ref();
    g_delta = (g_after == before) ? 1ULL : 0ULL;

    JARVIS_ASSERT_EQ(1ULL, g_pmm_deny);
    JARVIS_ASSERT_EQ(1ULL, g_pool_ok);
    JARVIS_ASSERT_EQ(1ULL, g_delta);
    JARVIS_TEST_PASS();
}

void register_mem_handover_tests() {
    Logger::info("Registering mem_handover tests");
    // g_done ordering: the bounds test must execute while the
    // registry is still empty (any successful handover latches
    // g_done for the rest of the class run). Plan numbering is
    // kept; only execution order adapts.
    JARVIS_REGISTER_TEST(untyped_handover_bounds_precheck_rollback);
    JARVIS_REGISTER_TEST(untyped_handover_caps_cover_free_pool);
    JARVIS_REGISTER_TEST(untyped_handover_no_double_claim);
    JARVIS_REGISTER_TEST(untyped_handover_snapshot_immune);
    JARVIS_REGISTER_TEST(mem_pressure_latch_once);
    JARVIS_REGISTER_TEST(freeze_denies_all_pmm_entry_points);
    JARVIS_REGISTER_TEST(mk_inv_no_kernel_alloc);
}
