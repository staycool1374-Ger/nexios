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

/// @file test_cap_ipc.cpp
/// @brief Capability-gated IPC (CapEndpoint send/recv) and frame mapping
///        (CapFrame map/unmap) integration tests.  Driven style: real tasks,
///        real blocking, real scheduling.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/cap/endpoint.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/test/resource_tracker.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

/// @brief Global handshake flags shared with the real tasks below.
static uint64_t g_send_ok = 0;
static uint64_t g_recv_ok = 0;
static uint64_t g_recv_type = 0;
static uint64_t g_revoked_send_ok = 0;
static uint64_t g_map_ok = 0;
static uint64_t g_unmap_ok = 0;
static uint64_t g_revoke_denied_map_ok = 0;
static uint64_t g_no_cap_send_ok = 0;
static uint64_t g_teardown_ok = 0;

cap::Endpoint *g_ep_global;
Message g_msg_global;
cap::FrameCap *g_frame_global;

/// @brief A sender task: pushes a message through the endpoint.
void sender_entry() {
    g_send_ok = IPC::send_via_cap(g_ep_global, g_msg_global) ? 1 : 0;
    Scheduler::terminate(*Scheduler::current_task(), 0);
}

/// @brief A receiver task: drains the endpoint queue into its own queue.
void receiver_entry() {
    Message m;
    g_recv_ok = IPC::recv_via_cap(g_ep_global, m) ? 1 : 0;
    g_recv_type = m.type;
    Scheduler::terminate(*Scheduler::current_task(), 0);
}

/// @brief A sender against a revoked endpoint must fail.
void revoked_sender_entry() {
    g_revoked_send_ok = IPC::send_via_cap(g_ep_global, g_msg_global) ? 1 : 0;
    Scheduler::terminate(*Scheduler::current_task(), 0);
}

/// @brief Maps a FrameCap into a scratch PML4.
void frame_map_entry() {
    uint64_t pml4 = VMM::clone_kernel_pml4();
    if (!pml4) {
        g_map_ok = 2;
        Scheduler::terminate(*Scheduler::current_task(), 0);
        return;
    }
    g_map_ok = VMM::map_frame_from_cap(g_frame_global, 0x400000ULL, false, pml4)
                   ? 1
                   : 0;
    if (g_map_ok == 1) {
        g_unmap_ok =
            (VMM::virt_to_phys_in_pml4(0x400000ULL, pml4) == g_frame_global->phys)
                ? 1
                : 0;
        VMM::unmap_frame_from_cap(0x400000ULL, pml4);
    }
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    Scheduler::terminate(*Scheduler::current_task(), 0);
}

/// @brief Mapping a revoked FrameCap must be refused.
void revoked_frame_map_entry() {
    uint64_t pml4 = VMM::clone_kernel_pml4();
    if (!pml4) {
        g_revoke_denied_map_ok = 2;
        Scheduler::terminate(*Scheduler::current_task(), 0);
        return;
    }
    g_revoke_denied_map_ok =
        VMM::map_frame_from_cap(g_frame_global, 0x400000ULL, false, pml4) ? 1
                                                                         : 0;
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    Scheduler::terminate(*Scheduler::current_task(), 0);
}

/// @brief Sending with a null endpoint (no capability) must fail.
void no_cap_send_entry() {
    g_no_cap_send_ok = IPC::send_via_cap(nullptr, g_msg_global) ? 1 : 0;
    Scheduler::terminate(*Scheduler::current_task(), 0);
}

/// @brief An endpoint that survives to the end of a test disposes cleanly.
void endpoint_teardown_entry() {
    g_teardown_ok = 1;
    Scheduler::terminate(*Scheduler::current_task(), 0);
}

/// @brief Creates a real task, runs it to termination, drains the zombie list.
TaskControlBlock *run_cap_ipc_task(void (*entry)(), uint64_t prio = 11) {
    auto *t = TaskControlBlock::create(entry, prio, 10);
    if (t == nullptr)
        return nullptr;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();
    return t;
}

} // namespace

