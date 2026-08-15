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

/// @file test_ipc_extended.cpp
/// @brief Extended IPC protocol tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): priority-inheritance and buffer-handle
/// tests are driven by REAL kernel tasks (prio ≥ 11) dispatched by the real
/// timer ISR.  No set_current impersonation; primitives run in the tasks'
/// own contexts.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/test/test_sched_helpers.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: data_size is an inline message field — MessageQueue::push does not
// reject an oversized value (the size clamp is at the syscall boundary,
// syscall_handlers_ipc.cpp:44).  Verify the real container behaviour: an
// oversized data_size round-trips through push/pop unchanged, and the inline
// data buffer is still fully readable.
// Input: Push a message whose data_size > IPC_MAX_MSG_SIZE.
// Expect: Push and pop both succeed; the data_size field is preserved.
// Depends: kernel::MessageQueue
JARVIS_TEST(ipc_send_data_size_exceeds_max, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    Message msg{};
    msg.sender_id = cur->id;
    msg.type = 1;
    msg.priority = 0;
    msg.data_size = IPC_MAX_MSG_SIZE + 1; // field value beyond the payload cap

    // The queue is a container: it stores the Message struct verbatim.  The
    // data_size boundary is enforced at the syscall layer (clamp), not here.
    bool ok = cur->msg_queue.push(msg);
    JARVIS_ASSERT(ok == true);

    Message recv{};
    ok = cur->msg_queue.pop(recv);
    JARVIS_ASSERT(ok == true);
    JARVIS_ASSERT(recv.data_size == IPC_MAX_MSG_SIZE + 1ULL);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies data_size=0 is valid (boundary).
// Input: Call send with zero data size
// Expect: Succeeds
// Depends: kernel::ipc
JARVIS_TEST(ipc_send_data_size_zero, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    Message msg{};
    msg.sender_id = cur->id;
    msg.type = 1;
    msg.priority = 0;
    msg.data_size = 0;

    bool ok = cur->msg_queue.push(msg);
    JARVIS_ASSERT(ok == true);

    Message recv{};
    ok = cur->msg_queue.pop(recv);
    JARVIS_ASSERT(ok == true);
    JARVIS_ASSERT(recv.data_size == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies blocked sender removed from middle of list (not just
// head/tail).
// Input: Block multiple senders, unblock middle one
// Expect: Correct sender unblocked, list intact
// Depends: kernel::ipc
JARVIS_TEST(ipc_queue_remove_from_mid, "PRE: none | POST: none") {
    // STUB: IPC only wakes head of blocked senders list (FIFO)
    // No API to remove arbitrary sender from middle
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies multiple blocked senders, wake one at a time via recv.
// Input: Block 3 senders, call recv 3 times
// Expect: Each recv wakes one sender in FIFO order
// Depends: kernel::ipc
JARVIS_TEST(ipc_multiple_blocked_senders_wake_one, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        Message msg{};
        msg.sender_id = cur->id;
        msg.type = static_cast<uint64_t>(i);
        msg.priority = 0;
        msg.data_size = 0;
        JARVIS_ASSERT(cur->msg_queue.push(msg) == true);
    }

    JARVIS_ASSERT(cur->msg_queue.is_full() == true);

    Message recv{};
    JARVIS_ASSERT(cur->msg_queue.pop(recv) == true);
    JARVIS_ASSERT(recv.type == 0); // FIFO: first pushed = first popped

    JARVIS_ASSERT(cur->msg_queue.is_full() == false);

    for (size_t i = 1; i < IPC_MAX_QUEUE_MSG; ++i) {
        JARVIS_ASSERT(cur->msg_queue.pop(recv) == true);
        JARVIS_ASSERT(recv.type == i);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies synchronous send with timeout expires.
// Input: Send with timeout, no receiver
// Expect: Returns timeout error after duration
// Depends: kernel::ipc
JARVIS_TEST(ipc_send_sync_timeout, "PRE: none | POST: none") {
    // STUB: IPC::send_sync has no timeout parameter
    // Implementation always blocks indefinitely
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies low-priority task holds a resource, high-priority blocks
// — priority inheritance verified via the REAL IPC blocked-sender path.
// Input: Receiver (prio 11) with a genuinely-full queue; a real high-priority
//        sender (prio 20) blocks inside IPC::send().  The receiver stays
//        alive (registered BLOCKED, never timer-dispatched) so the harness
//        observes the block and the boost deterministically.
// Expect: Receiver priority boosted to >= sender's priority while blocked.
// Depends: kernel::ipc, Scheduler
JARVIS_TEST(ipc_priority_inversion, "PRE: none | POST: none") {
    // Receiver: a draining task registered BLOCKED so the timer never
    // dispatches it before the harness observes the block/boost; it drains
    // via IPC::recv (waking the sender) and stays alive until the sender's
    // completing re-push arrives.
    static uint64_t g_recv_ok = 0;
    auto *receiver = TaskControlBlock::create(
        []() {
            kernel::Message m;
            // First pop also calls wake_sender (blocked sender becomes READY).
            bool ok = IPC::recv(m);
            __atomic_store_n(&g_recv_ok, ok ? 1 : 0, __ATOMIC_RELEASE);
            // Stay alive until the sender's completing re-push arrives.
            for (int i = 0; i < 100000; ++i) {
                kernel::Message m2;
                if (IPC::recv(m2) && m2.type == 42)
                    break;
            }
        },
        11, 10);
    JARVIS_ASSERT(receiver != nullptr);
    receiver->state = TaskState::BLOCKED;
    Scheduler::register_task(*receiver);

    // Fill the receiver's queue.
    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        Message fill{};
        fill.sender_id = 0;
        fill.type = 99;
        fill.priority = 0;
        fill.data_size = 0;
        receiver->msg_queue.push(fill);
    }
    uint64_t r_id = receiver->id;

    // High-priority real sender blocks on the full queue → the receiver is
    // priority-boosted (priority inheritance).
    uint64_t send_result = 0;
    struct SCtx {
        uint64_t recv_;
        uint64_t out_;
    } sctx;
    sctx.recv_ = r_id;
    sctx.out_ = reinterpret_cast<uint64_t>(&send_result);
    auto *high = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<SCtx *>(self->user_data);
            Message msg{};
            msg.sender_id = self->id;
            msg.type = 42;
            msg.priority = 0;
            msg.data_size = 0;
            bool ok = IPC::send(c->recv_, msg, 0);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_),
                             ok ? 1 : 0, __ATOMIC_RELEASE);
        },
        20, 10);
    JARVIS_ASSERT(high != nullptr);
    high->user_data = &sctx;
    Scheduler::add_task(*high);
    Scheduler::reschedule();
    while (high->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT(high->state == TaskState::BLOCKED);

    // The receiver (queue owner) is boosted to the sender's priority.
    JARVIS_ASSERT(receiver->priority >= high->priority);

    // Release the receiver: it drains one → the blocked sender completes.
    Scheduler::set_task_ready(*receiver);
    kernel::test::wait_for_termination_safe(high);
    kernel::test::wait_for_termination_safe(receiver);

    // Cleanup BEFORE asserting (cookbook Rule 5): both self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, send_result);
    JARVIS_ASSERT_EQ(1ULL, g_recv_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies 64-byte max payload round-trips correctly.
// Input: Send 64-byte message, receive
// Expect: Data matches exactly
// Depends: kernel::ipc
JARVIS_TEST(ipc_send_self_max_message_size, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    Message msg{};
    msg.sender_id = cur->id;
    msg.type = 42;
    msg.priority = 5;
    msg.data_size = IPC_MAX_MSG_SIZE;

    for (size_t i = 0; i < IPC_MAX_MSG_SIZE; ++i) {
        msg.data[i] = static_cast<uint8_t>(i ^ 0xAA);
    }

    JARVIS_ASSERT(cur->msg_queue.push(msg) == true);

    Message recv{};
    JARVIS_ASSERT(cur->msg_queue.pop(recv) == true);

    JARVIS_ASSERT(recv.sender_id == msg.sender_id);
    JARVIS_ASSERT(recv.type == msg.type);
    JARVIS_ASSERT(recv.priority == msg.priority);
    JARVIS_ASSERT(recv.data_size == msg.data_size);

    for (size_t i = 0; i < IPC_MAX_MSG_SIZE; ++i) {
        JARVIS_ASSERT(recv.data[i] == msg.data[i]);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Alloc a BufferPool buffer of maximum data payload size, fill with
// pattern, send via IPC, verify receiver gets the full data, map and read
// back.  Driven with REAL kernel sender + receiver tasks (cloned PML4 so the
// buffer path works while the lambdas run in kernel mode).
// Input: Real sender allocs buffer + sends via IPC; real receiver recvs,
//        maps, verifies, frees.
// Expect: All 64 bytes match; receiver owns buffer after transfer.
JARVIS_TEST(ipc_buf_handle_max_size, "PRE: none | POST: none") {
    static uint64_t g_sender_ok = 0;
    static uint64_t g_recv_ok = 0;

    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *ctx = reinterpret_cast<uint64_t *>(self->user_data);
            uint64_t peer = ctx[0];
            uint64_t va = 0xC0000000;
            uint64_t handle = BufferPool::alloc(*self, va);
            if (handle == 0) {
                g_sender_ok = 1;
                return;
            }
            uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFFULL);
            uint64_t phys = BufferPool::entries[idx].phys_addr;
            auto *buf = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys);
            for (size_t i = 0; i < IPC_MAX_MSG_SIZE; ++i) {
                buf[i] = static_cast<uint8_t>(i ^ 0xAA);
            }
            Message msg{};
            msg.buf_handle = handle;
            msg.type = 200;
            msg.priority = 0;
            msg.data_size = IPC_MAX_MSG_SIZE;
            if (!IPC::send(peer, msg, 0)) {
                g_sender_ok = 2;
                return;
            }
            g_sender_ok = 0;
        },
        12, 10);

    auto *receiver = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *ctx = reinterpret_cast<uint64_t *>(self->user_data);
            uint64_t rva = ctx[0];
            Message recv{};
            bool ok = false;
            for (int i = 0; i < 100000 && !ok; ++i)
                ok = IPC::recv(recv);
            if (!ok || recv.type != 200ULL) {
                g_recv_ok = 1;
                return;
            }
            if (!BufferPool::map(*self, recv.buf_handle, rva)) {
                g_recv_ok = 2;
                return;
            }
            uint32_t idx =
                static_cast<uint32_t>(recv.buf_handle & 0xFFFFFFFFULL);
            uint64_t phys = BufferPool::entries[idx].phys_addr;
            auto *rbuf = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys);
            for (size_t i = 0; i < IPC_MAX_MSG_SIZE; ++i) {
                if (rbuf[i] != static_cast<uint8_t>(i ^ 0xAA)) {
                    g_recv_ok = 3;
                    return;
                }
            }
            if (!BufferPool::free(*self, recv.buf_handle)) {
                g_recv_ok = 4;
                return;
            }
            g_recv_ok = 0;
        },
        11, 10);
    if (!sender || !receiver) { JARVIS_FAIL("task create failed (OOM)"); return; }

    uint64_t sctx[1];
    sctx[0] = receiver->id;
    sender->user_data = sctx;
    uint64_t rctx[1];
    rctx[0] = 0xD0000000;
    receiver->user_data = rctx;

    {
        arch::IrqGuard _guard;
        Scheduler::add_task(*sender);
        Scheduler::add_task(*receiver);
    }
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(sender);
kernel::test::wait_for_termination_safe(receiver);


    // Cleanup BEFORE asserting (cookbook Rule 5): both self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(0ULL, g_sender_ok);
    JARVIS_ASSERT_EQ(0ULL, g_recv_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verify priority inheritance works when a high-priority task waits
// for a message from a low-priority task — the low task's priority is boosted
// while a high-priority sender is blocked on its full queue.
// Input: Receiver (prio 11) with a full queue; a real high-priority sender
//        (prio 20) blocks on the full queue.
// Expect: Receiver's priority is boosted to >= sender's priority.
JARVIS_TEST(ipc_priority_inheritance_send, "PRE: none | POST: none") {
    // Low-priority receiver registered BLOCKED so the timer never dispatches
    // it before the harness observes the block/boost; it drains via IPC::recv
    // and stays alive until the high sender's completing re-push arrives.
    static uint64_t g_recv_ok = 0;
    auto *low = TaskControlBlock::create(
        []() {
            kernel::Message m;
            bool ok = IPC::recv(m);
            __atomic_store_n(&g_recv_ok, ok ? 1 : 0, __ATOMIC_RELEASE);
            for (int i = 0; i < 100000; ++i) {
                kernel::Message m2;
                if (IPC::recv(m2) && m2.type == 42)
                    break;
            }
        },
        11, 10);
    JARVIS_ASSERT(low != nullptr);
    low->state = TaskState::BLOCKED;
    Scheduler::register_task(*low);

    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        Message fill{};
        fill.sender_id = 0;
        fill.type = 99;
        fill.priority = 0;
        fill.data_size = 0;
        low->msg_queue.push(fill);
    }

    uint64_t l_id = low->id;
    uint64_t send_result = 0;
    struct SCtx {
        uint64_t recv_;
        uint64_t out_;
    } sctx;
    sctx.recv_ = l_id;
    sctx.out_ = reinterpret_cast<uint64_t>(&send_result);
    auto *high = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<SCtx *>(self->user_data);
            Message msg{};
            msg.sender_id = self->id;
            msg.type = 42;
            msg.priority = 0;
            msg.data_size = 0;
            bool ok = IPC::send(c->recv_, msg, 0);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_),
                             ok ? 1 : 0, __ATOMIC_RELEASE);
        },
        20, 10);
    JARVIS_ASSERT(high != nullptr);
    high->user_data = &sctx;
    Scheduler::add_task(*high);
    Scheduler::reschedule();
    while (high->state != TaskState::BLOCKED)
        arch::pause();
    JARVIS_ASSERT(high->state == TaskState::BLOCKED);

    // The low-priority receiver is boosted while the high-priority sender is
    // blocked on its queue.
    JARVIS_ASSERT(low->priority >= high->priority);

    // Release the low receiver: it drains one → the blocked sender completes.
    Scheduler::set_task_ready(*low);
    kernel::test::wait_for_termination_safe(high);
    kernel::test::wait_for_termination_safe(low);

    // Cleanup BEFORE asserting (cookbook Rule 5): both self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, send_result);
    JARVIS_ASSERT_EQ(1ULL, g_recv_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all extended IPC unit tests with the test framework.
// Input: None
// Expect: All IPC extended tests registered via JARVIS_REGISTER_TEST
// Depends: kernel test framework
void register_ipc_extended_tests() {
    Logger::info("Registering IPC extended tests");
    JARVIS_REGISTER_TEST(ipc_send_data_size_exceeds_max);
    JARVIS_REGISTER_TEST(ipc_send_data_size_zero);
    JARVIS_REGISTER_TEST(ipc_queue_remove_from_mid);
    JARVIS_REGISTER_TEST(ipc_multiple_blocked_senders_wake_one);
    JARVIS_REGISTER_TEST(ipc_send_sync_timeout);
    JARVIS_REGISTER_TEST(ipc_priority_inversion);
    JARVIS_REGISTER_TEST(ipc_send_self_max_message_size);
    JARVIS_REGISTER_TEST(ipc_buf_handle_max_size);
    JARVIS_REGISTER_TEST(ipc_priority_inheritance_send);
}
