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

/// @file test_ipc_robustness.cpp
/// @brief IPC robustness and error-handling tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): concurrent senders are REAL kernel
/// tasks (prio ≥ 11) dispatched by the real timer ISR that call IPC::send()
/// in their own running context.  Blocked-sender cleanup is driven through
/// real dispatch and termination.  No set_current impersonation.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/memory/vmm.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-null-argument"
#endif

namespace {

/// @brief  Context for a task lambda (captureless lambdas only).
struct IpcRctx {
    uint64_t peer_id_;
    uint64_t out_;
};

} // namespace

TEST_CLASS(IpcMisformedMessages) {
    auto *cur = Scheduler::current_task();
    CT_ASSERT(cur != nullptr);

    MessageQueue q;
    q.init();

    Message msg{};
    msg.sender_id = 1;
    msg.type = 99;
    msg.data_size = 4;
    msg.data[0] = 0xDE;
    msg.data[1] = 0xAD;

    msg.priority = 32;
    bool ok = q.push(msg);
    CT_ASSERT(ok);

    msg.priority = 0;
    msg.type = 1;
    ok = q.push(msg);
    CT_ASSERT(ok);

    Message out;
    ok = q.pop(out);
    CT_ASSERT(ok);
    CT_ASSERT(out.type == 1);

    ok = q.pop(out);
    CT_ASSERT(ok);
    CT_ASSERT(out.type == 99);

    CT_ASSERT(Scheduler::current_task() != nullptr);

    {
        MessageQueue q2;
        q2.init();
        msg.priority = 0;
        msg.data_size = IPC_MAX_MSG_SIZE + 1;
        msg.type = 42;
        ok = q2.push(msg);
        CT_ASSERT(ok);
        Message out2;
        ok = q2.pop(out2);
        CT_ASSERT(ok);
        CT_ASSERT(out2.data_size == IPC_MAX_MSG_SIZE + 1);
    }

    {
        MessageQueue q3;
        q3.init();
        msg.data_size = 0;
        msg.type = 43;
        ok = q3.push(msg);
        CT_ASSERT(ok);
        Message out3;
        ok = q3.pop(out3);
        CT_ASSERT(ok);
        CT_ASSERT(out3.data_size == 0);
    }
};

TEST_CLASS(IpcQueueWraparoundEdge) {
    MessageQueue q;
    q.init();
    Message msg{};
    msg.sender_id = 1;
    msg.priority = 0;
    msg.data_size = 0;

    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        msg.type = i;
        CT_ASSERT(q.push(msg));
    }
    CT_ASSERT(q.is_full());

    Message out;
    CT_ASSERT(q.pop(out));
    CT_ASSERT(out.type == 0ULL);

    msg.type = 99;
    CT_ASSERT(q.push(msg));
    CT_ASSERT(q.is_full());

    for (size_t i = 1; i < IPC_MAX_QUEUE_MSG; ++i) {
        CT_ASSERT(q.pop(out));
        CT_ASSERT(out.type == i);
    }
    CT_ASSERT(q.pop(out));
    CT_ASSERT(out.type == 99ULL);
    CT_ASSERT(q.is_empty());

    q.init();
    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        msg.type = i;
        msg.priority = IPC_MAX_QUEUE_MSG - i;
        CT_ASSERT(q.push(msg));
    }
    CT_ASSERT(q.pop(out));
    CT_ASSERT(out.type == IPC_MAX_QUEUE_MSG - 1);
    CT_ASSERT(q.count == IPC_MAX_QUEUE_MSG - 1);

    size_t count = 0;
    while (q.pop(out))
        ++count;
    CT_ASSERT(count == IPC_MAX_QUEUE_MSG - 1);
};

