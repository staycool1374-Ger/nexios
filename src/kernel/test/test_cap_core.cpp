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

/// @file test_cap_core.cpp
/// @brief CSpace core engine tests: CNode/CSlot lifecycle, handle decode,
///        revocation, cascade revoke and root-CNode task teardown.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/test/resource_tracker.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

/// @brief Minimal shared-heap KernelObject used as a capability target.
///        Tracks dispose()/revoke() invocations for lifecycle assertions.
///        Stack-allocated in tests (never pool-marked), so dispose() only
///        counts — it must not MemPool::free.
struct TestTarget : public KernelObject {
    static uint32_t g_dispose_count;
    static uint32_t g_revoke_count;

    void dispose() noexcept override { ++g_dispose_count; }
    bool is_shared() const noexcept override { return true; }
    void revoke() noexcept override {
        ++g_revoke_count;
        KernelObject::revoke();
    }
};

uint32_t TestTarget::g_dispose_count = 0;
uint32_t TestTarget::g_revoke_count = 0;

/// @brief A task whose entry touches no capabilities, created to verify that
///        root-CNode teardown happens when the TCB's object list is released.
static uint64_t g_teardown_ok = 0;

void teardown_probe_entry() {
    g_teardown_ok = 1;
}

} // namespace

// Runmode: kernel
// Testidea: A freshly created CNode has every slot unoccupied.
// Input: Stack CNode, inspect all slots
// Expect: occupied_count == 0, peek() fails for every slot
// Depends: kernel::cap::CNode
JARVIS_TEST(cslot_init_empty, "PRE: none | POST: none") {
    cap::CNode node;
    JARVIS_ASSERT_EQ(0U, cap::occupied_count(&node));
    for (uint32_t i = 0; i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT); ++i)
        JARVIS_ASSERT(node.peek(i, cap::CapType::Task) == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: install() takes a strong reference on the target (acquire).
// Input: install a TestTarget, check refcount 1 -> 2
// Expect: refcount bumped; remove() drops it back to 1
// Depends: kernel::cap::CNode, kernel::KernelObject
JARVIS_TEST(cnode_install_acquires_ref, "PRE: none | POST: none") {
    cap::CNode node;
    TestTarget tgt;
    JARVIS_ASSERT_EQ(1U, tgt.refcount());
    int idx = node.install(&tgt, cap::CapType::Task, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(idx >= 0);
    JARVIS_ASSERT_EQ(2U, tgt.refcount());
    JARVIS_ASSERT_EQ(1U, cap::occupied_count(&node));
    node.remove(static_cast<uint32_t>(idx));
    JARVIS_ASSERT_EQ(1U, tgt.refcount());
    JARVIS_ASSERT_EQ(0U, cap::occupied_count(&node));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Removing the last slot reference releases the target but does
//           not dispose a stack (non-pool-backed) object.
// Input: install then remove a stack TestTarget
// Expect: refcount returns to 1; dispose NOT called (owner ref still held)
// Depends: kernel::cap::CNode
JARVIS_TEST(cnode_release_drops_ref, "PRE: none | POST: none") {
    TestTarget::g_dispose_count = 0;
    cap::CNode node;
    TestTarget tgt;
    int idx = node.install(&tgt, cap::CapType::Task, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(idx >= 0);
    node.remove(static_cast<uint32_t>(idx));
    JARVIS_ASSERT_EQ(1U, tgt.refcount());
    JARVIS_ASSERT_EQ(0U, TestTarget::g_dispose_count);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A pool-backed CNode's dispose() frees the MemPool block and
//           drops the cap_objects ResourceTracker count back to baseline.
// Input: CNode::create(), install a stack target, release the last ref
// Expect: dispose runs; cap_objects returns to 0; block freed
// Depends: kernel::cap::CNode, kernel::memory::MemPool
JARVIS_TEST(cnode_dispose_frees_pool_block, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);
    TestTarget::g_dispose_count = 0;
    TestTarget tgt;

    cap::CNode *node = cap::CNode::create(0xAB);
    JARVIS_ASSERT(node != nullptr);
    JARVIS_ASSERT(node->is_pool_backed());
    int idx = node->install(&tgt, cap::CapType::Task, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(idx >= 0);
    JARVIS_ASSERT_EQ(1U, tgt.refcount() - 1U);

    node->release(); // creator ref -> dispose(); tgt still held by slot ref
    JARVIS_ASSERT_EQ(1U, tgt.refcount());

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: revoke() marks the object revoked; subsequent acquire() refuses.
// Input: install target, call node.revoke()
// Expect: tgt.revoked() true; acquire() returns false
// Depends: kernel::cap::CNode, kernel::KernelObject
JARVIS_TEST(cnode_revoke_refuses_acquire, "PRE: none | POST: none") {
    TestTarget::g_revoke_count = 0;
    cap::CNode node;
    TestTarget tgt;
    int idx = node.install(&tgt, cap::CapType::Task, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(idx >= 0);
    node.revoke();
    JARVIS_ASSERT_EQ(1U, TestTarget::g_revoke_count);
    JARVIS_ASSERT(tgt.revoked());
    JARVIS_ASSERT(!tgt.acquire());
    node.remove(static_cast<uint32_t>(idx));
    JARVIS_ASSERT_EQ(1U, tgt.refcount());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Revoking a parent CNode cascades: child CNode and leaf target
//           are both marked revoked.
// Input: parent -> child (CNode cap) -> leaf (Task cap); revoke parent
// Expect: leaf.revoked() true, child.revoked() true
// Depends: kernel::cap::CNode
JARVIS_TEST(cnode_cascade_revoke_marks_children, "PRE: none | POST: none") {
    TestTarget::g_revoke_count = 0;
    cap::CNode parent;
    cap::CNode child;
    TestTarget leaf;
    int cidx = child.install(&leaf, cap::CapType::Task, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(cidx >= 0);
    int pidx = parent.install(&child, cap::CapType::CNode, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(pidx >= 0);

    parent.revoke();
    JARVIS_ASSERT(child.revoked());
    JARVIS_ASSERT(leaf.revoked());
    // Cascade also invoked revoke() on the child's leaf target.
    JARVIS_ASSERT_EQ(2U, TestTarget::g_revoke_count);

    parent.remove(static_cast<uint32_t>(pidx));
    child.remove(static_cast<uint32_t>(cidx));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: encode_handle / handle_slot / handle_cspace / handle_gen are a
//           lossless round trip.
// Input: encode then decode a known handle
// Expect: cspace/slot/gen all round-trip
// Depends: kernel::cap
JARVIS_TEST(handle_decode_valid, "PRE: none | POST: none") {
    uint64_t h = cap::encode_handle(0x42, 7, 3);
    JARVIS_ASSERT_EQ(0x42U, cap::handle_cspace(h));
    JARVIS_ASSERT_EQ(7U, cap::handle_slot(h));
    JARVIS_ASSERT_EQ(3U, cap::handle_gen(h));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A handle addressed to a foreign CSpace (different cspace_id) is
//           rejected — the out-of-authority boundary.
// Input: install a target in node (cspace_id 0), look up a handle that
//        encodes cspace_id 1
// Expect: lookup returns nullptr (cspace mismatch)
// Depends: kernel::cap
JARVIS_TEST(handle_decode_out_of_range_fails, "PRE: none | POST: none") {
    cap::CNode node;
    TestTarget tgt;
    int idx = node.install(&tgt, cap::CapType::Task, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(idx >= 0);
    // Slot index is masked to CAP_SLOT_BITS (power-of-two table), so a
    // decodable slot is always in-range; the authority boundary is the
    // cspace_id, which here addresses a different CSpace.
    uint64_t foreign = cap::encode_handle(
        static_cast<uint32_t>(node.cspace_id + 1u), static_cast<uint32_t>(idx),
        0);
    KernelObject *got = cap::lookup(&node, foreign, cap::CapType::Task,
                                    cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(got == nullptr);
    node.remove(static_cast<uint32_t>(idx));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A stale generation (from a removed/recycled slot) fails decode.
// Input: install, capture handle with gen, remove (bumps gen), reuse slot,
//        look up with the old handle
// Expect: lookup returns nullptr (gen mismatch)
// Depends: kernel::cap
JARVIS_TEST(handle_decode_stale_gen_fails, "PRE: none | POST: none") {
    cap::CNode node;
    TestTarget tgt;
    TestTarget tgt2;
    int idx = node.install(&tgt, cap::CapType::Task, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(idx >= 0);
    uint32_t old_gen = node.slot_gen(static_cast<uint32_t>(idx));
    uint64_t old_handle =
        cap::encode_handle(node.cspace_id, static_cast<uint32_t>(idx), old_gen);

    // remove bumps gen; reinstall same slot with a new target
    node.remove(static_cast<uint32_t>(idx));
    int idx2 = node.install(&tgt2, cap::CapType::Task, cap::CAP_RIGHT_READ);
    JARVIS_ASSERT_EQ(idx, idx2);
    JARVIS_ASSERT(node.slot_gen(static_cast<uint32_t>(idx2)) != old_gen);

    KernelObject *got = cap::lookup(&node, old_handle, cap::CapType::Task,
                                    cap::CAP_RIGHT_READ);
    JARVIS_ASSERT(got == nullptr);
    node.remove(static_cast<uint32_t>(idx2));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A task that touches capabilities gets a root CNode attached to
//           its object list; task teardown releases and frees it.
// Input: ensure_cspace() on a real task, terminate, assert cap_objects delta
// Expect: cap_objects and cap_slots return to baseline after teardown
// Depends: kernel::cap::CNode, kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(root_cnode_attached_to_task_teardown_frees,
            "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    g_teardown_ok = 0;
    auto *t = TaskControlBlock::create(teardown_probe_entry, 11, 10);
    JARVIS_ASSERT(t != nullptr);
    t->ensure_cspace();
    JARVIS_ASSERT(t->get_cspace() != nullptr);
    JARVIS_ASSERT(t->get_cspace()->is_pool_backed());

    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, g_teardown_ok);

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all CSpace core engine tests.
// Input: None
// Expect: All tests registered
// Depends: test framework
void register_cap_core_tests() {
    Logger::info("Registering CSpace core engine tests");
    JARVIS_REGISTER_TEST(cslot_init_empty);
    JARVIS_REGISTER_TEST(cnode_install_acquires_ref);
    JARVIS_REGISTER_TEST(cnode_release_drops_ref);
    JARVIS_REGISTER_TEST(cnode_dispose_frees_pool_block);
    JARVIS_REGISTER_TEST(cnode_revoke_refuses_acquire);
    JARVIS_REGISTER_TEST(cnode_cascade_revoke_marks_children);
    JARVIS_REGISTER_TEST(handle_decode_valid);
    JARVIS_REGISTER_TEST(handle_decode_out_of_range_fails);
    JARVIS_REGISTER_TEST(handle_decode_stale_gen_fails);
    JARVIS_REGISTER_TEST(root_cnode_attached_to_task_teardown_frees);
}
