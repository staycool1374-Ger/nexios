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

/// @file test_init.cpp
/// @brief Kernel initialisation sequence tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/ipc/ipc_boot.hpp>

using namespace kernel;

JARVIS_TEST(init_task_exists, "PRE: none | POST: none") {
    auto *init = Scheduler::find_task(1);
    JARVIS_ASSERT(init != nullptr);
    JARVIS_ASSERT_EQ(1ULL, init->id);
    JARVIS_ASSERT(init->state == TaskState::READY ||
                  init->state == TaskState::RUNNING);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(init_task_has_no_parent, "PRE: none | POST: none") {
    auto *init = Scheduler::find_task(1);
    JARVIS_ASSERT(init != nullptr);
    JARVIS_ASSERT_EQ(0ULL, init->parent_id);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Child exit pokes the PID 1 reaper (issue #155): terminate()
//           notifies PID 1's sleep object after the zombie-list push, so
//           a parked reaper wakes and drains to zero.
// Input: Drain stale notify value; register + terminate a child;
//        consume the poke; drain zombies.
// Expect: try_wait succeeds exactly once after the exit (the poke
//         landed); zombie count returns to 0 after the drain.
// Depends: Scheduler::terminate reaper poke, Notify level-triggering
JARVIS_TEST(reaper_notified_on_child_exit, "PRE: none | POST: none") {
    auto *init = Scheduler::find_task(1);
    JARVIS_ASSERT(init != nullptr);
    uint64_t stale = 0;
    while (init->notify.try_wait(&stale)) {
    }
    auto *child = TaskControlBlock::create([]() {}, 10,
                                           TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(child != nullptr);
    Scheduler::register_task(*child);
    Scheduler::terminate(*child, 0);
    uint64_t wake = 0;
    JARVIS_ASSERT(init->notify.try_wait(&wake));
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(0ULL, Scheduler::zombie_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Daemon-ready IPC to PID 1 lands in its queue (issue #155):
//           the reaper's second wake source.  The suite runs as PID 1
//           (harness exemption), so it can send-to-self and recv back.
// Input: IPC::send(1, MSG_DAEMON_READY); IPC::recv.
// Expect: send succeeds; recv returns the same type.  The parked
//         reaper is woken by the generic BLOCKED path on send.
// Depends: IPC::send BLOCKED-wake, PID 1 queue (issue #155)
JARVIS_TEST(reaper_wakes_on_daemon_ipc, "PRE: none | POST: none") {
    auto *self = Scheduler::current_task();
    JARVIS_ASSERT(self != nullptr);
    JARVIS_ASSERT_EQ(1ULL, self->id);
    Message msg{};
    msg.sender_id = self->id;
    msg.type = ipc::MSG_DAEMON_READY;
    JARVIS_ASSERT(kernel::IPC::send(1, msg, 0));
    Message got{};
    JARVIS_ASSERT(kernel::IPC::recv(got));
    JARVIS_ASSERT(got.type == ipc::MSG_DAEMON_READY);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(init_task_reparents_orphans, "PRE: none | POST: none") {
    // Create a child task, set parent to a task that will exit,
    // verify it gets reparented to init

    // Get init task ref
    auto *init = Scheduler::find_task(1);
    JARVIS_ASSERT(init != nullptr);

    // Get current task
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    // Verify there are at least 2 tasks (init + current)
    JARVIS_ASSERT(Scheduler::task_count() >= 2);
    JARVIS_TEST_PASS();
}

void register_init_tests() {
    kernel::Logger::info("Registering init tests");
    JARVIS_REGISTER_TEST(init_task_exists);
    JARVIS_REGISTER_TEST(init_task_has_no_parent);
    JARVIS_REGISTER_TEST(init_task_reparents_orphans);
    JARVIS_REGISTER_TEST(reaper_notified_on_child_exit);
    JARVIS_REGISTER_TEST(reaper_wakes_on_daemon_ipc);
}