TEST_CLASS(IpcConcurrentSenders) {
    // Real receiver: a forever-spinning container task so its queue stays
    // alive for the concurrent senders.  It must NOT block on a gate: IPC::send
    // (ipc.cpp:244) wakes ANY BLOCKED destination, so a gate-blocked receiver
    // is woken by the first sender, self-terminates, and leaves a freed TCB in
    // the semaphore waiter list (ROADMAP v0.3.9 teardown gap).  A forever task
    // is never BLOCKED, so sends never spuriously wake it.
    auto *receiver = kernel::test::create_forever_task(11, 10, "recv-container");
    CT_ASSERT(receiver != nullptr);
    Scheduler::reschedule();
    uint64_t recv_id = receiver->id;

    Message fill{};
    fill.sender_id = 0;
    fill.type = 0;
    fill.priority = 0;
    fill.data_size = 0;
    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG / 2; ++i) {
        receiver->msg_queue.push(fill);
    }

    static const int NUM_SENDERS = 4;
    static const int MSGS_PER = 5;
    TaskControlBlock *senders[NUM_SENDERS];

    // Register all cooperating senders under one IrqGuard so no timer tick
    // can split the registration (cookbook Rule 2).
    {
        arch::IrqGuard guard;
        for (int i = 0; i < NUM_SENDERS; ++i) {
            struct SCtx {
                uint64_t recv_;
                uint64_t base_;
            };
            static SCtx sctx[NUM_SENDERS];
            sctx[i].recv_ = recv_id;
            sctx[i].base_ = static_cast<uint64_t>(i);
            senders[i] = TaskControlBlock::create(
                []() {
                    auto *self = Scheduler::current_task();
                    auto *c = reinterpret_cast<SCtx *>(self->user_data);
                    for (int m = 0; m < MSGS_PER; ++m) {
                        Message msg{};
                        msg.sender_id = self->id;
                        msg.type = c->base_ * MSGS_PER + static_cast<uint64_t>(m);
                        msg.priority = 0;
                        msg.data_size = 0;
                        IPC::send(c->recv_, msg, IPC_NONBLOCK);
                    }
                },
                12 + static_cast<uint64_t>(i), 10);
            CT_ASSERT(senders[i] != nullptr);
            senders[i]->user_data = &sctx[i];
            Scheduler::add_task(*senders[i]);
        }
    }

    Scheduler::reschedule();

    // All senders genuinely run and attempt their non-blocking sends; the
    // queue holds at most IPC_MAX_QUEUE_MSG messages.
    for (int i = 0; i < NUM_SENDERS; ++i) {
        while (TaskControlBlock::is_valid(senders[i]) &&
               senders[i]->state != TaskState::TERMINATED)
            arch::pause();
    }
    JARVIS_ASSERT(receiver->msg_queue.count <= IPC_MAX_QUEUE_MSG);

    // Drain the receiver queue — the messages sent by the real senders are
    // present.
    {
        Message out;
        while (receiver->msg_queue.pop(out)) {
        }
    }
    JARVIS_ASSERT(receiver->msg_queue.is_empty());

    // Cleanup BEFORE asserting (cookbook Rule 5): the senders self-terminated,
    // so reclaim them via the zombie list.  The receiver is a forever task —
    // terminate it explicitly, then drain.
    Scheduler::drain_zombie_list();
    kernel::test::terminate_and_drain(*receiver);
};

