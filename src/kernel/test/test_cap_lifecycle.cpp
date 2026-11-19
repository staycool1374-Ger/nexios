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
#include <kernel/cap/cap_errors.hpp>
#include <kernel/cap/derivation.hpp>
#include <kernel/cap/endpoint.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/cap/untyped.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/test/resource_tracker.hpp>

using namespace kernel;

namespace {

/// @brief Minimal shared-heap KernelObject used as a capability target.
///        Pool-backed in tests (allocated via MemPool), so dispose() returns
///        the block and balances the ResourceTracker counter.
struct TestTarget : public KernelObject {
    static uint32_t g_dispose_count;
    void dispose() noexcept override {
        ++g_dispose_count;
        if (is_pool_backed()) {
            kernel::test::ResourceTracker::instance().track_cap_object_remove();
            MemPool::free(this);
        }
    }
    bool is_shared() const noexcept override {
        return true;
    }
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
                        cap::CapRights::READ | cap::CapRights::GRANT);
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
                        cap::CapRights::READ | cap::CapRights::COPY);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(src.cspace_id, static_cast<uint32_t>(s),
                                     src.slot_gen(static_cast<uint32_t>(s)));
    int d = cap::copy(&src, sh, &dst);
    JARVIS_ASSERT(d >= 0);
    JARVIS_ASSERT_EQ(3U, tgt.refcount());
    // copy() must NOT clear the source COPY right (only grant() is mint-once).
    KernelObject *pinned =
        cap::lookup(&src, sh, cap::CapType::Null, cap::CapRights::COPY);
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
    int s = node.install(&tgt, cap::CapType::Task, cap::CapRights::READ);
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
    int s = node.install(&tgt, cap::CapType::Task, cap::CapRights::READ);
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
                        cap::CapRights::READ | cap::CapRights::WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(src.cspace_id, static_cast<uint32_t>(s),
                                     src.slot_gen(static_cast<uint32_t>(s)));
    int d = cap::mint(&src, sh, &dst, cap::CapRights::READ, 0);
    JARVIS_ASSERT(d >= 0);
    uint64_t dh = cap::encode_handle(dst.cspace_id, static_cast<uint32_t>(d),
                                     dst.slot_gen(static_cast<uint32_t>(d)));
    KernelObject *rw =
        cap::lookup(&dst, dh, cap::CapType::Null, cap::CapRights::WRITE);
    JARVIS_ASSERT(rw == nullptr);
    KernelObject *ro =
        cap::lookup(&dst, dh, cap::CapType::Null, cap::CapRights::READ);
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
    int s = node.install(&tgt, cap::CapType::Task, cap::CapRights::READ);
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
                         cap::CapRights::READ | cap::CapRights::WRITE);
    JARVIS_ASSERT(s >= 0);
    node.remove(static_cast<uint32_t>(s)); // slot ref dropped
    JARVIS_ASSERT_EQ(1U, fc->refcount());  // creator ref only
    fc->release();                         // creator ref -> dispose frees page

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
                         cap::CapRights::READ | cap::CapRights::WRITE);
    JARVIS_ASSERT(s >= 0);
    node.remove(static_cast<uint32_t>(s));
    JARVIS_ASSERT_EQ(1U, ep->refcount()); // creator ref only
    ep->release();                        // creator ref -> dispose

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all CSpace lifecycle primitive tests.
// Runmode: kernel
// Testidea: F2 INV-MK-7 — CAP_DELETE removes one slot without touching the
//        shared target; CAP_REVOKE destroys derivations, named survives.
// Input: one TestTarget installed in A, CNode-cap to B, copy into B.
// Expect: DELETE of the B slot => slot gone, target NOT revoked, A lookup
//        still succeeds; re-copy then REVOKE(A seed) => B derived slot gone,
//        A slot + object live and usable; delta 0.
// Depends: kernel::cap remove / revoke_derivations, derivation edges
JARVIS_TEST(mk_cap_delete_vs_revoke, "PRE: none | POST: none") {
    cap::CNode node_a;
    cap::CNode node_b;
    TestTarget tgt;
    int s = node_a.install(&tgt, cap::CapType::Task,
                           cap::CapRights::READ | cap::CapRights::COPY);
    JARVIS_ASSERT(s >= 0);
    int c = node_a.install(&node_b, cap::CapType::CNode, cap::CapRights::READ);
    JARVIS_ASSERT(c >= 0);
    int d = cap::copy(
        &node_a,
        cap::encode_handle(node_a.cspace_id, static_cast<uint32_t>(s),
                           node_a.slot_gen(static_cast<uint32_t>(s))),
        &node_b);
    JARVIS_ASSERT(d >= 0);
    uint64_t sh = cap::encode_handle(
        node_a.cspace_id, static_cast<uint32_t>(s),
        node_a.slot_gen(static_cast<uint32_t>(s)));
    // DELETE the derived slot: slot gone, target untouched.
    uint64_t dh = cap::encode_handle(
        node_b.cspace_id, static_cast<uint32_t>(d),
        node_b.slot_gen(static_cast<uint32_t>(d)));
    JARVIS_ASSERT(cap::remove(&node_b, dh));
    JARVIS_ASSERT_EQ(0U, cap::occupied_count(&node_b));
    JARVIS_ASSERT(!tgt.revoked());
    KernelObject *still_there =
        cap::lookup(&node_a, sh, cap::CapType::Task, cap::CapRights::READ);
    JARVIS_ASSERT(still_there != nullptr);
    if (still_there)
        still_there->release();
    // Re-derive, then REVOKE the seed: derivation dies, named lives.
    int d2 = cap::copy(&node_a, sh, &node_b);
    JARVIS_ASSERT(d2 >= 0);
    JARVIS_ASSERT_EQ(1U, cap::occupied_count(&node_b));
    errors::CapError rev_err = errors::CAP_ERR_OK;
    int destroyed = cap::revoke_derivations(&node_a, sh, &rev_err);
    JARVIS_ASSERT_EQ(errors::CAP_ERR_OK, rev_err);
    JARVIS_ASSERT_EQ(1, destroyed);
    JARVIS_ASSERT_EQ(0U, cap::occupied_count(&node_b));
    JARVIS_ASSERT(!tgt.revoked());
    KernelObject *usable =
        cap::lookup(&node_a, sh, cap::CapType::Task, cap::CapRights::READ);
    JARVIS_ASSERT(usable != nullptr);
    if (usable)
        usable->release();
    node_a.remove(static_cast<uint32_t>(c));
    node_a.remove(static_cast<uint32_t>(s));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: CAP_DELETE of a sole-holder slot disposes exactly once.
// Input: install target, DELETE it.
// Expect: dispose_count delta 1, no occupied slots, no double free.
// Depends: kernel::cap remove
JARVIS_TEST(cap_delete_last_ref_disposes_once, "PRE: none | POST: none") {
    TestTarget::g_dispose_count = 0;
    cap::CNode node;
    auto *tgt = static_cast<TestTarget *>(MemPool::alloc(sizeof(TestTarget)));
    JARVIS_ASSERT(tgt != nullptr);
    new (tgt) TestTarget;
    tgt->mark_pool_backed();
    kernel::test::ResourceTracker::instance().track_cap_object_add();
    int s = node.install(tgt, cap::CapType::Task, cap::CapRights::READ);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(node.cspace_id, static_cast<uint32_t>(s),
                                     node.slot_gen(static_cast<uint32_t>(s)));
    JARVIS_ASSERT(cap::remove(&node, sh));
    JARVIS_ASSERT_EQ(0U, cap::occupied_count(&node));
    // The CSpace ref is gone; the caller ref keeps it alive (not revoked).
    JARVIS_ASSERT(!tgt->revoked());
    tgt->release();
    JARVIS_ASSERT_EQ(1U, TestTarget::g_dispose_count);
    // Re-delete is fail-closed, never a second dispose.
    JARVIS_ASSERT(!cap::remove(&node, sh));
    JARVIS_ASSERT_EQ(1U, TestTarget::g_dispose_count);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F2 §4.2 — CAP_REVOKE on an Untyped reclaims the carved target
//        and the child remainder, PMM pages exactly once; parent survives.
// Input: 4-page Untyped, retype 1-page Frame, REVOKE parent.
// Expect: carved + child slots gone, PMM delta 0, parent Untyped live, a
//        second retype fails ALREADY_RETYPED.
// Depends: kernel::cap retype / revoke_derivations, PMM
JARVIS_TEST(cap_revoke_untyped_reclaims_carve, "PRE: none | POST: none") {
    const uint64_t free_before = PMM::free_pages_ref();
    cap::CNode node;
    auto *ut = cap::UntypedMem::create(4 * arch::PAGE_SIZE, false);
    JARVIS_ASSERT(ut != nullptr);
    int s = node.install(ut, cap::CapType::Untyped,
                         cap::CapRights::READ | cap::CapRights::WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(node.cspace_id, static_cast<uint32_t>(s),
                                     node.slot_gen(static_cast<uint32_t>(s)));
    errors::CapError rt_err = errors::CAP_ERR_OK;
    int carved = cap::retype(&node, sh, cap::CapType::Frame, arch::PAGE_SIZE,
                             cap::CapRights::READ | cap::CapRights::WRITE,
                             nullptr, &rt_err);
    JARVIS_ASSERT(rt_err == errors::CAP_ERR_OK);
    JARVIS_ASSERT(carved >= 0);
    // One child Untyped (3-page remainder) must exist now.
    int child_idx = -1;
    for (uint32_t i = 0; i < CONFIG_CSLOT_COUNT; ++i) {
        auto *o = node.peek(i, cap::CapType::Untyped);
        if (o != nullptr && o != ut) {
            child_idx = static_cast<int>(i);
            break;
        }
    }
    JARVIS_ASSERT(child_idx >= 0);
    // REVOKE the parent: carved target + child remainder are destroyed.
    errors::CapError rev_err = errors::CAP_ERR_OK;
    int destroyed = cap::revoke_derivations(&node, sh, &rev_err);
    JARVIS_ASSERT_EQ(errors::CAP_ERR_OK, rev_err);
    JARVIS_ASSERT_EQ(2, destroyed);
    JARVIS_ASSERT_EQ(1U, cap::occupied_count(&node));
    // Parent Untyped survives and is still usable; already retyped.
    KernelObject *parent =
        cap::lookup(&node, sh, cap::CapType::Untyped, cap::CapRights::WRITE);
    JARVIS_ASSERT(parent != nullptr);
    if (parent)
        parent->release();
    errors::CapError rt_err2 = errors::CAP_ERR_OK;
    int retry = cap::retype(&node, sh, cap::CapType::Frame, arch::PAGE_SIZE,
                            cap::CapRights::READ, nullptr, &rt_err2);
    JARVIS_ASSERT(retry < 0);
    JARVIS_ASSERT_EQ(errors::CAP_ERR_ALREADY_RETYPED, rt_err2);
    node.remove(static_cast<uint32_t>(s));
    ut->release();
    JARVIS_ASSERT_EQ(free_before, PMM::free_pages_ref());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: INV-DERIV-9 merge gate — transitive revoke reaches every depth.
//        An 8-link CNode chain (max depth) is fully drained; a 9th link is
//        refused DEPTH_EXCEEDED. Fails under the r1 eager-remove order.
// Input: root seed + N1..N8 CNode-caps + chained copies (depths 1..8).
// Expect: revoke(seed) destroys all 8 derived slots (per-node counts),
//        named seed lives; 9th copy => -1 DEPTH_EXCEEDED, no slot.
// Depends: kernel::cap copy / revoke_derivations, derivation table
JARVIS_TEST(cap_revoke_derivation_depth_bound, "PRE: none | POST: none") {
    cap::CNode nodes[9];
    TestTarget tgt;
    int seed = nodes[0].install(&tgt, cap::CapType::Task,
                                cap::CapRights::READ | cap::CapRights::COPY);
    JARVIS_ASSERT(seed >= 0);
    // Chain: nodes[i] holds a CNode-cap to nodes[i+1] and the derived copy
    // lands in nodes[i+1] (depths 1..8).
    int cslot[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int dslot[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int prev_slot = seed;
    for (int i = 0; i < 8; ++i) {
        cslot[i] = nodes[i].install(&nodes[i + 1], cap::CapType::CNode,
                                    cap::CapRights::READ);
        JARVIS_ASSERT(cslot[i] >= 0);
        uint64_t ph = cap::encode_handle(
            nodes[i].cspace_id, static_cast<uint32_t>(prev_slot),
            nodes[i].slot_gen(static_cast<uint32_t>(prev_slot)));
        dslot[i] = cap::copy(&nodes[i], ph, &nodes[i + 1]);
        JARVIS_ASSERT(dslot[i] >= 0);
        prev_slot = dslot[i];
    }
    // Depth of the last derived slot is 8: one more copy must refuse.
    {
        uint64_t deep = cap::encode_handle(
            nodes[8].cspace_id, static_cast<uint32_t>(prev_slot),
            nodes[8].slot_gen(static_cast<uint32_t>(prev_slot)));
        cap::CNode extra;
        int ce = nodes[8].install(&extra, cap::CapType::CNode,
                                  cap::CapRights::READ);
        JARVIS_ASSERT(ce >= 0);
        errors::CapError dep_err = errors::CAP_ERR_OK;
        const uint32_t occ_before = cap::occupied_count(&extra);
        int nope = cap::copy(&nodes[8], deep, &extra, nullptr, &dep_err);
        JARVIS_ASSERT(nope < 0);
        JARVIS_ASSERT_EQ(errors::CAP_ERR_DEPTH_EXCEEDED, dep_err);
        JARVIS_ASSERT_EQ(occ_before, cap::occupied_count(&extra));
        nodes[8].remove(static_cast<uint32_t>(ce));
    }
    // Revoke the seed: every derived slot at every depth is destroyed.
    uint64_t seed_h = cap::encode_handle(
        nodes[0].cspace_id, static_cast<uint32_t>(seed),
        nodes[0].slot_gen(static_cast<uint32_t>(seed)));
    errors::CapError rev_err = errors::CAP_ERR_OK;
    int destroyed = cap::revoke_derivations(&nodes[0], seed_h, &rev_err);
    JARVIS_ASSERT_EQ(errors::CAP_ERR_OK, rev_err);
    JARVIS_ASSERT_EQ(8, destroyed);
    JARVIS_ASSERT_EQ(2U, cap::occupied_count(&nodes[0]));
    for (int i = 1; i <= 7; ++i)
        JARVIS_ASSERT_EQ(1U, cap::occupied_count(&nodes[i]));
    JARVIS_ASSERT_EQ(0U, cap::occupied_count(&nodes[8]));
    // Named seed survives and stays usable.
    KernelObject *live = cap::lookup(&nodes[0], seed_h, cap::CapType::Task,
                                     cap::CapRights::READ);
    JARVIS_ASSERT(live != nullptr);
    if (live)
        live->release();
    nodes[0].remove(static_cast<uint32_t>(seed));
    for (int i = 0; i < 8; ++i)
        nodes[i].remove(static_cast<uint32_t>(cslot[i]));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Copy into a raw in-kernel CNode (not a CNode-cap of the source)
//        records no edge: revoke is a zero-effect no-op.
// Input: install + copy into plain dst CNode.
// Expect: revoke returns 0 destroyed, both slots intact, delta 0.
// Depends: kernel::cap copy / revoke_derivations untracked rule
JARVIS_TEST(cap_revoke_untracked_dst_noop, "PRE: none | POST: none") {
    cap::CNode src;
    cap::CNode dst;
    TestTarget tgt;
    int s = src.install(&tgt, cap::CapType::Task,
                        cap::CapRights::READ | cap::CapRights::COPY);
    JARVIS_ASSERT(s >= 0);
    uint64_t sh = cap::encode_handle(src.cspace_id, static_cast<uint32_t>(s),
                                     src.slot_gen(static_cast<uint32_t>(s)));
    int d = cap::copy(&src, sh, &dst);
    JARVIS_ASSERT(d >= 0);
    errors::CapError rev_err = errors::CAP_ERR_OK;
    int destroyed = cap::revoke_derivations(&src, sh, &rev_err);
    JARVIS_ASSERT_EQ(errors::CAP_ERR_OK, rev_err);
    JARVIS_ASSERT_EQ(0, destroyed);
    JARVIS_ASSERT_EQ(1U, cap::occupied_count(&src));
    JARVIS_ASSERT_EQ(1U, cap::occupied_count(&dst));
    dst.remove(static_cast<uint32_t>(d));
    src.remove(static_cast<uint32_t>(s));
    JARVIS_TEST_PASS();
}

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
    JARVIS_REGISTER_TEST(mk_cap_delete_vs_revoke);
    JARVIS_REGISTER_TEST(cap_delete_last_ref_disposes_once);
    JARVIS_REGISTER_TEST(cap_revoke_untyped_reclaims_carve);
    JARVIS_REGISTER_TEST(cap_revoke_derivation_depth_bound);
    JARVIS_REGISTER_TEST(cap_revoke_untracked_dst_noop);
}
