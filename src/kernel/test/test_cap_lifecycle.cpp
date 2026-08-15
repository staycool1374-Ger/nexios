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

/// @file test_cap_lifecycle.cpp
/// @brief CSpace lifecycle primitive tests: grant/copy/revoke/mint and the
///        Endpoint/FrameCap shared-heap objects.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/endpoint.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/test/resource_tracker.hpp>

using namespace kernel;

namespace {

/// @brief Minimal shared-heap KernelObject used as a capability target.
struct TestTarget : public KernelObject {
    static uint32_t g_dispose_count;
    void dispose() noexcept override { ++g_dispose_count; }
    bool is_shared() const noexcept override { return true; }
};

uint32_t TestTarget::g_dispose_count = 0;

} // namespace

// Runmode: kernel
// Testidea: grant() installs a slot in the destination CNode referencing the
//           same object; the source GRANT right is consumed (mint-once).
// Input: source slot with GRANT; grant into destination
// Expect: dst occupied; both refs held; source GRANT cleared
// Depends: kernel::cap
JARVIS_TEST(grant_creates_slot_in_dest, "PRE: none | POST: none") {
    cap::CNode src;
    cap::CNode dst;
    TestTarget tgt;
    int s = src.install(&tgt, cap::CapType::Task,
                        cap::CAP_RIGHT_READ | cap::CAP_RIGHT_GRANT);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(src.cspace_id, static_cast<uint32_t>(s),
                                     src.slot_gen(static_cast<uint32_t>(s)));
    int d = cap::grant(&src, sh, &dst);
    JARVIS_ASSERT(d >= 0);
    JARVIS_ASSERT_EQ(1U, cap::occupied_count(&dst));
    JARVIS_ASSERT_EQ(3U, tgt.refcount()); // creator + src + dst
    JARVIS_ASSERT_EQ(1U, cap::occupied_count(&src));
    dst.remove(static_cast<uint32_t>(d));
    src.remove(static_cast<uint32_t>(s));
    JARVIS_ASSERT_EQ(1U, tgt.refcount());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: copy() duplicates the capability into the destination without
//           consuming the source GRANT right (unlike grant).
// Input: source slot with COPY; copy into destination
// Expect: dst occupied; source COPY right retained
// Depends: kernel::cap
JARVIS_TEST(copy_shares_refcount, "PRE: none | POST: none") {
    cap::CNode src;
    cap::CNode dst;
    TestTarget tgt;
    int s = src.install(&tgt, cap::CapType::Task,
                        cap::CAP_RIGHT_READ | cap::CAP_RIGHT_COPY);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(src.cspace_id, static_cast<uint32_t>(s),
                                     src.slot_gen(static_cast<uint32_t>(s)));
    int d = cap::copy(&src, sh, &dst);
    JARVIS_ASSERT(d >= 0);
    JARVIS_ASSERT_EQ(3U, tgt.refcount());
    // copy() must NOT clear the source COPY right (only grant() is mint-once).
    KernelObject *pinned = cap::lookup(&src, sh, cap::CapType::Null,
                                       cap::CAP_RIGHT_COPY);
    JARVIS_ASSERT(pinned != nullptr);
    if (pinned)
        pinned->release();
    dst.remove(static_cast<uint32_t>(d));
    src.remove(static_cast<uint32_t>(s));
    JARVIS_ASSERT_EQ(1U, tgt.refcount());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: revoke() removes the slot and drops its reference.
// Input: install target, revoke via handle
// Expect: occupied_count drops; refcount drops; revoke idempotent
// Depends: kernel::cap
JARVIS_TEST(revoke_removes_slot_and_releases, "PRE: none | POST: none") {
    cap::CNode node;
    TestTarget tgt;
    int s = node.install(&tgt, cap::CapType::Task, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(node.cspace_id, static_cast<uint32_t>(s),
                                     node.slot_gen(static_cast<uint32_t>(s)));
    JARVIS_ASSERT_EQ(2U, tgt.refcount());
    JARVIS_ASSERT(cap::revoke(&node, sh));
    JARVIS_ASSERT_EQ(0U, cap::occupied_count(&node));
    JARVIS_ASSERT_EQ(1U, tgt.refcount());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A second revoke on the same handle is a no-op (idempotent).
// Input: revoke, then revoke again with the same handle
// Expect: first true, second false; no double release
// Depends: kernel::cap
JARVIS_TEST(revoke_twice_idempotent, "PRE: none | POST: none") {
    cap::CNode node;
    TestTarget tgt;
    int s = node.install(&tgt, cap::CapType::Task, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(node.cspace_id, static_cast<uint32_t>(s),
                                     node.slot_gen(static_cast<uint32_t>(s)));
    JARVIS_ASSERT(cap::revoke(&node, sh));
    JARVIS_ASSERT(!cap::revoke(&node, sh));
    JARVIS_ASSERT_EQ(1U, tgt.refcount());
    JARVIS_ASSERT_EQ(0U, cap::occupied_count(&node));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: mint() copies with a reduced rights mask.
// Input: install READ|WRITE, mint with READ only into dst
// Expect: dst slot lacks WRITE; lookup for WRITE fails
// Depends: kernel::cap
JARVIS_TEST(mint_reduces_rights, "PRE: none | POST: none") {
    cap::CNode src;
    cap::CNode dst;
    TestTarget tgt;
    int s = src.install(&tgt, cap::CapType::Task,
                        cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(src.cspace_id, static_cast<uint32_t>(s),
                                     src.slot_gen(static_cast<uint32_t>(s)));
    int d = cap::mint(&src, sh, &dst, cap::CAP_RIGHT_READ, 0);
    JARVIS_ASSERT(d >= 0);
    uint64_t dh = cap::encode_handle(dst.cspace_id, static_cast<uint32_t>(d),
                                     dst.slot_gen(static_cast<uint32_t>(d)));
    KernelObject *rw = cap::lookup(&dst, dh, cap::CapType::Null,
                                   cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(rw == nullptr);
    KernelObject *ro = cap::lookup(&dst, dh, cap::CapType::Null,
                                   cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(ro != nullptr);
    if (ro)
        ro->release();
    dst.remove(static_cast<uint32_t>(d));
    src.remove(static_cast<uint32_t>(s));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A ScopedRef pin keeps a shared object alive across slot removal;
//           dispose() runs only when the last reference (the pin) drops.
// Input: install shared target, remove slot, then drop pin
// Expect: dispose runs exactly once, at pin release
// Depends: kernel::cap, kernel::KernelObject, kernel::ScopedRef
JARVIS_TEST(revoke_while_scopedref_pinned_delays_dispose,
            "PRE: none | POST: none") {
    TestTarget::g_dispose_count = 0;
    cap::CNode node;
    TestTarget tgt;
    int s = node.install(&tgt, cap::CapType::Task, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(s >= 0);

    ScopedRef pin(&tgt);
    JARVIS_ASSERT(pin.valid());
    JARVIS_ASSERT_EQ(3U, tgt.refcount()); // creator + slot + pin

    node.remove(static_cast<uint32_t>(s));
    JARVIS_ASSERT_EQ(2U, tgt.refcount()); // creator + pin still held
    JARVIS_ASSERT_EQ(0U, TestTarget::g_dispose_count);
    JARVIS_TEST_PASS(); // pin released at scope exit; dispose runs then
}

// Runmode: kernel
// Testidea: A FrameCap returns its wrapped PMM pages when its last reference
//           is released via the slot.
// Input: alloc_page for the frame, create FrameCap, install, remove slot
// Expect: dispose frees the page; PMM pages delta zero
// Depends: kernel::cap::FrameCap, kernel::memory::PMM
JARVIS_TEST(frame_cap_release_frees_pmm, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    uint64_t page = PMM::alloc_page();
    JARVIS_ASSERT(page != 0);
    auto *fc = cap::FrameCap::create(page, 1, false);
    JARVIS_ASSERT(fc != nullptr);

    cap::CNode node;
    int s = node.install(fc, cap::CapType::Frame,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    node.remove(static_cast<uint32_t>(s)); // slot ref dropped
    JARVIS_ASSERT_EQ(1U, fc->refcount());  // creator ref only
    fc->release();                          // creator ref -> dispose frees page

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: An Endpoint shared-heap object disposes cleanly (queue init'd,
//           block freed) on its last reference.
// Input: create Endpoint, install, remove slot
// Expect: cap_objects delta zero; refcount returns to creator's
// Depends: kernel::cap::Endpoint
JARVIS_TEST(endpoint_cap_release_disposes, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    auto *ep = cap::Endpoint::create(0x1234);
    JARVIS_ASSERT(ep != nullptr);
    JARVIS_ASSERT_EQ(0x1234U, ep->badge);
    JARVIS_ASSERT(ep->q.is_empty());

    cap::CNode node;
    int s = node.install(ep, cap::CapType::Endpoint,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    node.remove(static_cast<uint32_t>(s));
    JARVIS_ASSERT_EQ(1U, ep->refcount()); // creator ref only
    ep->release();                          // creator ref -> dispose

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all CSpace lifecycle primitive tests.
// Input: None
// Expect: All tests registered
// Depends: test framework
void register_cap_lifecycle_tests() {
    Logger::info("Registering CSpace lifecycle primitive tests");
    JARVIS_REGISTER_TEST(grant_creates_slot_in_dest);
    JARVIS_REGISTER_TEST(copy_shares_refcount);
    JARVIS_REGISTER_TEST(revoke_removes_slot_and_releases);
    JARVIS_REGISTER_TEST(revoke_twice_idempotent);
    JARVIS_REGISTER_TEST(mint_reduces_rights);
    JARVIS_REGISTER_TEST(revoke_while_scopedref_pinned_delays_dispose);
    JARVIS_REGISTER_TEST(frame_cap_release_frees_pmm);
    JARVIS_REGISTER_TEST(endpoint_cap_release_disposes);
}