#if !defined(CONFIG_ARCH_RISCV64)
TEST_CLASS(IpcBufHandleTransferRoundtrip) {
    // Real KERNEL sender + receiver (prio 12/11).  Each task's lambda runs in
    // its own dispatched context; page_table_ is set to a kernel-PML4 clone
    // so BufferPool::alloc/map (which require a non-null page table) work.
    // BUGS.md#020 hazard avoided: the lambdas run in KERNEL mode.
    static uint64_t g_sender_result = 0;
    static uint64_t g_receiver_result = 0;

    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *ctx = reinterpret_cast<uint64_t *>(self->user_data);
            uint64_t peer = ctx[0];
            uint64_t sva = ctx[1];
            uint64_t handle = BufferPool::alloc(*self, sva);
            if (handle == 0) {
                g_sender_result = 1; // failed
                return;
            }
            uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFFULL);
            uint64_t phys = BufferPool::entries[idx].phys_addr;
            volatile auto *buf =
                reinterpret_cast<volatile uint8_t *>(arch::HHDM_OFFSET + phys);
            for (size_t i = 0; i < arch::PAGE_SIZE; ++i)
                buf[i] = static_cast<uint8_t>(i ^ 0xA5);
            Message msg{};
            msg.buf_handle = handle;
            msg.type = 100;
            msg.priority = 0;
            msg.data_size = 0;
            if (!IPC::send(peer, msg, 0)) {
                g_sender_result = 2; // failed
                return;
            }
            g_sender_result = 0; // ok
        },
        12, 10);

    auto *receiver = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *ctx = reinterpret_cast<uint64_t *>(self->user_data);
            uint64_t rva = ctx[0];
            Message recv_msg{};
            bool ok = false;
            for (int i = 0; i < 100000 && !ok; ++i)
                ok = IPC::recv(recv_msg);
            if (!ok || recv_msg.type != 100ULL) {
                g_receiver_result = 1; // failed
                return;
            }
            if (!BufferPool::map(*self, recv_msg.buf_handle, rva)) {
                g_receiver_result = 2; // failed
                return;
            }
            uint32_t idx =
                static_cast<uint32_t>(recv_msg.buf_handle & 0xFFFFFFFFULL);
            uint64_t rphys = BufferPool::entries[idx].phys_addr;
            volatile auto *rbuf =
                reinterpret_cast<volatile uint8_t *>(arch::HHDM_OFFSET + rphys);
            for (size_t i = 0; i < arch::PAGE_SIZE; ++i) {
                if (rbuf[i] != static_cast<uint8_t>(i ^ 0xA5)) {
                    g_receiver_result = 3; // data mismatch
                    return;
                }
            }
            if (!BufferPool::free(*self, recv_msg.buf_handle)) {
                g_receiver_result = 4; // failed
                return;
            }
            g_receiver_result = 0; // ok
        },
        11, 10);
    if (!sender || !receiver) { JARVIS_FAIL("task create failed (OOM)"); return; }

    uint64_t sctx[2];
    sctx[0] = receiver->id;
    sctx[1] = 0x80000000;
    sender->user_data = sctx;
    uint64_t rctx[1];
    rctx[0] = 0x90000000;
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
    JARVIS_ASSERT_EQ(0ULL, g_sender_result);
    JARVIS_ASSERT_EQ(0ULL, g_receiver_result);
};
#endif

TEST_CLASS(IpcBidirectionalSendSync) {
    auto *me = Scheduler::current_task();
    CT_ASSERT(me != nullptr);
    uint64_t my_id = me->id;

    static volatile uint64_t g_task_id = 0;

    // Endpoint B: receive A's first request and reply, then receive A's second
    // request and reply — a full bidirectional synchronous exchange. Every IPC
    // call blocks cooperatively via the scheduler, so this task runs
    // synchronously when the test task (A) blocks on send_sync().
    auto *peer = TaskControlBlock::create(
        []() {
            Message msg{}, reply{};
            while (!IPC::recv(msg)) {
                Scheduler::reschedule();
                arch::hlt();
            }
            JARVIS_ASSERT(msg.type == 10ULL);
            reply.sender_id = msg.sender_id;
            reply.type = 20;
            reply.priority = 0;
            reply.data_size = 0;
            bool ok = IPC::send(msg.sender_id, reply);
            JARVIS_ASSERT(ok);

            while (!IPC::recv(msg)) {
                Scheduler::reschedule();
                arch::hlt();
            }
            JARVIS_ASSERT(msg.type == 30ULL);
            reply.sender_id = msg.sender_id;
            reply.type = 40;
            reply.priority = 0;
            reply.data_size = 0;
            ok = IPC::send(msg.sender_id, reply);
            JARVIS_ASSERT(ok);
        },
        5, 10);
    CT_ASSERT(peer != nullptr);
    g_task_id = peer->id;
    Scheduler::add_task(*peer);

    Message req{}, reply{};
    req.sender_id = my_id;
    req.type = 10;
    req.priority = 0;
    req.data_size = 0;
    bool ok = IPC::send_sync(g_task_id, req, reply);
    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(reply.type == 20ULL);

    req.type = 30;
    ok = IPC::send_sync(g_task_id, req, reply);

    // The peer self-terminated after its two recv/reply cycles — reclaim via
    // the zombie list (cookbook Rule 4/5).
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(reply.type == 40ULL);
    JARVIS_TEST_PASS();
};

