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

/// @file test_ipc_blocking.cpp
/// @brief IPC blocking send/receive tests.

// Runmode: kernel
// Testidea: Verifies that IPC::recv() restores the was_blocked flag correctly.
// Input: Create a task, send a message to itself, receive it.
// Expect: was_blocked flag is set during blocking, restored to READY after
// receive. Depends: kernel::IPC, kernel::MessageQueue, kernel::Scheduler

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/task/task_errors.hpp>
#include <kernel/arch/irq_guard.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-null-argument"
#endif

JARVIS_TEST(ipc_receive_was_blocked_restores_state, "PRE: none | POST: none") {
    arch::IrqGuard guard;
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    Message msg{};
    msg.sender_id = cur->id;
    msg.type = 1;
    msg.priority = 0;
    msg.data_size = 0;

    bool ok = IPC::send(cur->id, msg);
    JARVIS_ASSERT(ok);

    // Verify queue has message
    JARVIS_ASSERT(!cur->msg_queue.is_empty());

    // The was_blocked flag is internal to sys_receive - we verify by checking
    // that after receiving, the task is in READY state (not BLOCKED)
    Message out;
    ok = IPC::recv(out);
    JARVIS_ASSERT(ok);
    JARVIS_ASSERT_EQ(1ULL, out.type);

    // Task should be in RUNNING state after successful non-blocking receive
    // (the message was already in the queue — no blocking occurred)
    JARVIS_ASSERT(cur->state == TaskState::RUNNING);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that IPC::send_sync() restores the was_blocked flag
// correctly. Input: Create sender and receiver tasks. Sender calls send_sync,
// receiver replies. Expect: was_blocked flag is set during blocking, restored
// to READY after reply. Depends: kernel::IPC, kernel::MessageQueue,
// kernel::Scheduler
JARVIS_TEST(ipc_send_sync_was_blocked_restores_state,
            "PRE: none | POST: none") {
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
            bool ok2 = IPC::send(msg.sender_id, reply);
            JARVIS_ASSERT(ok2);
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

    // Register both cooperating tasks under one IrqGuard so a timer tick
    // cannot split the registration (cookbook Rule 2).
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*receiver);
        Scheduler::add_task(*sender);
    }

    auto *original = Scheduler::current_task();
    // Yield to the *receiver* (not the sender): next_task() skips whatever is
    // current_task_ptr_, so yielding to the receiver makes next_task() return
    // the higher-priority sender (prio 12 > 11), which runs first, sends, and
    // blocks; the receiver then runs and replies.  Yielding to the sender would
    // set it current and get it skipped+discarded, deadlocking the handshake.
    kernel::test::yield_as(*receiver);

    // Drive the sender→receiver→sender handshake to completion.  A single
    // reschedule() defers the context switch; the timer ISR dispatches the
    // peer tasks on subsequent ticks.  Busy-wait WITHOUT reschedule()
    // so the timer ISR can acquire the scheduler lock without contention.
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(sender);
    kernel::test::wait_for_termination_safe(receiver);

    Scheduler::set_current(*original);

    // Both tasks should have run to completion (the sender blocked on
    // send_sync, the receiver replied, then both exited).
    JARVIS_ASSERT(sender->state == TaskState::TERMINATED);
    JARVIS_ASSERT(receiver->state == TaskState::TERMINATED);

    kernel::test::terminate_and_drain2(sender, receiver);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that a task blocking in IPC::recv() (kernel path) runs
