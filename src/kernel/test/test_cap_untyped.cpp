/*
 * NexIOS RTOS — Capability-Based Access Control (CSpace)
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

/// @file test_cap_untyped.cpp
/// @brief Untyped memory allocator tests (ROADMAP 0.4.1 item 3): UntypedMem
///        lifecycle and cap::retype ownership transfer.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/untyped.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/page_table.hpp>
#include "test_sched_helpers.hpp"
#include "task_ptr.hpp"

using namespace kernel;

namespace {

/// @brief Runs @p entry as a real task, waits for termination and drains
///        zombies (test_cap_mmio pattern).
TaskControlBlock *run_cap_task(void (*entry)(), uint64_t prio = 11,
                               uint64_t period = 10) {
    auto *t = TaskControlBlock::create(entry, prio, period);
    if (t == nullptr)
        return nullptr;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();
    return t;
}

/// @brief Index of the first occupied slot of @p type whose target is not
///        @p exclude; -1 when none.  Bounded scan (CONFIG_CSLOT_COUNT).
int find_slot_of(const cap::CNode &node, cap::CapType type,
                 const KernelObject *exclude) noexcept {
    for (uint32_t i = 0; i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT); ++i) {
        KernelObject *obj = node.peek(i, type);
        if (obj && obj != exclude)
            return static_cast<int>(i);
    }
    return -1;
}

// -- shared result flags for the SYS_CAP_RETYPE dispatch task --
uint64_t g_rtype_ret = 0;
uint64_t g_rtype_base = 0;
uint64_t g_rtype_frame_ok = 0;
uint64_t g_rtype_child_ok = 0;

/// @brief Installs a 4-page Untyped and dispatches SYS_CAP_RETYPE for a
///        1-page sub-range carve; verifies frame + child slots; teardown.
void cap_retype_happy_entry() {
    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();
    cap::CNode *cs = cur->get_cspace();
    const size_t sz = 4 * arch::PAGE_SIZE;
    auto *ut = cap::UntypedMem::create(sz, false);
    if (!ut) {
        g_rtype_ret = 99;
        Scheduler::terminate(*cur, 0);
        return;
    }
    g_rtype_base = ut->phys;
    int s = cs->install(ut, cap::CapType::Untyped,
                        cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    if (s < 0) {
        ut->release();
        g_rtype_ret = 98;
        Scheduler::terminate(*cur, 0);
        return;
    }
    uint64_t h = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                    cs->slot_gen(static_cast<uint32_t>(s)));
    uint64_t ret = Syscall::handle(
        static_cast<uint64_t>(SyscallNumber::CAP_RETYPE), h,
        static_cast<uint64_t>(cap::CapType::Frame),
        static_cast<uint64_t>(arch::PAGE_SIZE),
        static_cast<uint64_t>(cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE),
        nullptr);
    g_rtype_ret = ret;
    if (ret < static_cast<uint64_t>(CONFIG_CSLOT_COUNT)) {
        KernelObject *target = cap::lookup(
            cs, cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(ret),
                                   cs->slot_gen(static_cast<uint32_t>(ret))),
            cap::CapType::Frame, cap::CAP_RIGHT_READ);
        if (target) {
            auto *fc = static_cast<cap::FrameCap *>(target);
            g_rtype_frame_ok =
                (fc->phys == g_rtype_base && fc->count == 1) ? 1 : 0;
            target->release();
        } else {
            g_rtype_frame_ok = 0;
        }
    }
    int child_idx = find_slot_of(*cs, cap::CapType::Untyped, ut);
    if (child_idx >= 0) {
        auto *child = static_cast<cap::UntypedMem *>(
            cs->peek(static_cast<uint32_t>(child_idx), cap::CapType::Untyped));
        g_rtype_child_ok =
            (child->phys == g_rtype_base + arch::PAGE_SIZE &&
             child->size == 3 * arch::PAGE_SIZE)
                ? 1
                : 0;
    } else {
        g_rtype_child_ok = 0;
    }
    cs->remove(static_cast<uint32_t>(s));
    if (ret < static_cast<uint64_t>(CONFIG_CSLOT_COUNT))
        cs->remove(static_cast<uint32_t>(ret));
    if (child_idx >= 0)
        cs->remove(static_cast<uint32_t>(child_idx));
    ut->release();
    Scheduler::terminate(*cur, 0);
}

// -- shared results for the SYS_CAP_RETYPE validation task --
// [0]=bad handle, [1]=Endpoint target, [2]=oversize, [3]=size 0,
// [4]=unaligned, [5]=parent intact flag, [6]=exact retype result
uint64_t g_rtype_val[7] = {0, 0, 0, 0, 0, 0, 0};

/// @brief Dispatches SYS_CAP_RETYPE with the given inputs; returns the result.
uint64_t cap_retype_attempt(uint64_t cap_handle, uint64_t target_type,
                            uint64_t size, uint64_t rights) {
    return Syscall::handle(static_cast<uint64_t>(SyscallNumber::CAP_RETYPE),
                           cap_handle, target_type, size, rights, nullptr);
}

/// @brief Dispatches SYS_CAP_RETYPE for a failing input matrix; stores the
///        results in g_rtype_val; final exact-size carve must succeed.
void cap_retype_validation_entry() {
    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();
    cap::CNode *cs = cur->get_cspace();
    const size_t sz = 4 * arch::PAGE_SIZE;
    auto *ut = cap::UntypedMem::create(sz, false);
    if (!ut) {
        g_rtype_val[0] = 99;
        Scheduler::terminate(*cur, 0);
        return;
    }
    int s = cs->install(ut, cap::CapType::Untyped,
                        cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    if (s < 0) {
        ut->release();
        g_rtype_val[0] = 98;
        Scheduler::terminate(*cur, 0);
        return;
    }
    uint64_t h = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                    cs->slot_gen(static_cast<uint32_t>(s)));
    uint32_t free_idx = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT); ++i) {
        if (!cs->slots[i].occupied) {
            free_idx = i;
            break;
        }
    }
    uint64_t bad_h =
        cap::encode_handle(cs->cspace_id, free_idx, 0); // empty slot
    g_rtype_val[0] = cap_retype_attempt(
        bad_h, static_cast<uint64_t>(cap::CapType::Frame),
        static_cast<uint64_t>(arch::PAGE_SIZE),
        static_cast<uint64_t>(cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE));
    g_rtype_val[1] = cap_retype_attempt(
        h, static_cast<uint64_t>(cap::CapType::Endpoint),
        static_cast<uint64_t>(arch::PAGE_SIZE),
        static_cast<uint64_t>(cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE));
    g_rtype_val[2] = cap_retype_attempt(
        h, static_cast<uint64_t>(cap::CapType::Frame),
        static_cast<uint64_t>(6 * arch::PAGE_SIZE),
        static_cast<uint64_t>(cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE));
    g_rtype_val[3] = cap_retype_attempt(
        h, static_cast<uint64_t>(cap::CapType::Frame), 0,
        static_cast<uint64_t>(cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE));
    g_rtype_val[4] = cap_retype_attempt(
        h, static_cast<uint64_t>(cap::CapType::Frame),
        static_cast<uint64_t>(arch::PAGE_SIZE + 0x100),
        static_cast<uint64_t>(cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE));
    g_rtype_val[5] = ut->owns_region() ? 1 : 0; // parent intact after failures
    g_rtype_val[6] = cap_retype_attempt(
        h, static_cast<uint64_t>(cap::CapType::Frame),
        static_cast<uint64_t>(sz),
        static_cast<uint64_t>(cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE));
    cs->remove(static_cast<uint32_t>(s));
    if (g_rtype_val[6] < static_cast<uint64_t>(CONFIG_CSLOT_COUNT))
        cs->remove(static_cast<uint32_t>(g_rtype_val[6]));
    ut->release();
    Scheduler::terminate(*cur, 0);
}

} // namespace

// Runmode: kernel
// Testidea: UntypedMem::create carves a contiguous, page-aligned PMM region.
// Input: create(2 * PAGE_SIZE, true)
// Expect: phys != 0, page-aligned, size correct; PMM +2 pages, cap_objects +1
// Depends: kernel::cap::UntypedMem, kernel::memory::PMM
JARVIS_TEST(untyped_create_allocates_contiguous_region,
            "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    const size_t sz = 2 * arch::PAGE_SIZE;
    auto *ut = cap::UntypedMem::create(sz, true);
    JARVIS_ASSERT(ut != nullptr);
    JARVIS_ASSERT(ut->phys != 0);
    JARVIS_ASSERT((ut->phys & (arch::PAGE_SIZE - 1)) == 0);
    JARVIS_ASSERT_EQ(sz, ut->size);
    JARVIS_ASSERT(ut->is_user);
    JARVIS_ASSERT(ut->owns_region());
    JARVIS_ASSERT(ut->is_pool_backed());

    ut->release(); // creator ref -> dispose frees region + block

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: An Untyped installed in a CNode frees its region on teardown.
// Input: create, install, remove slot, release creator
// Expect: pmm_pages_used / cap_objects / cap_slots back to baseline
// Depends: kernel::cap::UntypedMem, kernel::cap::CNode
JARVIS_TEST(untyped_dispose_returns_region_to_pmm, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    const size_t sz = 3 * arch::PAGE_SIZE;
    auto *ut = cap::UntypedMem::create(sz, false);
    JARVIS_ASSERT(ut != nullptr);

    cap::CNode node;
    int idx = node.install(ut, cap::CapType::Untyped, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(idx >= 0);
    JARVIS_ASSERT_EQ(1U, cap::occupied_count(&node));
    node.remove(static_cast<uint32_t>(idx));
    JARVIS_ASSERT_EQ(1U, ut->refcount()); // creator ref only
    ut->release();                         // creator -> dispose

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: cap::copy duplicates an Untyped cap; both slots share the object
//           and the region stays owned (retype-ability intact).
// Input: install in src, copy to dst
// Expect: both slots type Untyped; refcount 3; owns_region() still true
// Depends: kernel::cap::UntypedMem, kernel::cap::CNode, kernel::cap::copy
JARVIS_TEST(untyped_cap_copy_shares_object, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    const size_t sz = arch::PAGE_SIZE;
    auto *ut = cap::UntypedMem::create(sz, false);
    JARVIS_ASSERT(ut != nullptr);

    cap::CNode src;
    cap::CNode dst;
    int s = src.install(ut, cap::CapType::Untyped,
                        cap::CAP_RIGHT_READ | cap::CAP_RIGHT_COPY);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(src.cspace_id, static_cast<uint32_t>(s),
                                     src.slot_gen(static_cast<uint32_t>(s)));
    int d = cap::copy(&src, sh, &dst);
    JARVIS_ASSERT(d >= 0);
    JARVIS_ASSERT_EQ(3U, ut->refcount()); // creator + src + dst
    JARVIS_ASSERT(ut->owns_region());     // not retyped yet

    dst.remove(static_cast<uint32_t>(d));
    src.remove(static_cast<uint32_t>(s));
    ut->release();

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: retype transfers an exact-size region to a FrameCap end-to-end.
// Input: create 4-page Untyped, install, retype(Frame, exact size)
// Expect: new Frame slot; FrameCap.phys == ut->phys, count == 4;
//         ut->owns_region() false; teardown frees frames exactly once
// Depends: kernel::cap::UntypedMem, kernel::cap::retype, kernel::cap::FrameCap
JARVIS_TEST(retype_frame_exact_size_end_to_end, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    const size_t sz = 4 * arch::PAGE_SIZE;
    auto *ut = cap::UntypedMem::create(sz, true);
    JARVIS_ASSERT(ut != nullptr);

    cap::CNode node;
    int s = node.install(ut, cap::CapType::Untyped,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(node.cspace_id, static_cast<uint32_t>(s),
                                     node.slot_gen(static_cast<uint32_t>(s)));

    int r = cap::retype(&node, sh, cap::CapType::Frame, sz,
                        cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(r >= 0);
    // Exact-size carve installs ONE slot (no child Untyped).
    JARVIS_ASSERT_EQ(2U, cap::occupied_count(&node)); // ut slot + frame slot

    // The new slot is a FrameCap wrapping the exact same region.
    KernelObject *target =
        cap::lookup(&node, cap::encode_handle(node.cspace_id,
                                              static_cast<uint32_t>(r),
                                              node.slot_gen(static_cast<uint32_t>(r))),
                    cap::CapType::Frame, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(target != nullptr);
    auto *fc = static_cast<cap::FrameCap *>(target);
    JARVIS_ASSERT_EQ(ut->phys, fc->phys);
    JARVIS_ASSERT_EQ((size_t)4, fc->count);
    target->release();
    JARVIS_ASSERT(!ut->owns_region()); // ownership transferred

    // Teardown: Untyped slot dispose frees NO frames (guard set); FrameCap
    // slot dispose frees the region exactly once.
    node.remove(static_cast<uint32_t>(s));
    node.remove(static_cast<uint32_t>(r));
    ut->release(); // Untyped dispose: no PMM free (guard set)

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: retype with an oversize (size > ut->size) fails and leaves the
//           Untyped intact.  (Sub-range carves smaller than the region are
//           legal since issue #1 — see the carve tests.)
// Input: 4-page Untyped, retype with 6-page size
// Expect: -1; owns_region() still true; exact-size retype succeeds after
// Depends: kernel::cap::UntypedMem, kernel::cap::retype
JARVIS_TEST(retype_oversize_rejected_parent_intact, "PRE: none | POST: none") {
    const size_t sz = 4 * arch::PAGE_SIZE;
    auto *ut = cap::UntypedMem::create(sz, false);
    JARVIS_ASSERT(ut != nullptr);

    cap::CNode node;
    int s = node.install(ut, cap::CapType::Untyped,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(node.cspace_id, static_cast<uint32_t>(s),
                                     node.slot_gen(static_cast<uint32_t>(s)));

    JARVIS_ASSERT_EQ(-1, cap::retype(&node, sh, cap::CapType::Frame,
                                     6 * arch::PAGE_SIZE, cap::CAP_RIGHT_READ));
    JARVIS_ASSERT(ut->owns_region()); // untouched

    int r = cap::retype(&node, sh, cap::CapType::Frame, sz, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(r >= 0);

    node.remove(static_cast<uint32_t>(s));
    node.remove(static_cast<uint32_t>(r));
    ut->release();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A second retype of the same Untyped fails (single-winner guard).
// Input: retype succeeds; retype again with the same handle
// Expect: second returns -1; exactly one FrameCap ever owns the region
// Depends: kernel::cap::UntypedMem, kernel::cap::retype
JARVIS_TEST(retype_twice_fails_no_double_free, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    const size_t sz = arch::PAGE_SIZE;
    auto *ut = cap::UntypedMem::create(sz, false);
    JARVIS_ASSERT(ut != nullptr);

    cap::CNode node;
    int s = node.install(ut, cap::CapType::Untyped,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(node.cspace_id, static_cast<uint32_t>(s),
                                     node.slot_gen(static_cast<uint32_t>(s)));

    int r1 = cap::retype(&node, sh, cap::CapType::Frame, sz, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(r1 >= 0);
    // Second retype on the same handle: the slot still points at the Untyped,
    // but the guard is set -> -1.  No second FrameCap is ever created.
    JARVIS_ASSERT_EQ(-1, cap::retype(&node, sh, cap::CapType::Frame, sz,
                                     cap::CAP_RIGHT_READ));

    node.remove(static_cast<uint32_t>(s));
    node.remove(static_cast<uint32_t>(r1));
    ut->release();

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: retype rejects non-Frame targets; the Untyped stays intact.
// Input: retype(Endpoint) and retype(CNode)
// Expect: -1 both; owns_region() true; exact-size Frame retype works after
// Depends: kernel::cap::UntypedMem, kernel::cap::retype
JARVIS_TEST(retype_wrong_target_type_fails, "PRE: none | POST: none") {
    const size_t sz = arch::PAGE_SIZE;
    auto *ut = cap::UntypedMem::create(sz, false);
    JARVIS_ASSERT(ut != nullptr);

    cap::CNode node;
    int s = node.install(ut, cap::CapType::Untyped,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(node.cspace_id, static_cast<uint32_t>(s),
                                     node.slot_gen(static_cast<uint32_t>(s)));

    JARVIS_ASSERT_EQ(-1, cap::retype(&node, sh, cap::CapType::Endpoint, sz,
                                     cap::CAP_RIGHT_READ));
    JARVIS_ASSERT_EQ(-1, cap::retype(&node, sh, cap::CapType::CNode, sz,
                                     cap::CAP_RIGHT_READ));
    JARVIS_ASSERT(ut->owns_region());

    int r = cap::retype(&node, sh, cap::CapType::Frame, sz, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(r >= 0);
    node.remove(static_cast<uint32_t>(s));
    node.remove(static_cast<uint32_t>(r));
    ut->release();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: retype requires CAP_RIGHT_WRITE on the Untyped cap.
// Input: install with READ|COPY only (no WRITE)
// Expect: -1 (lookup rights check); Untyped intact
// Depends: kernel::cap::UntypedMem, kernel::cap::retype
JARVIS_TEST(retype_requires_write_right, "PRE: none | POST: none") {
    const size_t sz = arch::PAGE_SIZE;
    auto *ut = cap::UntypedMem::create(sz, false);
    JARVIS_ASSERT(ut != nullptr);

    cap::CNode node;
    int s = node.install(ut, cap::CapType::Untyped,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_COPY);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(node.cspace_id, static_cast<uint32_t>(s),
                                     node.slot_gen(static_cast<uint32_t>(s)));

    JARVIS_ASSERT_EQ(-1, cap::retype(&node, sh, cap::CapType::Frame, sz,
                                     cap::CAP_RIGHT_READ));
    JARVIS_ASSERT(ut->owns_region());

    node.remove(static_cast<uint32_t>(s));
    ut->release();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A revoked Untyped refuses retype; the region frees exactly once.
// Input: install in CNode A, revoke the slot; copied cap in CNode B
// Expect: retype via B's handle -> -1 (acquire refused); region freed once
// Depends: kernel::cap::UntypedMem, kernel::cap::retype, kernel::cap::revoke
JARVIS_TEST(retype_revoked_untyped_fails_single_free, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    const size_t sz = arch::PAGE_SIZE;
    auto *ut = cap::UntypedMem::create(sz, false);
    JARVIS_ASSERT(ut != nullptr);

    cap::CNode a;
    cap::CNode b;
    int sa = a.install(ut, cap::CapType::Untyped,
                       cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE |
                           cap::CAP_RIGHT_COPY);
    JARVIS_ASSERT(sa >= 0);
    uint64_t ha = cap::encode_handle(a.cspace_id, static_cast<uint32_t>(sa),
                                     a.slot_gen(static_cast<uint32_t>(sa)));
    int sb = cap::copy(&a, ha, &b);
    JARVIS_ASSERT(sb >= 0);

    // Revoke the slot in A: marks the object revoked, drops A's ref.
    JARVIS_ASSERT(cap::revoke(&a, ha));
    JARVIS_ASSERT(ut->revoked());

    // Retype via B's handle: acquire() refused -> -1.
    uint64_t hb = cap::encode_handle(b.cspace_id, static_cast<uint32_t>(sb),
                                     b.slot_gen(static_cast<uint32_t>(sb)));
    JARVIS_ASSERT_EQ(-1, cap::retype(&b, hb, cap::CapType::Frame, sz,
                                     cap::CAP_RIGHT_READ));

    b.remove(static_cast<uint32_t>(sb));
    ut->release(); // region freed exactly once by Untyped dispose

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A sub-range carve installs the target FrameCap AND a child
//           Untyped for the remainder; the parent is spent.
// Input: 4-page Untyped, retype 1 page
// Expect: 3 occupied slots (ut + frame + child); frame covers [base, base+1p),
//         child covers [base+1p, base+4p) and owns_region() true; parent
//         owns nothing; zero ResourceTracker delta after teardown
// Depends: kernel::cap::UntypedMem, kernel::cap::retype, kernel::cap::FrameCap
JARVIS_TEST(retype_subrange_carve_creates_child_remainder,
            "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    const size_t sz = 4 * arch::PAGE_SIZE;
    auto *ut = cap::UntypedMem::create(sz, true);
    JARVIS_ASSERT(ut != nullptr);

    cap::CNode node;
    int s = node.install(ut, cap::CapType::Untyped,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(node.cspace_id, static_cast<uint32_t>(s),
                                     node.slot_gen(static_cast<uint32_t>(s)));

    const size_t carve = arch::PAGE_SIZE;
    int r = cap::retype(&node, sh, cap::CapType::Frame, carve,
                        cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(r >= 0);
    JARVIS_ASSERT_EQ(3U, cap::occupied_count(&node)); // ut + frame + child
    JARVIS_ASSERT(!ut->owns_region());                // parent spent

    KernelObject *target =
        cap::lookup(&node, cap::encode_handle(node.cspace_id,
                                              static_cast<uint32_t>(r),
                                              node.slot_gen(static_cast<uint32_t>(r))),
                    cap::CapType::Frame, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(target != nullptr);
    auto *fc = static_cast<cap::FrameCap *>(target);
    JARVIS_ASSERT_EQ(ut->phys, fc->phys);
    JARVIS_ASSERT_EQ((size_t)1, fc->count);
    target->release();

    int child_idx = find_slot_of(node, cap::CapType::Untyped, ut);
    JARVIS_ASSERT(child_idx >= 0);
    auto *child = static_cast<cap::UntypedMem *>(
        node.peek(static_cast<uint32_t>(child_idx), cap::CapType::Untyped));
    JARVIS_ASSERT(child != nullptr);
    JARVIS_ASSERT_EQ(ut->phys + carve, child->phys);
    JARVIS_ASSERT_EQ(sz - carve, child->size);
    JARVIS_ASSERT(child->owns_region());

    node.remove(static_cast<uint32_t>(s));
    node.remove(static_cast<uint32_t>(r));
    node.remove(static_cast<uint32_t>(child_idx));
    ut->release();

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The child Untyped of a carve is itself retypable (two-level
//           split), producing a grandchild for the further remainder.
// Input: 4-page Untyped; carve 1p -> child(3p); retype child 1p
// Expect: frame1 [base,1p), frame2 [base+1p,1p), grandchild [base+2p,2p);
//         zero ResourceTracker delta after teardown
// Depends: kernel::cap::UntypedMem, kernel::cap::retype, kernel::cap::FrameCap
JARVIS_TEST(retype_two_level_split_child_retyped_again,
            "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    const size_t sz = 4 * arch::PAGE_SIZE;
    auto *ut = cap::UntypedMem::create(sz, false);
    JARVIS_ASSERT(ut != nullptr);

    cap::CNode node;
    int s = node.install(ut, cap::CapType::Untyped,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(node.cspace_id, static_cast<uint32_t>(s),
                                     node.slot_gen(static_cast<uint32_t>(s)));

    int r1 = cap::retype(&node, sh, cap::CapType::Frame, arch::PAGE_SIZE,
                         cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(r1 >= 0);
    int child1_idx = find_slot_of(node, cap::CapType::Untyped, ut);
    JARVIS_ASSERT(child1_idx >= 0);
    auto *child1 = static_cast<cap::UntypedMem *>(
        node.peek(static_cast<uint32_t>(child1_idx), cap::CapType::Untyped));
    JARVIS_ASSERT(child1 != nullptr);
    JARVIS_ASSERT_EQ(ut->phys + arch::PAGE_SIZE, child1->phys);
    JARVIS_ASSERT_EQ(3 * arch::PAGE_SIZE, child1->size);

    // Retype the child: frame2 + grandchild.
    uint64_t ch = cap::encode_handle(
        node.cspace_id, static_cast<uint32_t>(child1_idx),
        node.slot_gen(static_cast<uint32_t>(child1_idx)));
    int r2 = cap::retype(&node, ch, cap::CapType::Frame, arch::PAGE_SIZE,
                         cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(r2 >= 0);
    JARVIS_ASSERT(!child1->owns_region()); // child spent by its carve
    // find_slot_of returns the FIRST Untyped slot, i.e. the still-occupied
    // child1, not the grandchild: scan excluding both spent parents.
    int gc_idx = -1;
    for (uint32_t i = 0; i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT); ++i) {
        KernelObject *obj = node.peek(i, cap::CapType::Untyped);
        if (obj && obj != ut && obj != child1) {
            gc_idx = static_cast<int>(i);
            break;
        }
    }
    JARVIS_ASSERT(gc_idx >= 0);
    auto *gc = static_cast<cap::UntypedMem *>(
        node.peek(static_cast<uint32_t>(gc_idx), cap::CapType::Untyped));
    JARVIS_ASSERT(gc != nullptr);
    JARVIS_ASSERT_EQ(ut->phys + 2 * arch::PAGE_SIZE, gc->phys);
    JARVIS_ASSERT_EQ(2 * arch::PAGE_SIZE, gc->size);

    KernelObject *target2 = cap::lookup(
        &node, cap::encode_handle(node.cspace_id, static_cast<uint32_t>(r2),
                                  node.slot_gen(static_cast<uint32_t>(r2))),
        cap::CapType::Frame, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(target2 != nullptr);
    auto *fc2 = static_cast<cap::FrameCap *>(target2);
    JARVIS_ASSERT_EQ(ut->phys + arch::PAGE_SIZE, fc2->phys);
    JARVIS_ASSERT_EQ((size_t)1, fc2->count);
    target2->release();

    node.remove(static_cast<uint32_t>(s));
    node.remove(static_cast<uint32_t>(r1));
    node.remove(static_cast<uint32_t>(child1_idx));
    node.remove(static_cast<uint32_t>(r2));
    node.remove(static_cast<uint32_t>(gc_idx));
    ut->release();

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A non-PAGE-aligned carve size is rejected; the parent is intact.
// Input: 4-page Untyped, retype 1 page + 0x100
// Expect: -1; owns_region() true; exact-size retype succeeds after
// Depends: kernel::cap::UntypedMem, kernel::cap::retype
JARVIS_TEST(retype_unaligned_size_rejected_parent_intact,
            "PRE: none | POST: none") {
    const size_t sz = 4 * arch::PAGE_SIZE;
    auto *ut = cap::UntypedMem::create(sz, false);
    JARVIS_ASSERT(ut != nullptr);

    cap::CNode node;
    int s = node.install(ut, cap::CapType::Untyped,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(node.cspace_id, static_cast<uint32_t>(s),
                                     node.slot_gen(static_cast<uint32_t>(s)));

    JARVIS_ASSERT_EQ(-1, cap::retype(&node, sh, cap::CapType::Frame,
                                     arch::PAGE_SIZE + 0x100,
                                     cap::CAP_RIGHT_READ));
    JARVIS_ASSERT(ut->owns_region());

    int r = cap::retype(&node, sh, cap::CapType::Frame, sz, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(r >= 0);
    node.remove(static_cast<uint32_t>(s));
    node.remove(static_cast<uint32_t>(r));
    ut->release();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Disposing the child Untyped frees ONLY the remainder; the carved
//           frame stays owned by the FrameCap.
// Input: 4-page Untyped, carve 1p; remove the child slot
// Expect: pmm_pages_used drops by exactly 3 pages on child dispose
// Depends: kernel::cap::UntypedMem, kernel::cap::retype
JARVIS_TEST(retype_child_dispose_frees_only_remainder,
            "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    const size_t sz = 4 * arch::PAGE_SIZE;
    auto *ut = cap::UntypedMem::create(sz, false);
    JARVIS_ASSERT(ut != nullptr);

    cap::CNode node;
    int s = node.install(ut, cap::CapType::Untyped,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(node.cspace_id, static_cast<uint32_t>(s),
                                     node.slot_gen(static_cast<uint32_t>(s)));
    int r = cap::retype(&node, sh, cap::CapType::Frame, arch::PAGE_SIZE,
                        cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(r >= 0);
    int child_idx = find_slot_of(node, cap::CapType::Untyped, ut);
    JARVIS_ASSERT(child_idx >= 0);

    kernel::test::ResourceCounters mid{};
    rt.capture(mid);

    node.remove(static_cast<uint32_t>(child_idx)); // child frees 3 pages
    kernel::test::ResourceCounters after_child{};
    rt.capture(after_child);
    JARVIS_ASSERT_EQ(mid.pmm_pages_used - 3, after_child.pmm_pages_used);

    node.remove(static_cast<uint32_t>(s));
    node.remove(static_cast<uint32_t>(r));
    ut->release();

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Disposing the spent parent Untyped after a carve frees nothing.
// Input: 4-page Untyped, carve 1p; remove the ut slot + release creator
// Expect: pmm_pages_used unchanged (frame 1p + child 3p still owned)
// Depends: kernel::cap::UntypedMem, kernel::cap::retype
JARVIS_TEST(retype_parent_dispose_after_carve_frees_nothing,
            "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    const size_t sz = 4 * arch::PAGE_SIZE;
    auto *ut = cap::UntypedMem::create(sz, false);
    JARVIS_ASSERT(ut != nullptr);

    cap::CNode node;
    int s = node.install(ut, cap::CapType::Untyped,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(node.cspace_id, static_cast<uint32_t>(s),
                                     node.slot_gen(static_cast<uint32_t>(s)));
    int r = cap::retype(&node, sh, cap::CapType::Frame, arch::PAGE_SIZE,
                        cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(r >= 0);
    int child_idx = find_slot_of(node, cap::CapType::Untyped, ut);
    JARVIS_ASSERT(child_idx >= 0);

    node.remove(static_cast<uint32_t>(s));
    ut->release(); // parent dispose: owns nothing -> NO PMM free

    kernel::test::ResourceCounters after_parent{};
    rt.capture(after_parent);
    JARVIS_ASSERT_EQ(before.pmm_pages_used + 4, after_parent.pmm_pages_used);

    node.remove(static_cast<uint32_t>(r));
    node.remove(static_cast<uint32_t>(child_idx));

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The non-destructive slot-capacity pre-check rejects a carve that
//           would exceed the CNode table, leaving the Untyped intact.
// Input: fill the CNode to 63/64 slots; sub-range carve (needs 2)
// Expect: -1; owns_region() true; after freeing fillers the retype succeeds
// Depends: kernel::cap::UntypedMem, kernel::cap::retype, kernel::cap::CNode
JARVIS_TEST(retype_full_table_precheck_fails_closed,
            "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    const size_t sz = 4 * arch::PAGE_SIZE;
    auto *ut = cap::UntypedMem::create(sz, false);
    JARVIS_ASSERT(ut != nullptr);

    cap::CNode node;
    int s = node.install(ut, cap::CapType::Untyped,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(node.cspace_id, static_cast<uint32_t>(s),
                                     node.slot_gen(static_cast<uint32_t>(s)));
    int fill[62];
    for (int i = 0; i < 62; ++i) {
        fill[i] = node.install(ut, cap::CapType::Untyped, cap::CAP_RIGHT_READ);
        JARVIS_ASSERT(fill[i] >= 0);
    }
    JARVIS_ASSERT_EQ(63U, cap::occupied_count(&node));

    // Sub-range carve needs 2 slots; only 1 free -> rejected, parent intact.
    JARVIS_ASSERT_EQ(-1, cap::retype(&node, sh, cap::CapType::Frame,
                                     arch::PAGE_SIZE, cap::CAP_RIGHT_READ));
    JARVIS_ASSERT(ut->owns_region());

    for (int i = 0; i < 62; ++i)
        node.remove(static_cast<uint32_t>(fill[i]));
    node.remove(static_cast<uint32_t>(s));

    // With space available the exact retype succeeds.
    int s2 = node.install(ut, cap::CapType::Untyped,
                          cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s2 >= 0);
    uint64_t sh2 = cap::encode_handle(node.cspace_id,
                                      static_cast<uint32_t>(s2),
                                      node.slot_gen(static_cast<uint32_t>(s2)));
    int r = cap::retype(&node, sh2, cap::CapType::Frame, sz,
                        cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(r >= 0);
    node.remove(static_cast<uint32_t>(s2));
    node.remove(static_cast<uint32_t>(r));
    ut->release();

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_CAP_RETYPE dispatches a sub-range carve end-to-end in a real
//           task; the frame + child slots land in the caller's CSpace.
// Input: real task installs 4-page Untyped; Syscall::handle(CAP_RETYPE, ...)
// Expect: return >= 0; frame slot (count 1, phys == base); child slot
//         [base+1p, 3p); zero ResourceTracker delta after task teardown
// Depends: kernel::cap::UntypedMem, kernel::syscall::Syscall, kernel::task
JARVIS_TEST(sys_cap_retype_dispatch_end_to_end, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    g_rtype_ret = 0;
    g_rtype_base = 0;
    g_rtype_frame_ok = 0;
    g_rtype_child_ok = 0;
    auto *t = run_cap_task(cap_retype_happy_entry);
    JARVIS_ASSERT(t != nullptr);

    JARVIS_ASSERT(g_rtype_ret != 99 && g_rtype_ret != 98);
    JARVIS_ASSERT(g_rtype_ret < static_cast<uint64_t>(CONFIG_CSLOT_COUNT));
    JARVIS_ASSERT_EQ(1U, g_rtype_frame_ok);
    JARVIS_ASSERT_EQ(1U, g_rtype_child_ok);

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_ASSERT_EQ(before.tasks, after.tasks);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_CAP_RETYPE rejects bad handle / wrong target / oversize /
//           zero / unaligned sizes without consuming the Untyped.
// Input: real task; validation matrix; then an exact-size carve
// Expect: every failing input -> -1; parent intact; exact carve succeeds
// Depends: kernel::cap::UntypedMem, kernel::syscall::Syscall, kernel::task
JARVIS_TEST(sys_cap_retype_validation_matrix, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    for (size_t i = 0; i < 7; ++i)
        g_rtype_val[i] = 0;
    auto *t = run_cap_task(cap_retype_validation_entry);
    JARVIS_ASSERT(t != nullptr);

    const uint64_t syscall_fail = static_cast<uint64_t>(-1);
    JARVIS_ASSERT(g_rtype_val[0] != 99 && g_rtype_val[0] != 98);
    JARVIS_ASSERT_EQ(syscall_fail, g_rtype_val[0]); // bad handle
    JARVIS_ASSERT_EQ(syscall_fail, g_rtype_val[1]); // Endpoint target
    JARVIS_ASSERT_EQ(syscall_fail, g_rtype_val[2]); // oversize
    JARVIS_ASSERT_EQ(syscall_fail, g_rtype_val[3]); // size 0
    JARVIS_ASSERT_EQ(syscall_fail, g_rtype_val[4]); // unaligned
    JARVIS_ASSERT_EQ(1U, g_rtype_val[5]);           // parent intact
    JARVIS_ASSERT(g_rtype_val[6] <
                  static_cast<uint64_t>(CONFIG_CSLOT_COUNT)); // exact works

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_ASSERT_EQ(before.tasks, after.tasks);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The shared CONFIG_CAP_MAX_UNTYPED live bound counts child
//           Untypeds: a carve that would create a child beyond the bound fails
//           closed (stretch) and returns the whole region to PMM once.
// Input: exhaust the bound; carve a child from a 4-page Untyped
// Expect: -1; parent spent; only the remaining untypeds' pages stay in use
// Depends: kernel::cap::UntypedMem, kernel::cap::retype
JARVIS_TEST(untyped_live_bound_counts_children, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    constexpr uint32_t kMax = CONFIG_CAP_MAX_UNTYPED;
    cap::UntypedMem *uts[kMax] = {};
    uint32_t n = 0;
    for (uint32_t i = 0; i < kMax; ++i) {
        const size_t pages = (i + 1 == kMax) ? 4 : 1; // last one multi-page
        cap::UntypedMem *u =
            cap::UntypedMem::create(pages * arch::PAGE_SIZE, false);
        if (!u)
            break;
        uts[n++] = u;
    }
    JARVIS_ASSERT_EQ(kMax, n); // budget fully consumed

    // Carve 1 page from the last (4-page) Untyped: the child creation trips
    // the bound inside create_subrange -> stretch fail-closed.
    auto *ut = uts[kMax - 1];
    cap::CNode node;
    int s = node.install(ut, cap::CapType::Untyped,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(node.cspace_id, static_cast<uint32_t>(s),
                                     node.slot_gen(static_cast<uint32_t>(s)));
    JARVIS_ASSERT_EQ(-1, cap::retype(&node, sh, cap::CapType::Frame,
                                     arch::PAGE_SIZE, cap::CAP_RIGHT_READ));
    JARVIS_ASSERT(!ut->owns_region()); // spent (guard stays set)

    node.remove(static_cast<uint32_t>(s));
    ut->release(); // dispose: owns nothing, block freed

    kernel::test::ResourceCounters after_fail{};
    rt.capture(after_fail);
    // The 4 pages of `ut` were returned by the stretch; only the remaining
    // (kMax - 1) single-page untypeds are still in use.
    JARVIS_ASSERT_EQ(before.pmm_pages_used + (kMax - 1),
                     after_fail.pmm_pages_used);

    for (uint32_t i = 0; i < n - 1; ++i)
        uts[i]->release(); // each frees its 1 page

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all Untyped memory allocator tests.
// Input: None
// Expect: All tests registered
// Depends: test framework
void register_cap_untyped_tests() {
    Logger::info("Registering Untyped memory allocator tests");
    JARVIS_REGISTER_TEST(untyped_create_allocates_contiguous_region);
    JARVIS_REGISTER_TEST(untyped_dispose_returns_region_to_pmm);
    JARVIS_REGISTER_TEST(untyped_cap_copy_shares_object);
    JARVIS_REGISTER_TEST(retype_frame_exact_size_end_to_end);
    JARVIS_REGISTER_TEST(retype_oversize_rejected_parent_intact);
    JARVIS_REGISTER_TEST(retype_twice_fails_no_double_free);
    JARVIS_REGISTER_TEST(retype_wrong_target_type_fails);
    JARVIS_REGISTER_TEST(retype_requires_write_right);
    JARVIS_REGISTER_TEST(retype_revoked_untyped_fails_single_free);
    JARVIS_REGISTER_TEST(retype_subrange_carve_creates_child_remainder);
    JARVIS_REGISTER_TEST(retype_two_level_split_child_retyped_again);
    JARVIS_REGISTER_TEST(retype_unaligned_size_rejected_parent_intact);
    JARVIS_REGISTER_TEST(retype_child_dispose_frees_only_remainder);
    JARVIS_REGISTER_TEST(retype_parent_dispose_after_carve_frees_nothing);
    JARVIS_REGISTER_TEST(retype_full_table_precheck_fails_closed);
    JARVIS_REGISTER_TEST(sys_cap_retype_dispatch_end_to_end);
    JARVIS_REGISTER_TEST(sys_cap_retype_validation_matrix);
    JARVIS_REGISTER_TEST(untyped_live_bound_counts_children);
}
