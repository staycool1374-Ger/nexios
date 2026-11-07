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

/// @file test_rlimit.cpp
/// @brief Resource limit (rlimit) tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): every syscall runs inside a REAL
/// kernel task (prio ≥ 11) that is genuinely dispatched — the handler's
/// `syscall_task()` resolves to the running task.  The harness never calls
/// Syscall::handle() directly.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/test/test_sched_helpers.hpp>

using namespace kernel;

struct Rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

enum RlimitResource {
    RLIMIT_DATA = 0,
    RLIMIT_STACK = 1,
    RLIMIT_NOFILE = 2,
};

JARVIS_TEST(sys_getrlimit_nofile, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_cur = 0;
    static uint64_t g_max = 0;

    auto *t = TaskControlBlock::create(
        []() {
            Rlimit rl;
            g_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::GETRLIMIT), RLIMIT_NOFILE,
                reinterpret_cast<uint64_t>(&rl), 0, 0, nullptr);
            g_cur = rl.rlim_cur;
            g_max = rl.rlim_max;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    // Cleanup BEFORE asserting (cookbook Rule 5): self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_ASSERT(g_cur > 0);
    JARVIS_ASSERT(g_max > 0);
    JARVIS_ASSERT_EQ(g_cur, g_max);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(sys_getrlimit_stack, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_cur = 0;
    static uint64_t g_max = 0;

    auto *t = TaskControlBlock::create(
        []() {
            Rlimit rl;
            g_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::GETRLIMIT), RLIMIT_STACK,
                reinterpret_cast<uint64_t>(&rl), 0, 0, nullptr);
            g_cur = rl.rlim_cur;
            g_max = rl.rlim_max;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    // Cleanup BEFORE asserting (cookbook Rule 5): self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_ASSERT(g_cur > 0);
    JARVIS_ASSERT(g_max > 0);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(sys_getrlimit_data, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_cur = 0;
    static uint64_t g_max = 0;

    auto *t = TaskControlBlock::create(
        []() {
            Rlimit rl;
            g_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::GETRLIMIT), RLIMIT_DATA,
                reinterpret_cast<uint64_t>(&rl), 0, 0, nullptr);
            g_cur = rl.rlim_cur;
            g_max = rl.rlim_max;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    // Cleanup BEFORE asserting (cookbook Rule 5): self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_ASSERT(g_cur > 0);
    JARVIS_ASSERT(g_max > 0);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(sys_getrlimit_invalid, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;

    auto *t = TaskControlBlock::create(
        []() {
            Rlimit rl;
            g_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::GETRLIMIT), 99,
                reinterpret_cast<uint64_t>(&rl), 0, 0, nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    // Cleanup BEFORE asserting (cookbook Rule 5): self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_ret);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(sys_brk_query, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_break = 0;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            g_ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::BRK),
                                    0, 0, 0, 0, nullptr);
            g_break = cur->program_break;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    // Cleanup BEFORE asserting (cookbook Rule 5): self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(g_break, g_ret);
    JARVIS_TEST_PASS();
}

void register_rlimit_tests() {
    kernel::Logger::info("Registering rlimit tests");
    JARVIS_REGISTER_TEST(sys_getrlimit_nofile);
    JARVIS_REGISTER_TEST(sys_getrlimit_stack);
    JARVIS_REGISTER_TEST(sys_getrlimit_data);
    JARVIS_REGISTER_TEST(sys_getrlimit_invalid);
    JARVIS_REGISTER_TEST(sys_brk_query);
}
