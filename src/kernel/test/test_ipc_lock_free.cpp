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

/// @file test_ipc_lock_free.cpp
/// @brief IPC lock-free queue tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): every kernel task is a REAL task
/// (prio ≥ 11) dispatched by the real timer ISR that calls IPC::send/recv in
/// its own running context.  The interrupt-flag checks are performed inside
/// the genuinely-running task; the ping-pong throughput runs on real ticks.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/ipc/ipc.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

static volatile uint64_t g_ipc_recv_count_ = 0;

namespace {
void release_task(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    kernel::test::terminate_if_live(t);
}
} // namespace

// Runmode: kernel
// Testidea: Kernel-task sys_receive does not call cli().  A REAL kernel task
// sends a message to itself and receives it; interrupts must remain enabled
// before, during, and after the receive.
// Input: Dispatched kernel task (prio 11) self-sends + self-recvs, sampling
//        the interrupt flag inside its own running context.
// Expect: interrupts enabled before/during/after receive; recv completes.
// Depends: IPC, Scheduler, arch::interrupts_enabled
JARVIS_TEST(ipc_recv_no_cli, "PRE: none | POST: none") {
    static uint64_t g_if_before = 0;
    static uint64_t g_if_during = 0;
    static uint64_t g_if_after = 0;
    static uint64_t g_ok = 0;

    auto *task = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            g_if_before = arch::interrupts_enabled() ? 1 : 0;

            Message msg{};
            msg.sender_id = self->id;
            msg.type = 42;
            msg.priority = 0;
            msg.data_size = 0;
            bool ok = IPC::send(self->id, msg);
            if (!ok)
                return;

            Message out;
            g_if_during = arch::interrupts_enabled() ? 1 : 0;
            ok = IPC::recv(out);
            g_if_after = arch::interrupts_enabled() ? 1 : 0;
            if (ok && out.type == 42)
                g_ok = 1;
        },
        11, 10);
    JARVIS_ASSERT(task != nullptr);
    JARVIS_ASSERT(task->is_user_ == false); // kernel task (MP-1)
    Scheduler::add_task(*task);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(task);
    JARVIS_ASSERT_EQ(1ULL, g_if_before);
    JARVIS_ASSERT_EQ(1ULL, g_if_during);
    JARVIS_ASSERT_EQ(1ULL, g_if_after);
    JARVIS_ASSERT_EQ(1ULL, g_ok);

    release_task(task);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(arch::interrupts_enabled());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Kernel-task send_sync does not call cli().  A REAL kernel sender
// calls send_sync to a REAL kernel receiver that replies; interrupts must
// remain enabled throughout.
// Input: Dispatched sender (prio 12) + receiver (prio 11); the sender calls
//        IPC::send_sync, the receiver replies; both run for real.
// Expect: interrupts enabled before, during, and after send_sync.
// Depends: IPC, Scheduler, arch::interrupts_enabled
JARVIS_TEST(ipc_send_sync_no_cli, "PRE: none | POST: none") {
    static uint64_t g_receiver_id = 0;
    static uint64_t g_if_before = 0;
    static uint64_t g_if_during = 0;
    static uint64_t g_if_after = 0;
    static uint64_t g_reply_ok = 0;

    auto *receiver = TaskControlBlock::create(
        []() {
            Message msg;
            bool ok = false;
            for (int i = 0; i < 100000 && !ok; ++i)
                ok = IPC::recv(msg);
            if (!ok)
                return;
            Message reply;
            reply.sender_id = Scheduler::current_task()->id;
            reply.type = 99;
            reply.priority = 0;
            reply.data_size = 0;
            IPC::send(msg.sender_id, reply);
        },
        11, 10);
    JARVIS_ASSERT(receiver != nullptr);
    g_receiver_id = receiver->id;

    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            g_if_before = arch::interrupts_enabled() ? 1 : 0;
            Message msg;
            msg.sender_id = self->id;
            msg.type = 42;
            msg.priority = 0;
            msg.data_size = 0;
            Message reply;
            g_if_during = arch::interrupts_enabled() ? 1 : 0;
            bool ok = IPC::send_sync(g_receiver_id, msg, reply);
            g_if_after = arch::interrupts_enabled() ? 1 : 0;
            if (ok && reply.type == 99)
                g_reply_ok = 1;
        },
        12, 10);
    JARVIS_ASSERT(sender != nullptr);
    JARVIS_ASSERT(sender->is_user_ == false); // kernel task (MP-1)

    {
        arch::IrqGuard _guard;
        Scheduler::add_task(*sender);
        Scheduler::add_task(*receiver);
    }
    Scheduler::reschedule();

    kernel::test::wait_for_termination_safe(sender);