// Runmode: kernel
// Testidea: A sender task pushes a message through a capability endpoint; a
//           receiver task (the bound receiver) drains it.
// Input: create endpoint, bind receiver, run sender + receiver tasks
// Expect: send succeeds; recv succeeds with the expected type
// Depends: kernel::cap::Endpoint, kernel::IPC
JARVIS_TEST(endpoint_send_recv_roundtrip, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    g_send_ok = 0;
    g_recv_ok = 0;
    g_recv_type = 0;

    auto *ep = cap::Endpoint::create(0x100);
    JARVIS_ASSERT(ep != nullptr);
    g_ep_global = ep;
    g_msg_global = {};
    g_msg_global.type = 0x42;
    g_msg_global.data_size = 0;
    g_msg_global.priority = 0;

    // Sender runs first: pushes into the (empty) endpoint queue.
    auto *sender = run_cap_ipc_task(sender_entry, 11);
    JARVIS_ASSERT(sender != nullptr);

    // Receiver runs second: drains the queued message.
    auto *receiver = run_cap_ipc_task(receiver_entry, 12);
    JARVIS_ASSERT(receiver != nullptr);
    ep->bound_receiver = receiver;

    JARVIS_ASSERT_EQ(1ULL, g_send_ok);
    JARVIS_ASSERT_EQ(1ULL, g_recv_ok);
    JARVIS_ASSERT_EQ(0x42ULL, g_recv_type);

    ep->release(); // creator ref -> dispose

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A revoked endpoint refuses a send.
// Input: create endpoint, revoke it, run a sender
// Expect: send_via_cap fails (revoked)
// Depends: kernel::cap::Endpoint, kernel::IPC
JARVIS_TEST(endpoint_revoked_send_fails, "PRE: none | POST: none") {
    g_revoked_send_ok = 0;

    auto *ep = cap::Endpoint::create(0x200);
    JARVIS_ASSERT(ep != nullptr);
    g_ep_global = ep;
    g_msg_global = {};
    g_msg_global.type = 0x1;
    g_msg_global.data_size = 0;
    g_msg_global.priority = 0;

    ep->revoke();

    auto *t = run_cap_ipc_task(revoked_sender_entry, 11);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_revoked_send_ok);

    ep->release(); // creator ref -> dispose
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A FrameCap maps its frames into a scratch PML4 and unmaps cleanly.
// Input: alloc a frame, wrap in FrameCap, run a map task
// Expect: map succeeds; virt_to_phys returns the frame; unmap succeeds
// Depends: kernel::cap::FrameCap, kernel::memory::VMM, kernel::memory::PMM
JARVIS_TEST(frame_cap_map_unmap_roundtrip, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    g_map_ok = 0;
    g_unmap_ok = 0;

    uint64_t phys = PMM::alloc_page();
    JARVIS_ASSERT(phys != 0);
    auto *fc = cap::FrameCap::create(phys, 1, false);
    JARVIS_ASSERT(fc != nullptr);
    g_frame_global = fc;

    auto *t = run_cap_ipc_task(frame_map_entry, 11);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(1ULL, g_map_ok);
    JARVIS_ASSERT_EQ(1ULL, g_unmap_ok);

    fc->release(); // creator ref -> dispose frees the page

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A revoked FrameCap refuses mapping.
// Input: create FrameCap, revoke it, run a map task
// Expect: map_frame_from_cap returns false
// Depends: kernel::cap::FrameCap, kernel::memory::VMM
JARVIS_TEST(frame_cap_revoke_denies_map, "PRE: none | POST: none") {
    g_revoke_denied_map_ok = 0;

    uint64_t phys = PMM::alloc_page();
    JARVIS_ASSERT(phys != 0);
    auto *fc = cap::FrameCap::create(phys, 1, false);
    JARVIS_ASSERT(fc != nullptr);
    fc->revoke();
    g_frame_global = fc;

    auto *t = run_cap_ipc_task(revoked_frame_map_entry, 11);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_revoke_denied_map_ok);

    fc->release(); // creator ref -> dispose frees the page
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Sending without an endpoint capability (null) fails.
// Input: run a sender with a null endpoint pointer
// Expect: send_via_cap returns false
// Depends: kernel::IPC
JARVIS_TEST(send_without_endpoint_cap_denied, "PRE: none | POST: none") {
    g_no_cap_send_ok = 0;
    g_msg_global = {};
    g_msg_global.type = 0x3;
    g_msg_global.data_size = 0;
    g_msg_global.priority = 0;

    auto *t = run_cap_ipc_task(no_cap_send_entry, 11);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_no_cap_send_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: An endpoint whose last reference is dropped during a test tears
//           down with zero ResourceTracker delta.
// Input: create endpoint, run a trivial task, release the creator ref
// Expect: cap_objects returns to baseline
// Depends: kernel::cap::Endpoint
JARVIS_TEST(endpoint_teardown_no_leak, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    g_teardown_ok = 0;
    auto *ep = cap::Endpoint::create(0x300);
    JARVIS_ASSERT(ep != nullptr);
    g_ep_global = ep;

    auto *t = run_cap_ipc_task(endpoint_teardown_entry, 11);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(1ULL, g_teardown_ok);

    ep->release(); // creator ref -> dispose

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all capability-gated IPC / frame mapping tests.
// Input: None
// Expect: All tests registered
// Depends: test framework
void register_cap_ipc_tests() {
    Logger::info("Registering capability-gated IPC / frame mapping tests");
    JARVIS_REGISTER_TEST(endpoint_send_recv_roundtrip);
    JARVIS_REGISTER_TEST(endpoint_revoked_send_fails);
    JARVIS_REGISTER_TEST(frame_cap_map_unmap_roundtrip);
    JARVIS_REGISTER_TEST(frame_cap_revoke_denies_map);
    JARVIS_REGISTER_TEST(send_without_endpoint_cap_denied);
    JARVIS_REGISTER_TEST(endpoint_teardown_no_leak);
}
