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

/// @file test_ipc.cpp
/// @brief IPC (Inter-Process Communication) tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): waiter-list and blocking tests use
/// REAL kernel tasks (prio ≥ 11) dispatched by the real timer ISR.  A real
/// sender genuinely blocks in IPC::send() on a full receiver queue and a real
/// receiver (or the harness as a real IPC peer) drains it — the block/wake
/// transitions are reached through real execution, never via set_current
/// impersonation or direct state writes.

#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-null-argument"
#pragma GCC diagnostic ignored "-Wanalyzer-possible-null-dereference"
#endif

#include <test.hpp>
#include <logger.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/sync/notify.hpp>
#include <kernel/sync/eventgroup.hpp>
#include <kernel/task/scheduler.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

/// @brief  Context for a task lambda (captureless lambdas only).
struct IpcCtx {
    uint64_t peer_id_;
    uint64_t out_;
};

/// @brief  Fill a task's queue to capacity (setup: the queue is genuinely
///         full so a real sender blocks on it).
void fill_queue(TaskControlBlock &dst) {
    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        kernel::Message fill{};
        fill.sender_id = 0;
        fill.type = 99;
        fill.priority = 0;
        fill.data_size = 0;
        dst.msg_queue.push(fill);
    }
}

/// @brief  Register a receiver task in BLOCKED state (create_test_task
///         pattern): find_task() resolves its id for IPC routing, but the
///         scheduler NEVER dispatches it (not in the ready queue), so the
///         harness can deterministically observe a blocked sender BEFORE
///         releasing the receiver via set_task_ready().  Without this, the
///         prio-11 receiver is dispatched by the timer the tick after the
///         prio-12 sender blocks, completing the whole drain before the
///         prio-10 harness gets CPU to observe BLOCKED.
void register_blocked_receiver(TaskControlBlock &t) {
    t.state = TaskState::BLOCKED;
    Scheduler::register_task(t);
}

/// @brief  Create a draining receiver (prio 11) that stays ALIVE after
///         draining until the blocked sender's completing re-push arrives.
///         IPC::recv wakes the blocked sender (wake_sender); the poll loop
///         keeps the receiver running (not terminated) so the sender's
///         re-lookup in IPC::send finds a live destination and the send
///         SUCCEEDS (send_result == 1).  Mirrors the poll pattern of the
///         passing ipc_send_sync_was_blocked_restores_state receiver.
/// @param  recv_ok Out-param set to 1 iff the first drain popped a message.
TaskControlBlock *spawn_draining_receiver(uint64_t &recv_ok) {
    static IpcCtx rctx;
    rctx.peer_id_ = 0;
    rctx.out_ = reinterpret_cast<uint64_t>(&recv_ok);
    auto *receiver = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<IpcCtx *>(self->user_data);
            kernel::Message m;
            // First pop also calls wake_sender (blocked sender becomes READY).
            bool ok = IPC::recv(m);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_),
                             ok ? 1 : 0, __ATOMIC_RELEASE);
            // Stay alive until the sender's completing re-push arrives, then
            // consume it, so the sender's send succeeds.
            for (int i = 0; i < 100000; ++i) {
                kernel::Message m2;
                if (IPC::recv(m2) && m2.type == 1)
                    break;
            }
        },
        11, 10);
    if (receiver == nullptr)
        return nullptr;
    receiver->user_data = &rctx;
    return receiver;
}

} // namespace