kernel::test::wait_for_termination_safe(receiver);

    JARVIS_ASSERT_EQ(1ULL, g_if_before);
    JARVIS_ASSERT_EQ(1ULL, g_if_during);
    JARVIS_ASSERT_EQ(1ULL, g_if_after);
    JARVIS_ASSERT_EQ(1ULL, g_reply_ok);

    release_task(sender);
    release_task(receiver);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(arch::interrupts_enabled());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Measure IPC roundtrip throughput with lock-free primitives over
// real timer ticks.  The higher-priority task uses IPC::send_sync (genuine
// blocking — the kernel reply-wait), so the lower-priority peer is always
// dispatched while the sender is blocked; every roundtrip completes through
// genuine dispatch.  Mirrors ipc_send_sync_was_blocked_restores_state.
// Input: 100 IPC roundtrips between two kernel tasks driven by real ticks.
// Expect: All roundtrips complete; no deadlock; both tasks terminate.
// Depends: IPC, Scheduler
JARVIS_TEST(ipc_lock_free_throughput, "PRE: none | POST: none") {
    static uint64_t g_receiver_id = 0;
    static uint64_t g_sender_done = 0;
    static uint64_t g_receiver_done = 0;

    // Receiver (prio 11): blocks genuinely (arch::hlt) until the sender's
    // message arrives — the sender delivers before blocking in send_sync, so
    // hlt is immediately woken; replies to the sender each round.  hlt (not a
    // bounded spin) lets the timer ISR dispatch the sender between rounds.
    auto *receiver = TaskControlBlock::create(
        []() {
            for (uint64_t i = 0; i < 100; ++i) {
                Message msg;
                while (!IPC::recv(msg))
                    arch::hlt();
                Message reply;
                reply.sender_id = Scheduler::current_task()->id;
                reply.type = msg.type + 1;
                reply.priority = 0;
                reply.data_size = 0;
                if (!IPC::send(msg.sender_id, reply))
                    return;
            }
            g_receiver_done = 1;
        },
        11, 10);

    // Sender (prio 12): send_sync blocks the sender each round (kernel
    // reply-wait), so the receiver is dispatched, replies, and the sender
    // resumes — a real ping-pong over real ticks.
    auto *sender = TaskControlBlock::create(
        []() {
            for (uint64_t i = 0; i < 100; ++i) {
                Message msg;
                msg.sender_id = Scheduler::current_task()->id;
                msg.type = static_cast<uint64_t>(i & 0xFF);
                msg.priority = 0;
                msg.data_size = 0;
                Message reply;
                if (!IPC::send_sync(g_receiver_id, msg, reply))
                    return;
                if (reply.type != msg.type + 1)
                    return;
            }
            g_sender_done = 1;
        },
        12, 10);
    JARVIS_ASSERT(sender != nullptr);
    JARVIS_ASSERT(receiver != nullptr);
    g_receiver_id = receiver->id;

    {
        arch::IrqGuard _guard;
        Scheduler::add_task(*receiver);
        Scheduler::add_task(*sender);
    }

    // Yield to the receiver first so next_task() returns the higher-priority
    // sender, which runs, blocks in send_sync, and lets the receiver run.
    kernel::test::yield_as(*receiver);
    __atomic_store_n(&scheduler_need_resched, true, __ATOMIC_RELEASE);

    // Drive the ping-pong on real ticks.
    uint64_t start = arch::Timer::ticks();
    while ((!g_sender_done || !g_receiver_done) &&
           (arch::Timer::ticks() - start) < 5000) {
        arch::hlt();
        __atomic_store_n(&scheduler_need_resched, true, __ATOMIC_RELEASE);
    }

    JARVIS_ASSERT_EQ(1ULL, g_sender_done);

    // Cleanup BEFORE asserting (cookbook Rule 5): both self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, g_receiver_done);
    JARVIS_TEST_PASS();
}

void register_ipc_lock_free_tests() {
    Logger::info("Registering IPC lock-free tests");
    JARVIS_REGISTER_TEST(ipc_recv_no_cli);
    JARVIS_REGISTER_TEST(ipc_send_sync_no_cli);
    JARVIS_REGISTER_TEST(ipc_lock_free_throughput);
}