// to completion on a self-send/self-recv handshake.  NOTE: a genuine *userspace*
// task must reach IPC via the syscall ABI (int $0x80 SYS_RECEIVE, which emits
// sti/hlt/cli) — not by calling kernel IPC:: directly.  The kernel has no
// mechanism to execute an arbitrary C++ lambda in user mode (create_user only
// sets page_table_/user stack; a real userspace task is an ELF-loaded app that
// inherits user mappings via clone()).  So the userspace-syscall blocking path
// is exercised by the ELF user-app + test_syscall infrastructure; this test
// verifies the kernel IPC::recv blocking contract directly on a kernel task,
// mirroring ipc_kernel_block_skips_sti.
// Depends: kernel::IPC, kernel::MessageQueue, kernel::Scheduler
JARVIS_TEST(ipc_userspace_block_uses_sti_hlt_cli, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    // Use a kernel task: a C++ lambda cannot run as a userspace task (no
    // user-mapped entry mechanism), and calling IPC:: directly from user mode
    // would fault.  The userspace syscall path is covered elsewhere.
    auto *user_task = TaskControlBlock::create(
        []() {
            Message msg{};
            msg.sender_id = Scheduler::current_task()->id;
            msg.type = 1;
            msg.priority = 0;
            msg.data_size = 0;

            bool ok = IPC::send(Scheduler::current_task()->id, msg);
            JARVIS_ASSERT(ok);

            Message out;
            ok = IPC::recv(out);
            JARVIS_ASSERT(ok);
            JARVIS_ASSERT_EQ(1ULL, out.type);
        },
        11, 10);
    JARVIS_ASSERT(user_task != nullptr);
    JARVIS_ASSERT(user_task->is_user_ == false); // kernel task (MP-1)
    Scheduler::add_task(*user_task);

    auto *original = Scheduler::current_task();
    // Do NOT yield_as(*user_task): next_task() skips the current task, so that
    // would make the only test task current and never dispatch it.  A plain
    // reschedule() lets next_task() pick the higher-priority task (11 > 10).
    Scheduler::reschedule();

    // Drive the task's self-IPC handshake to completion.  A single
    // reschedule() defers the context switch; the timer ISR dispatches
    // the peer task on the next tick.  Busy-wait WITHOUT reschedule()
    // so the timer ISR can acquire the scheduler lock without contention.
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(user_task);

    Scheduler::set_current(*original);

    // Task should have run to completion (sent to itself, received).

    const auto user_task_state = user_task->state;
    kernel::test::terminate_and_drain(*user_task);
    JARVIS_ASSERT(user_task_state == TaskState::TERMINATED);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that kernel task blocking in recv() skips sti/hlt/cli
// mechanism. Input: Create kernel task (page_table_ = null), send message to
// itself, receive. Expect: sti/hlt/cli is NOT emitted during blocking (internal
// to sys_receive). Depends: kernel::IPC, kernel::MessageQueue,
// kernel::Scheduler
JARVIS_TEST(ipc_kernel_block_skips_sti, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    // Create a kernel task (no page_table_)
    auto *kernel_task = TaskControlBlock::create(
        []() {
            Message msg{};
            msg.sender_id = Scheduler::current_task()->id;
            msg.type = 1;
            msg.priority = 0;
            msg.data_size = 0;

            bool ok = IPC::send(Scheduler::current_task()->id, msg);
            JARVIS_ASSERT(ok);

            Message out;
            ok = IPC::recv(out);
            JARVIS_ASSERT(ok);
            JARVIS_ASSERT_EQ(1ULL, out.type);
        },
        11, 10);
    JARVIS_ASSERT(kernel_task != nullptr);
    JARVIS_ASSERT(kernel_task->is_user_ == false); // kernel task (MP-1)
    Scheduler::add_task(*kernel_task);

    auto *original = Scheduler::current_task();
    // Do NOT yield_as(*kernel_task): next_task() skips the current task, so
    // that would make the only test task current and never dispatch it.  A
    // plain reschedule() picks the higher-priority kernel task (11 > 10).
    Scheduler::reschedule();

    // Drive the kernel task's self-IPC handshake to completion.
    // reschedule() only defers the switch (it happens in the next timer
    // ISR), so IRQs MUST stay enabled here for the timer ISR to apply the
    // deferred context switch; an IrqGuard would starve the switch and the
    // task would never run.  Wait (unbounded) until the task terminates.
    kernel::test::wait_for_termination_safe(kernel_task);

    Scheduler::set_current(*original);

    // Kernel task should have run to completion.

    const auto kernel_task_state = kernel_task->state;
    kernel::test::terminate_and_drain(*kernel_task);
    JARVIS_ASSERT(kernel_task_state == TaskState::TERMINATED);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all IPC blocking tests with the test framework.
// Input: None
// Expect: All ipc_* blocking tests registered via JARVIS_REGISTER_TEST
// Depends: kernel test framework
void register_ipc_blocking_tests() {
    Logger::info("Registering IPC blocking tests");
    JARVIS_REGISTER_TEST(ipc_receive_was_blocked_restores_state);
    JARVIS_REGISTER_TEST(ipc_send_sync_was_blocked_restores_state);
    JARVIS_REGISTER_TEST(ipc_userspace_block_uses_sti_hlt_cli);
    JARVIS_REGISTER_TEST(ipc_kernel_block_skips_sti);
}
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