// Runmode: kernel
// Testidea: Verifies that a freshly initialized MessageQueue reports empty,
// not full, with correct priority level and zero bitmap.
// Input: None (basic init)
// Expect: is_empty() == true, is_full() == false, highest_priority() ==
// IPC_PRIORITY_LEVELS, prio_bitmap == 0
// Depends: kernel::MessageQueue
JARVIS_TEST(ipc_queue_init, "PRE: none | POST: none") {
    MessageQueue q;
    q.init();
    JARVIS_ASSERT(q.is_empty());
    JARVIS_ASSERT(!q.is_full());
    JARVIS_ASSERT_EQ(IPC_PRIORITY_LEVELS, q.highest_priority());
    JARVIS_ASSERT_EQ(0ULL, q.prio_bitmap);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that a message can be pushed and popped correctly,
// preserving all fields.
// Input: msg.sender_id=1, msg.type=42, msg.priority=0, msg.data_size=4,
// msg.data = 0xAA,0xBB,0xCC,0xDD
// Expect: push returns true, pop returns matching
// sender_id/type/priority/data_size/data, queue empty after pop
// Depends: kernel::MessageQueue
JARVIS_TEST(ipc_queue_push_pop, "PRE: none | POST: none") {
    MessageQueue q;
    q.init();
    Message msg;
    msg.sender_id = 1;
    msg.type = 42;
    msg.priority = 0;
    msg.data_size = 4;
    msg.data[0] = 0xAA;
    msg.data[1] = 0xBB;
    msg.data[2] = 0xCC;
    msg.data[3] = 0xDD;

    JARVIS_ASSERT(q.push(msg));
    JARVIS_ASSERT(!q.is_empty());
    JARVIS_ASSERT_EQ(1ULL, q.count);

    Message out;
    JARVIS_ASSERT(q.pop(out));
    JARVIS_ASSERT_EQ(1ULL, out.sender_id);
    JARVIS_ASSERT_EQ(42ULL, out.type);
    JARVIS_ASSERT_EQ(0ULL, out.priority);
    JARVIS_ASSERT_EQ(4ULL, out.data_size);
    JARVIS_ASSERT_EQ(0xAA, out.data[0]);
    JARVIS_ASSERT_EQ(0xBB, out.data[1]);
    JARVIS_ASSERT_EQ(0xCC, out.data[2]);
    JARVIS_ASSERT_EQ(0xDD, out.data[3]);
    JARVIS_ASSERT(q.is_empty());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that messages are dequeued in priority order (lowest
// priority value = highest priority).
// Input: Four messages with priorities 3, 2, 1, 0 pushed in that order
// Expect: Popped types are 3, 2, 1, 0 (priority 0 dequeued first, priority 3
// last)
// Depends: kernel::MessageQueue
JARVIS_TEST(ipc_queue_priority_order, "PRE: none | POST: none") {
    MessageQueue q;
    q.init();
    Message msgs[4];
    for (int i = 0; i < 4; ++i) {
        msgs[i].sender_id = 100 + i;
        msgs[i].type = static_cast<uint64_t>(i);
        msgs[i].priority = 3 - static_cast<uint64_t>(i);
        msgs[i].data_size = 0;
    }
    for (int i = 0; i < 4; ++i)
        JARVIS_ASSERT(q.push(msgs[i]));

    JARVIS_ASSERT_EQ(0ULL, q.highest_priority());
    Message out;
    JARVIS_ASSERT(q.pop(out));
    JARVIS_ASSERT_EQ(3ULL, out.type);

    JARVIS_ASSERT(q.pop(out));
    JARVIS_ASSERT_EQ(2ULL, out.type);

    JARVIS_ASSERT(q.pop(out));
    JARVIS_ASSERT_EQ(1ULL, out.type);

    JARVIS_ASSERT(q.pop(out));
    JARVIS_ASSERT_EQ(0ULL, out.type);

    JARVIS_ASSERT(q.is_empty());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies FIFO ordering for messages with the same priority level.
// Input: Five messages with priority=7, sender_id=0..4, type=i*10, pushed
// sequentially
// Expect: Popped in same order (sender_id 0..4, type 0,10,20,30,40)
// Depends: kernel::MessageQueue
JARVIS_TEST(ipc_queue_fifo_same_priority, "PRE: none | POST: none") {
    MessageQueue q;
    q.init();
    for (uint64_t i = 0; i < 5; ++i) {
        Message msg;
        msg.sender_id = i;
        msg.type = i * 10;
        msg.priority = 7;
        msg.data_size = 0;
        JARVIS_ASSERT(q.push(msg));
    }
    for (uint64_t i = 0; i < 5; ++i) {
        Message out;
        JARVIS_ASSERT(q.pop(out));
        JARVIS_ASSERT_EQ(i, out.sender_id);
        JARVIS_ASSERT_EQ(i * 10, out.type);
    }
    JARVIS_ASSERT(q.is_empty());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that the queue correctly reports full after
// IPC_MAX_QUEUE_MSG pushes and rejects subsequent pushes.
// Input: IPC_MAX_QUEUE_MSG pushes of a trivial message, then one extra push
// Expect: First IPC_MAX_QUEUE_MSG pushes succeed, is_full() true, extra push
// returns false
// Depends: kernel::MessageQueue, IPC_MAX_QUEUE_MSG
JARVIS_TEST(ipc_queue_full, "PRE: none | POST: none") {
    MessageQueue q;
    q.init();
    Message msg;
    msg.sender_id = 1;
    msg.type = 0;
    msg.priority = 0;
    msg.data_size = 0;

    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        JARVIS_ASSERT(q.push(msg));
    }
    JARVIS_ASSERT(q.is_full());
    JARVIS_ASSERT(!q.push(msg));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that popping from an empty MessageQueue returns false.
// Input: None (newly initialized queue)
// Expect: q.pop(out) returns false
// Depends: kernel::MessageQueue
JARVIS_TEST(ipc_queue_empty_pop, "PRE: none | POST: none") {
    MessageQueue q;
    q.init();
    Message out;
    JARVIS_ASSERT(!q.pop(out));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that the ring-buffer MessageQueue handles wrap-around
// correctly by filling, partially draining, then refilling.
// Input: Fill queue to IPC_MAX_QUEUE_MSG, pop half, push half again, then
// drain all
// Expect: Total drained count == IPC_MAX_QUEUE_MSG, queue empty at end
// Depends: kernel::MessageQueue, IPC_MAX_QUEUE_MSG
JARVIS_TEST(ipc_queue_wrap_around, "PRE: none | POST: none") {
    MessageQueue q;
    q.init();
    Message msg;
    msg.sender_id = 0;
    msg.type = 0;
    msg.priority = 0;
    msg.data_size = 0;

    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        JARVIS_ASSERT(q.push(msg));
    }
    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG / 2; ++i) {
        Message out;
        JARVIS_ASSERT(q.pop(out));
    }
    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG / 2; ++i) {
        JARVIS_ASSERT(q.push(msg));
    }
    size_t count = 0;
    Message out;
    while (q.pop(out))
        ++count;
    JARVIS_ASSERT_EQ(IPC_MAX_QUEUE_MSG, count);
    JARVIS_ASSERT(q.is_empty());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that highest_priority() correctly tracks the minimum
// priority value in the queue as messages are pushed and popped.
// Input: Push priority 15, then priority 5; pop priority 5, then priority 15
// Expect: After push(15) -> highest=15, after push(5) -> highest=5, after
// pop(5) -> highest=15, after pop(15) -> highest=IPC_PRIORITY_LEVELS
// Depends: kernel::MessageQueue, IPC_PRIORITY_LEVELS
JARVIS_TEST(ipc_queue_highest_priority, "PRE: none | POST: none") {
    MessageQueue q;
    q.init();

    Message msg;
    msg.sender_id = 0;
    msg.type = 0;
    msg.data_size = 0;

    msg.priority = 15;
    JARVIS_ASSERT(q.push(msg));
    JARVIS_ASSERT_EQ(15ULL, q.highest_priority());

    msg.priority = 5;
    JARVIS_ASSERT(q.push(msg));
    JARVIS_ASSERT_EQ(5ULL, q.highest_priority());

    Message out;
    JARVIS_ASSERT(q.pop(out));
    JARVIS_ASSERT_EQ(5ULL, out.priority);
    JARVIS_ASSERT_EQ(15ULL, q.highest_priority());

    JARVIS_ASSERT(q.pop(out));
    JARVIS_ASSERT_EQ(15ULL, out.priority);
    JARVIS_ASSERT_EQ(IPC_PRIORITY_LEVELS, q.highest_priority());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that IPC::send and IPC::recv work for self-messaging
// (sender == receiver).
// Input: msg type=77, priority=0, sent to own task ID
// Expect: send returns true, recv returns true, received type=77, sender_id
// matches current task, queue empty after recv
// Depends: kernel::MessageQueue, kernel::IPC, kernel::Scheduler
JARVIS_TEST(ipc_send_recv_self, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    Message msg;
    msg.sender_id = cur->id;
    msg.type = 77;
    msg.priority = 0;
    msg.data_size = 0;

    bool ok = IPC::send(cur->id, msg);
    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(!cur->msg_queue.is_empty());
    JARVIS_ASSERT_EQ(1ULL, cur->msg_queue.count);

    Message out;
    ok = IPC::recv(out);
    JARVIS_ASSERT(ok);
    JARVIS_ASSERT_EQ(77ULL, out.type);
    JARVIS_ASSERT_EQ(cur->id, out.sender_id);
    JARVIS_ASSERT(cur->msg_queue.is_empty());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that sending to a nonexistent task ID returns false.
// Input: IPC::send(999999, msg) where 999999 does not match any task
// Expect: send returns false
// Depends: kernel::IPC, kernel::Scheduler
JARVIS_TEST(ipc_send_nonexistent, "PRE: none | POST: none") {
    Message msg;
    msg.sender_id = 0;
    msg.type = 0;
    msg.priority = 0;
    msg.data_size = 0;

    bool ok = IPC::send(999999, msg);
    JARVIS_ASSERT(!ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that IPC::send with IPC_NONBLOCK flag returns false
// when the target queue is full.
// Input: Fill own queue to IPC_MAX_QUEUE_MSG, then send with IPC_NONBLOCK
// Expect: Non-blocking send returns false while queue is full; after
// draining, all recvs succeed and queue is empty
// Depends: kernel::MessageQueue, kernel::IPC, kernel::Scheduler,
// IPC_MAX_QUEUE_MSG, IPC_NONBLOCK
JARVIS_TEST(ipc_send_nonblock_full, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    Message msg;
    msg.sender_id = cur->id;
    msg.type = 0;
    msg.priority = 0;
    msg.data_size = 0;

    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        JARVIS_ASSERT(IPC::send(cur->id, msg));
    }
    JARVIS_ASSERT(cur->msg_queue.is_full());

    bool ok = IPC::send(cur->id, msg, IPC_NONBLOCK);
    JARVIS_ASSERT(!ok);

    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        Message out;
        JARVIS_ASSERT(IPC::recv(out));
    }
    JARVIS_ASSERT(cur->msg_queue.is_empty());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that Notify can store and overwrite a notification value.
// Input: n.notify(42), then n.notify(99)
// Expect: n.value() == 42 after first notify, n.value() == 99 after second
// Depends: kernel::sync::Notify
JARVIS_TEST(ipc_notify_basic, "PRE: none | POST: none") {
    sync::Notify n;
    n.init();
    JARVIS_ASSERT_EQ(0ULL, n.value());

    n.notify(42);
    JARVIS_ASSERT_EQ(42ULL, n.value());

    n.notify(99);
    JARVIS_ASSERT_EQ(99ULL, n.value());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that Notify::try_wait consumes a pending notification
// value.
// Input: try_wait on empty notify, then notify(55) and try_wait again
// Expect: First try_wait returns false; second returns true with val==55
// Depends: kernel::sync::Notify
JARVIS_TEST(ipc_notify_try_wait, "PRE: none | POST: none") {
    sync::Notify n;
    n.init();

    uint64_t val = 0;
    bool ok = n.try_wait(&val);
    JARVIS_ASSERT(!ok);

    n.notify(55);
    ok = n.try_wait(&val);
    JARVIS_ASSERT(ok);
    JARVIS_ASSERT_EQ(55ULL, val);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that EventGroup::set_bits and clear_bits manipulate the
// bitmask correctly.
// Input: set_bits(0x0F), clear_bits(0x05), set_bits(0xF0)
// Expect: get_bits() == 0x00 -> 0x0F -> 0x0A -> 0xFA
// Depends: kernel::sync::EventGroup
JARVIS_TEST(ipc_eventgroup_set_clear, "PRE: none | POST: none") {
    sync::EventGroup eg;
    eg.init();
    JARVIS_ASSERT_EQ(0ULL, eg.get_bits());

    eg.set_bits(0x0F);
    JARVIS_ASSERT_EQ(0x0FULL, eg.get_bits());

    eg.clear_bits(0x05);
    JARVIS_ASSERT_EQ(0x0AULL, eg.get_bits());

    eg.set_bits(0xF0);
    JARVIS_ASSERT_EQ(0xFAULL, eg.get_bits());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that EventGroup::try_wait_bits checks whether specific
// bits are set without consuming them.
// Input: set_bits(0x03), then try_wait for 0x01, 0x02, 0x03, 0x04
// Expect: try_wait(0x01) true, try_wait(0x02) true, try_wait(0x03) true,
// try_wait(0x04) false
// Depends: kernel::sync::EventGroup
JARVIS_TEST(ipc_eventgroup_try_wait, "PRE: none | POST: none") {
    sync::EventGroup eg;
    eg.init();

    JARVIS_ASSERT(!eg.try_wait_bits(0x01));

    eg.set_bits(0x03);
    JARVIS_ASSERT(eg.try_wait_bits(0x01));
    JARVIS_ASSERT(eg.try_wait_bits(0x02));
    JARVIS_ASSERT(eg.try_wait_bits(0x03));
    JARVIS_ASSERT(!eg.try_wait_bits(0x04));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that a REAL sender blocked on a full receiver queue is
// linked into the receiver's blocked-senders list.
// Input: Receiver task (prio 11) with a genuinely-full queue; a real sender
//        (prio 12) dispatches and blocks inside IPC::send().
// Expect: blocked_senders_head == sender, blocked_next == nullptr; after the
// receiver drains, the sender wakes and both terminate.
// Depends: kernel::MessageQueue, kernel::IPC, kernel::TaskControlBlock,
// kernel::Scheduler
JARVIS_TEST(ipc_block_sender_adds_to_list, "PRE: none | POST: none") {
    // Receiver registered BLOCKED (never dispatched by the timer) so the
    // harness can deterministically observe the blocked sender; the receiver
    // drains via IPC::recv (waking the sender) and stays ALIVE until the
    // sender's completing re-push arrives, so the send SUCCEEDS.
    uint64_t recv_ok = 0;
    auto *receiver = spawn_draining_receiver(recv_ok);
    JARVIS_ASSERT(receiver != nullptr);
    fill_queue(*receiver);
    uint64_t recv_id = receiver->id;
    register_blocked_receiver(*receiver);

    IpcCtx sctx{};
    uint64_t send_result = 0;
    sctx.peer_id_ = recv_id;
    sctx.out_ = reinterpret_cast<uint64_t>(&send_result);
    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<IpcCtx *>(self->user_data);
            kernel::Message msg{};
            msg.sender_id = self->id;
            msg.type = 1;
            msg.priority = 0;
            msg.data_size = 0;
            bool ok = IPC::send(c->peer_id_, msg);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_),
                             ok ? 1 : 0, __ATOMIC_RELEASE);
        },
        12, 10);
    JARVIS_ASSERT(sender != nullptr);
    sender->user_data = &sctx;
    Scheduler::add_task(*sender);
    Scheduler::reschedule();

    // Wait for the real block inside IPC::send().
    while (sender->state != TaskState::BLOCKED)
        arch::pause();

    JARVIS_ASSERT(receiver->msg_queue.blocked_senders_head == sender);
    JARVIS_ASSERT(receiver->msg_queue.blocked_senders_tail == sender);
    JARVIS_ASSERT(sender->blocked_next == nullptr);

    // Release the receiver: it drains one message via IPC::recv (waking the
    // sender), stays alive until the sender's re-push arrives; both terminate.
    Scheduler::set_task_ready(*receiver);
    kernel::test::wait_for_termination_safe(sender);
    kernel::test::wait_for_termination_safe(receiver);


    // Cleanup BEFORE asserting (cookbook Rule 5): both self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(send_result == 1);
    JARVIS_ASSERT(recv_ok == 1);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that IPC::recv() wakes a blocked sender and removes it
// from the receiver's blocked-senders list (real wake_sender path).
// Input: Receiver (prio 11) with a full queue; a real sender (prio 12)
//        blocks inside IPC::send().  The harness drains via IPC::recv.
// Expect: blocked_senders_head becomes nullptr after the wake; sender state
// reaches READY (then TERMINATED).
// Depends: kernel::MessageQueue, kernel::IPC, kernel::TaskControlBlock,
// kernel::Scheduler
JARVIS_TEST(ipc_wake_sender_removes_from_list, "PRE: none | POST: none") {
    // Receiver registered BLOCKED; sender blocks on the full queue; the
    // harness observes the block, then releases the receiver to drain (waking
    // the sender, which stays alive until the sender's re-push arrives).
    uint64_t recv_ok = 0;
    auto *receiver = spawn_draining_receiver(recv_ok);
    JARVIS_ASSERT(receiver != nullptr);
    fill_queue(*receiver);
    uint64_t recv_id = receiver->id;
    register_blocked_receiver(*receiver);

    IpcCtx sctx{};
    uint64_t send_result = 0;
    sctx.peer_id_ = recv_id;
    sctx.out_ = reinterpret_cast<uint64_t>(&send_result);
    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<IpcCtx *>(self->user_data);
            kernel::Message msg{};
            msg.sender_id = self->id;
            msg.type = 1;
            msg.priority = 0;
            msg.data_size = 0;
            bool ok = IPC::send(c->peer_id_, msg);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_),
                             ok ? 1 : 0, __ATOMIC_RELEASE);
        },
        12, 10);
    JARVIS_ASSERT(sender != nullptr);
    sender->user_data = &sctx;
    Scheduler::add_task(*sender);
    Scheduler::reschedule();
    while (sender->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT(receiver->msg_queue.blocked_senders_head == sender);

    // Release the receiver: its IPC::recv pops one → wake_sender removes the
    // sender from the list and makes it READY.
    Scheduler::set_task_ready(*receiver);
    kernel::test::wait_for_termination_safe(sender);
    kernel::test::wait_for_termination_safe(receiver);

    JARVIS_ASSERT(receiver->msg_queue.blocked_senders_head == nullptr);
    JARVIS_ASSERT(receiver->msg_queue.blocked_senders_tail == nullptr);
    JARVIS_ASSERT(sender->blocked_on_queue == nullptr);

    // Cleanup AFTER the zombie-field asserts, then drain.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(send_result == 1);
    JARVIS_ASSERT(recv_ok == 1);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that when the receiver terminates with blocked senders,
// the real cleanup path wakes them (MessageQueue::~MessageQueue fast-fail).
// Input: Receiver (prio 11) with a full queue; a real sender (prio 12)
//        blocked inside IPC::send().  The receiver terminates and is
//        cleaned up; the sender's blocked send fails and it terminates.
// Expect: Sender reaches TERMINATED without hanging; list emptied.
// Depends: kernel::MessageQueue, kernel::IPC, kernel::TaskControlBlock,
// kernel::Scheduler
JARVIS_TEST(ipc_wake_sender_terminated, "PRE: none | POST: none") {
    // Receiver registered BLOCKED; sender blocks on the full queue; the
    // harness observes the block, then releases the receiver whose empty
    // lambda runs → trampoline terminates it → cleanup() →
    // MessageQueue::~MessageQueue wakes the blocked sender (fast-fail).
    auto *receiver = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(receiver != nullptr);
    fill_queue(*receiver);
    uint64_t recv_id = receiver->id;
    register_blocked_receiver(*receiver);

    IpcCtx sctx{};
    uint64_t send_result = 0;
    sctx.peer_id_ = recv_id;
    sctx.out_ = reinterpret_cast<uint64_t>(&send_result);
    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<IpcCtx *>(self->user_data);
            kernel::Message msg{};
            msg.sender_id = self->id;
            msg.type = 1;
            msg.priority = 0;
            msg.data_size = 0;
            // The receiver terminates below; the blocked send must
            // fast-fail (return false) rather than hang forever.
            bool ok = IPC::send(c->peer_id_, msg);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_),
                             ok ? 1 : 0, __ATOMIC_RELEASE);
        },
        12, 10);
    JARVIS_ASSERT(sender != nullptr);
    sender->user_data = &sctx;
    Scheduler::add_task(*sender);
    Scheduler::reschedule();
    while (sender->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT(receiver->msg_queue.blocked_senders_head == sender);

    // Release the receiver: empty lambda → trampoline terminates it → drain
    // runs its cleanup() → MessageQueue::~MessageQueue wakes the blocked
    // sender.
    Scheduler::set_task_ready(*receiver);
    kernel::test::wait_for_termination_safe(receiver);
    Scheduler::drain_zombie_list();

    // The sender's spin-wait sees its own queue/blocked state cleared by the
    // destructor wake; it re-looks-up the (now gone) destination and fails.
    kernel::test::wait_for_termination_safe(sender);


    // Cleanup BEFORE asserting (cookbook Rule 5): both self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(send_result == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that a REAL sender blocked on a full receiver queue is
// woken when the receiver drains (priority restored on wake).
// Input: Receiver (prio 11) with a full queue; a real sender (prio 12)
//        blocks in IPC::send().  The receiver's priority is boosted while
//        the sender is blocked; after the drain it is restored to base.
// Expect: After the drain, the sender terminates and the receiver's priority
//         is its base_priority.
// Depends: kernel::MessageQueue, kernel::IPC, kernel::TaskControlBlock,
// kernel::Scheduler
JARVIS_TEST(ipc_wake_sender_restores_priority, "PRE: none | POST: none") {
    // Receiver registered BLOCKED; sender blocks on the full queue; the
    // harness observes the boost, then releases the receiver to drain (which
    // also restores the receiver's priority via wake_sender).
    uint64_t recv_ok = 0;
    auto *receiver = spawn_draining_receiver(recv_ok);
    JARVIS_ASSERT(receiver != nullptr);
    uint64_t recv_id = receiver->id;
    fill_queue(*receiver);
    register_blocked_receiver(*receiver);

    IpcCtx sctx{};
    uint64_t send_result = 0;
    sctx.peer_id_ = recv_id;
    sctx.out_ = reinterpret_cast<uint64_t>(&send_result);
    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<IpcCtx *>(self->user_data);
            kernel::Message msg{};
            msg.sender_id = self->id;
            msg.type = 1;
            msg.priority = 0;
            msg.data_size = 0;
            bool ok = IPC::send(c->peer_id_, msg);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_),
                             ok ? 1 : 0, __ATOMIC_RELEASE);
        },
        12, 10);
    JARVIS_ASSERT(sender != nullptr);
    sender->user_data = &sctx;
    Scheduler::add_task(*sender);
    Scheduler::reschedule();
    while (sender->state != TaskState::BLOCKED)
        arch::pause();

    // The receiver is boosted while a higher-priority sender is blocked.
    JARVIS_ASSERT(receiver->priority >= sender->priority);

    // Release the receiver: its IPC::recv drains → wake_sender restores the
    // receiver's priority; both terminate.
    Scheduler::set_task_ready(*receiver);
    kernel::test::wait_for_termination_safe(sender);
    kernel::test::wait_for_termination_safe(receiver);

    JARVIS_ASSERT(receiver->priority == receiver->base_priority);

    // Cleanup BEFORE asserting (cookbook Rule 5): both self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(send_result == 1);
    JARVIS_ASSERT(recv_ok == 1);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies IPC::send blocks a REAL sender when the target queue is
// full, then completes once the queue has space (real block/wake).
// Input: Own queue filled; a real sender (prio 12) dispatches and blocks
//        inside IPC::send().  The harness drains one message.
// Expect: sender is BLOCKED while the queue is full; after the drain it wakes,
// completes the send, and TERMINATES with result==1.
// Depends: kernel::MessageQueue, kernel::IPC, kernel::TaskControlBlock,
// kernel::Scheduler, IPC_MAX_QUEUE_MSG
JARVIS_TEST(ipc_send_block_full, "PRE: none | POST: none") {
    // Receiver registered BLOCKED; sender blocks on the full queue; the
    // harness observes the block, then releases the receiver to drain via
    // IPC::recv → the blocked sender's send completes.
    uint64_t recv_ok = 0;
    auto *receiver = spawn_draining_receiver(recv_ok);
    JARVIS_ASSERT(receiver != nullptr);
    fill_queue(*receiver);
    uint64_t recv_id = receiver->id;
    register_blocked_receiver(*receiver);

    IpcCtx sctx{};
    uint64_t send_result = 0;
    sctx.peer_id_ = recv_id;
    sctx.out_ = reinterpret_cast<uint64_t>(&send_result);
    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<IpcCtx *>(self->user_data);
            kernel::Message msg{};
            msg.sender_id = self->id;
            msg.type = 1;
            msg.priority = 0;
            msg.data_size = 0;
            bool ok = IPC::send(c->peer_id_, msg);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_),
                             ok ? 1 : 0, __ATOMIC_RELEASE);
        },
        12, 10);
    JARVIS_ASSERT(sender != nullptr);
    sender->user_data = &sctx;
    Scheduler::add_task(*sender);
    Scheduler::reschedule();
    while (sender->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT(sender->state == TaskState::BLOCKED);

    // Release the receiver: its IPC::recv drains one message → the blocked
    // sender's send completes; both terminate.
    Scheduler::set_task_ready(*receiver);
    kernel::test::wait_for_termination_safe(sender);
    kernel::test::wait_for_termination_safe(receiver);


    // Cleanup BEFORE asserting (cookbook Rule 5): both self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(send_result == 1);
    JARVIS_ASSERT(recv_ok == 1);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies synchronous IPC send/receive round-trip between two tasks.
// Input: Create sender and receiver tasks. Sender calls send_sync with a
// message, receiver recvs and replies.
// Expect: send_sync returns true, received message matches, reply received
// correctly.
JARVIS_TEST(ipc_send_sync_roundtrip, "PRE: none | POST: none") {
    static uint64_t g_receiver_id = 0;

    auto *receiver = TaskControlBlock::create(
        []() {
            Message msg;
            // Receiver waits for the sender's message.  IPC::recv is
            // non-blocking, so retry until it arrives (the sender always
            // delivers before blocking in send_sync).
            bool ok = false;
            for (int i = 0; i < 100000 && !ok; ++i)
                ok = IPC::recv(msg);
            JARVIS_ASSERT(ok);
            JARVIS_ASSERT_EQ(42ULL, msg.type);
            // Send reply
            Message reply;
            reply.sender_id = Scheduler::current_task()->id;
            reply.type = 99;
            reply.priority = 0;
            reply.data_size = 0;
            JARVIS_ASSERT(IPC::send(msg.sender_id, reply));
        },
        11, 10);
    JARVIS_ASSERT(receiver != nullptr);
    g_receiver_id = receiver->id;

    auto *sender = TaskControlBlock::create(
        []() {
            Message msg;
            msg.sender_id = Scheduler::current_task()->id;
            msg.type = 42;
            msg.priority = 0;
            msg.data_size = 0;

            Message reply;
            bool ok = IPC::send_sync(g_receiver_id, msg, reply);
            JARVIS_ASSERT(ok);
            JARVIS_ASSERT_EQ(99ULL, reply.type);
        },
        12, 10);
    JARVIS_ASSERT(sender != nullptr);
    // Add both tasks atomically so the timer ISR always sees both ready
    // (preventing the receiver from being scheduled before the sender).
    {
        arch::IrqGuard _guard;
        Scheduler::add_task(*sender);
        Scheduler::add_task(*receiver);
    }

    // Yield to the *receiver* (not the sender): next_task() skips the current
    // task, so yielding to the receiver makes next_task() return the
    // higher-priority sender (prio 12 > 11), which runs first, sends, and
    // blocks; the receiver then runs and replies.
    // yield_to_task restores the harness as current and re-enqueues the
    // receiver so the wait loop runs as PID 1 (avoiding RSP corruption)
    // and the receiver stays schedulable.
    kernel::test::yield_to_task(*receiver);

    // Drive the sender→receiver→sender handshake to completion.
    // Set scheduler_need_resched and HLT to let the timer ISR dispatch
    // the highest-priority task from the ready queue (the sender).
    // Unlike a tight reschedule() loop, this avoids repeated dequeue +
    // lazy-rebuild cycles that can resurrect stale READY tasks and wedge
    // the scheduler.  The sender runs, blocks in send_sync, the receiver
    // runs, replies, and both terminate — at which point the harness
    // resumes from HLT and exits the loop.
    __atomic_store_n(&scheduler_need_resched, true, __ATOMIC_RELEASE);
    // Both self-terminate via the trampoline; the zombie reaper may free +
    // 0xDD-poison their TCBs before the harness polls — the safe wait exits
    // on freed (magic != TCB_MAGIC) blocks instead of spinning forever.
    while ((TaskControlBlock::is_valid(sender) &&
            sender->state != TaskState::TERMINATED) ||
           (TaskControlBlock::is_valid(receiver) &&
            receiver->state != TaskState::TERMINATED)) {
        arch::hlt();
        __atomic_store_n(&scheduler_need_resched, true, __ATOMIC_RELEASE);
    }

    kernel::test::terminate_and_drain2(sender, receiver);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that a REAL sender blocked on a receiver that then
// terminates is unblocked via the real cleanup path (fast-fail).
// Input: Receiver (prio 11) with a full queue; a real sender (prio 12)
//        blocks inside IPC::send().  The receiver is dispatched, terminates,
//        and its cleanup wakes the sender.
// Expect: sender terminates; the send returns false (destination gone).
JARVIS_TEST(ipc_sender_unblocked_on_receiver_exit, "PRE: none | POST: none") {
    // Receiver registered BLOCKED; sender blocks on the full queue; the
    // harness observes the block, then releases the receiver whose empty
    // lambda runs → real exit: trampoline terminates → cleanup wakes the
    // sender (fast-fail).
    auto *receiver = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(receiver != nullptr);
    fill_queue(*receiver);
    uint64_t recv_id = receiver->id;
    register_blocked_receiver(*receiver);

    IpcCtx sctx{};
    uint64_t send_result = 0;
    sctx.peer_id_ = recv_id;
    sctx.out_ = reinterpret_cast<uint64_t>(&send_result);
    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<IpcCtx *>(self->user_data);
            kernel::Message msg{};
            msg.sender_id = self->id;
            msg.type = 1;
            msg.priority = 0;
            msg.data_size = 0;
            bool ok = IPC::send(c->peer_id_, msg);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_),
                             ok ? 1 : 0, __ATOMIC_RELEASE);
        },
        12, 10);
    JARVIS_ASSERT(sender != nullptr);
    sender->user_data = &sctx;
    Scheduler::add_task(*sender);
    Scheduler::reschedule();
    while (sender->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT(receiver->msg_queue.blocked_senders_head == sender);

    // Release the receiver → real termination + cleanup.
    Scheduler::set_task_ready(*receiver);
    kernel::test::wait_for_termination_safe(receiver);
    Scheduler::drain_zombie_list();

    // Sender fast-fails once the receiver's cleanup woke it.
    kernel::test::wait_for_termination_safe(sender);

    // Cleanup BEFORE asserting (cookbook Rule 5): both self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(send_result == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that IPC::send() wakes a genuinely BLOCKED destination
// task (bug #014): a task blocked waiting for a reply in send_sync() is made
// READY when a reply arrives.
// Input: Receiver task (prio 11) blocks in IPC::send_sync() (real blocking).
//        The harness sends it a reply message via IPC::send().
// Expect: The receiver transitions from BLOCKED to RUNNING/TERMINATED after
//         the send (its send_sync completes).
JARVIS_TEST(ipc_send_wakes_blocked_destination, "PRE: none | POST: none") {
    // A real peer that receives the request and sends a reply — drives the
    // request phase so the receiver genuinely blocks in send_sync.
    uint64_t peer_id = Scheduler::current_task()->id;

    IpcCtx rctx{};
    uint64_t recv_done = 0;
    rctx.peer_id_ = peer_id;
    rctx.out_ = reinterpret_cast<uint64_t>(&recv_done);
    auto *receiver = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<IpcCtx *>(self->user_data);
            kernel::Message req{};
            req.sender_id = self->id;
            req.type = 42;
            req.priority = 0;
            req.data_size = 0;
            kernel::Message reply{};
            // Block until the harness delivers the reply.
            bool ok = IPC::send_sync(c->peer_id_, req, reply);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_),
                             ok ? 1 : 0, __ATOMIC_RELEASE);
        },
        11, 10);
    JARVIS_ASSERT(receiver != nullptr);
    receiver->user_data = &rctx;
    Scheduler::add_task(*receiver);
    Scheduler::reschedule();

    // Wait until the receiver genuinely blocks in send_sync (reply_wait).
    while (receiver->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT(receiver->reply_wait);

    // The harness is the request peer: deliver the reply to the blocked
    // receiver — the real IPC::send reply-wakeup path.
    kernel::Message reply{};
    reply.sender_id = peer_id;
    reply.type = 99;
    reply.priority = 0;
    reply.data_size = 0;
    JARVIS_ASSERT(IPC::send(receiver->id, reply));

    // The receiver wakes, completes send_sync, and terminates.
    kernel::test::wait_for_termination_safe(receiver);

    // Cleanup BEFORE asserting (cookbook Rule 5): the receiver self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(recv_done == 1);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all IPC unit tests with the test framework.
// Input: None
// Expect: All ipc_* tests are registered via JARVIS_REGISTER_TEST
// Depends: kernel test framework
void register_ipc_tests() {
    Logger::info("Registering IPC tests");

    JARVIS_REGISTER_TEST(ipc_queue_init);
    JARVIS_REGISTER_TEST(ipc_queue_push_pop);
    JARVIS_REGISTER_TEST(ipc_queue_priority_order);
    JARVIS_REGISTER_TEST(ipc_queue_fifo_same_priority);
    JARVIS_REGISTER_TEST(ipc_queue_full);
    JARVIS_REGISTER_TEST(ipc_queue_empty_pop);
    JARVIS_REGISTER_TEST(ipc_queue_wrap_around);
    JARVIS_REGISTER_TEST(ipc_queue_highest_priority);
    JARVIS_REGISTER_TEST(ipc_send_recv_self);
    JARVIS_REGISTER_TEST(ipc_send_nonexistent);
    JARVIS_REGISTER_TEST(ipc_send_nonblock_full);
    JARVIS_REGISTER_TEST(ipc_notify_basic);
    JARVIS_REGISTER_TEST(ipc_notify_try_wait);
    JARVIS_REGISTER_TEST(ipc_eventgroup_set_clear);
    JARVIS_REGISTER_TEST(ipc_eventgroup_try_wait);
    JARVIS_REGISTER_TEST(ipc_block_sender_adds_to_list);
    JARVIS_REGISTER_TEST(ipc_wake_sender_removes_from_list);
    JARVIS_REGISTER_TEST(ipc_wake_sender_terminated);
    JARVIS_REGISTER_TEST(ipc_wake_sender_restores_priority);
    JARVIS_REGISTER_TEST(ipc_send_block_full);
    JARVIS_REGISTER_TEST(ipc_send_sync_roundtrip);
    JARVIS_REGISTER_TEST(ipc_sender_unblocked_on_receiver_exit);
    JARVIS_REGISTER_TEST(ipc_send_wakes_blocked_destination);
}
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