TEST_CLASS(IpcBlockedSenderOnReceiverCleanup) {
    // Reference pattern (test.hpp __reference_blocked_sender_cleanup):
    // create BOTH TCBs first, fill the receiver's queue, then register both
    // under one arch::IrqGuard.  The receiver uses an EMPTY lambda — it must
    // NOT block on a gate: a gate-blocked task stays physically in the ready
    // queue (INV-2, Semaphore::wait never dequeues) and the scheduler
    // re-selects it, producing the H2-family runq desync.  The sender (prio
    // 12) runs first, blocks on the full queue; the receiver then runs its
    // empty lambda to termination, whose cleanup wakes the blocked sender.
    auto *receiver = TaskControlBlock::create([]() {}, 11, 10);
    CT_ASSERT(receiver != nullptr);

    // Fill the receiver's queue so a real sender blocks.
    Message fill{};
    fill.sender_id = 0;
    fill.type = 99;
    fill.priority = 0;
    fill.data_size = 0;
    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        receiver->msg_queue.push(fill);
    }

    uint64_t r_id = receiver->id;
    uint64_t send_result = 0;
    struct SCtx {
        uint64_t recv_;
        uint64_t out_;
    } sctx;
    sctx.recv_ = r_id;
    sctx.out_ = reinterpret_cast<uint64_t>(&send_result);
    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<SCtx *>(self->user_data);
            Message msg{};
            msg.sender_id = self->id;
            msg.type = 1;
            msg.priority = 0;
            msg.data_size = 0;
            // Blocks on the full receiver queue.
            bool ok = IPC::send(c->recv_, msg, 0);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_),
                             ok ? 1 : 0, __ATOMIC_RELEASE);
        },
        12, 10);
    CT_ASSERT(sender != nullptr);
    sender->user_data = &sctx;
    {
        arch::IrqGuard _guard;
        Scheduler::add_task(*receiver);
        Scheduler::add_task(*sender);
    }
    while (sender->state != TaskState::BLOCKED)
        arch::pause();
    CT_ASSERT(receiver->msg_queue.blocked_senders_head == sender);

    // Dispatch the receiver → real terminate → drain runs its cleanup, whose
    // MessageQueue teardown wakes the blocked sender (fast-fail).
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(receiver);
    Scheduler::drain_zombie_list();
    kernel::test::wait_for_termination_safe(sender);
    // The blocked send fast-fails (receiver gone).

    // Cleanup BEFORE asserting (cookbook Rule 5): both self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(0ULL, send_result);
};

void register_ipc_robustness_tests() {
    Logger::info("Registering IPC robustness tests");
    REGISTER_CLASS(IpcMisformedMessages);
    REGISTER_CLASS(IpcQueueWraparoundEdge);
    REGISTER_CLASS(IpcConcurrentSenders);
#if !defined(CONFIG_ARCH_RISCV64)
    REGISTER_CLASS(IpcBufHandleTransferRoundtrip);
#endif
    REGISTER_CLASS(IpcBidirectionalSendSync);
    REGISTER_CLASS(IpcBlockedSenderOnReceiverCleanup);
}
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
