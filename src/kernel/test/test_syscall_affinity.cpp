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

/// @file test_syscall_affinity.cpp
/// @brief SYS_SET/GET_AFFINITY syscall tests (issue #61).  DRIVEN: every
///        syscall is invoked by a REAL dispatched kernel task so
///        syscall_task() resolves genuinely (test_syscall.cpp pattern).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

// Create a REAL kernel task, dispatch it, and wait for genuine
// termination (test_syscall.cpp run_syscall_task pattern, local copy:
// that helper is file-static there).
TaskControlBlock *run_affinity_task(void (*entry)()) {
    auto *t = TaskControlBlock::create(entry, 11, 10);
    if (t == nullptr)
        return nullptr;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    return t;
}

void release_affinity_task(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    kernel::test::terminate_if_live(t);
}

} // namespace

// Runmode: kernel
// Testidea: SET/GET affinity round-trips on the caller (pid 0 = self).
// Input: Dispatched task calls SET_AFFINITY(0, 0x1), then
//        GET_AFFINITY(0).
// Expect: SET returns 0; GET returns 0x1.
// Depends: Syscall SET_AFFINITY/GET_AFFINITY (issue #61)
JARVIS_TEST(syscall_affinity_set_get_roundtrip, "PRE: none | POST: none") {
    static uint64_t g_set_ret = 0;
    static uint64_t g_get_ret = 0;

    auto *t = run_affinity_task([]() {
        g_set_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::SET_AFFINITY), 0, 0x1,
            0, 0, nullptr);
        g_get_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::GET_AFFINITY), 0, 0,
            0, 0, nullptr);
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_set_ret);
    JARVIS_ASSERT_EQ(0x1ULL, g_get_ret);
    release_affinity_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Explicit-pid form addresses the caller by its own pid.
// Input: Task reads its pid via GETPID, then SET/GET with that pid.
// Expect: SET returns 0; GET returns the set mask.
// Depends: Syscall SET_AFFINITY/GET_AFFINITY (issue #61)
JARVIS_TEST(syscall_affinity_explicit_pid, "PRE: none | POST: none") {
    static uint64_t g_set_ret = 0;
    static uint64_t g_get_ret = 0;

    auto *t = run_affinity_task([]() {
        uint64_t pid = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::GETPID), 0, 0, 0, 0,
            nullptr);
        g_set_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::SET_AFFINITY), pid,
            0x1, 0, 0, nullptr);
        g_get_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::GET_AFFINITY), pid,
            0, 0, 0, nullptr);
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_set_ret);
    JARVIS_ASSERT_EQ(0x1ULL, g_get_ret);
    release_affinity_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Unknown pids fail closed with -1 (no clamp, no panic).
// Input: SET/GET with a pid that can never exist.
// Expect: Both return 0xFFFFFFFFFFFFFFFF.
// Depends: Syscall SET_AFFINITY/GET_AFFINITY (issue #61)
JARVIS_TEST(syscall_affinity_bad_pid, "PRE: none | POST: none") {
    static uint64_t g_set_ret = 0;
    static uint64_t g_get_ret = 0;
    constexpr uint64_t k_no_such_pid = 0xFFFFFFFEULL;

    auto *t = run_affinity_task([]() {
        g_set_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::SET_AFFINITY),
            k_no_such_pid, 0x1, 0, 0, nullptr);
        g_get_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::GET_AFFINITY),
            k_no_such_pid, 0, 0, 0, nullptr);
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0xFFFFFFFFFFFFFFFFULL, g_set_ret);
    JARVIS_ASSERT_EQ(0xFFFFFFFFFFFFFFFFULL, g_get_ret);
    release_affinity_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: An empty mask clamps to CPU0 (fail-closed, same policy as
//           the kernel-internal set_affinity) instead of erroring.
// Input: SET_AFFINITY(0, 0), then GET_AFFINITY(0).
// Expect: SET returns 0; GET returns 0x1.
// Depends: Syscall SET_AFFINITY/GET_AFFINITY (issue #61)
JARVIS_TEST(syscall_affinity_empty_mask, "PRE: none | POST: none") {
    static uint64_t g_set_ret = 0;
    static uint64_t g_get_ret = 0;

    auto *t = run_affinity_task([]() {
        g_set_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::SET_AFFINITY), 0, 0,
            0, 0, nullptr);
        g_get_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::GET_AFFINITY), 0, 0,
            0, 0, nullptr);
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_set_ret);
    JARVIS_ASSERT_EQ(0x1ULL, g_get_ret);
    release_affinity_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

void register_syscall_affinity_tests() {
    Logger::info("Registering syscall affinity tests");
    JARVIS_REGISTER_TEST(syscall_affinity_set_get_roundtrip);
    JARVIS_REGISTER_TEST(syscall_affinity_explicit_pid);
    JARVIS_REGISTER_TEST(syscall_affinity_bad_pid);
    JARVIS_REGISTER_TEST(syscall_affinity_empty_mask);
}
