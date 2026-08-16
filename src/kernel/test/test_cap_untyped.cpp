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
#include <kernel/arch/page_table.hpp>

using namespace kernel;

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
// Testidea: retype with a wrong size fails and leaves the Untyped intact.
// Input: 4-page Untyped, retype with 2-page size
// Expect: -1; owns_region() still true; exact-size retype succeeds after
// Depends: kernel::cap::UntypedMem, kernel::cap::retype
JARVIS_TEST(retype_wrong_size_fails_keeps_untyped, "PRE: none | POST: none") {
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
                                     2 * arch::PAGE_SIZE, cap::CAP_RIGHT_READ));
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
    JARVIS_REGISTER_TEST(retype_wrong_size_fails_keeps_untyped);
    JARVIS_REGISTER_TEST(retype_twice_fails_no_double_free);
    JARVIS_REGISTER_TEST(retype_wrong_target_type_fails);
    JARVIS_REGISTER_TEST(retype_requires_write_right);
    JARVIS_REGISTER_TEST(retype_revoked_untyped_fails_single_free);
}
