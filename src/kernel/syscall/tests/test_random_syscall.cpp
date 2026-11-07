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

/// @file test_random_syscall.cpp
/// @brief Random system call interface tests.
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
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {
void release_task(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    kernel::test::terminate_if_live(t);
}
} // namespace

// Runmode: kernel
// Testidea: SYS_GETRANDOM fills a buffer with random bytes via syscall,
// invoked from a REAL dispatched kernel task.
// Input: Dispatched task calls Syscall::handle(GETRANDOM, buf, 64, 0, ...)
// Expect: Returns 64; buffer not all-zero or all-FF
JARVIS_TEST(syscall_getrandom_basic, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_all_zero = 0;
    static uint64_t g_all_ff = 0;

    auto *t = TaskControlBlock::create(
        []() {
            uint8_t buf[64];
            __builtin_memset(buf, 0, sizeof(buf));
            g_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::GETRANDOM),
                reinterpret_cast<uint64_t>(buf), 64, 0, 0, nullptr);

            bool all_zero = true;
            bool all_ff = true;
            for (size_t i = 0; i < sizeof(buf); ++i) {
                if (buf[i] != 0)
                    all_zero = false;
                if (buf[i] != 0xFF)
                    all_ff = false;
            }
            g_all_zero = all_zero ? 1 : 0;
            g_all_ff = all_ff ? 1 : 0;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT_EQ(64ULL, g_ret);
    JARVIS_ASSERT_FMT(g_all_zero == 0, "GETRANDOM returned 64 zero bytes");
    JARVIS_ASSERT_FMT(g_all_ff == 0, "GETRANDOM returned 64 0xFF bytes");
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Zero-length GETRANDOM returns 0 and leaves buffer unchanged.
// Input: Dispatched task calls Syscall::handle(GETRANDOM, buf, 0, 0, ...)
// Expect: Returns 0; buffer unchanged
JARVIS_TEST(syscall_getrandom_zero, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_unchanged = 0;

    auto *t = TaskControlBlock::create(
        []() {
            uint8_t buf[4] = {0xDE, 0xAD, 0xBE, 0xEF};
            g_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::GETRANDOM),
                reinterpret_cast<uint64_t>(buf), 0, 0, 0, nullptr);
            g_unchanged = (buf[0] == 0xDE && buf[1] == 0xAD && buf[2] == 0xBE &&
                           buf[3] == 0xEF)
                              ? 1
                              : 0;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_ASSERT_EQ(1ULL, g_unchanged);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Large GETRANDOM (4096 bytes) succeeds without overflow.
// Input: Dispatched task calls Syscall::handle(GETRANDOM, buf, 4096, 0, ...)
// Expect: Returns 4096; at least one byte non-zero
JARVIS_TEST(syscall_getrandom_large, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_any_nonzero = 0;

    auto *t = TaskControlBlock::create(
        []() {
            uint8_t buf[4096];
            __builtin_memset(buf, 0, sizeof(buf));
            g_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::GETRANDOM),
                reinterpret_cast<uint64_t>(buf), 4096, 0, 0, nullptr);

            for (size_t i = 0; i < sizeof(buf); ++i) {
                if (buf[i] != 0) {
                    g_any_nonzero = 1;
                    break;
                }
            }
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT_EQ(4096ULL, g_ret);
    JARVIS_ASSERT_FMT(g_any_nonzero == 1, "GETRANDOM(4096) returned all zeros");
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Non-zero flags to GETRANDOM returns -1 (EINVAL).
// Input: Dispatched task calls Syscall::handle(GETRANDOM, buf, 8, 1, ...)
// Expect: Returns static_cast<uint64_t>(-1)
JARVIS_TEST(syscall_getrandom_invalid_flags, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;

    auto *t = TaskControlBlock::create(
        []() {
            uint8_t buf[8];
            g_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::GETRANDOM),
                reinterpret_cast<uint64_t>(buf), 8, 1, 0, nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_ret);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

void register_random_syscall_tests() {
    Logger::info("Registering SYS_GETRANDOM tests");
    JARVIS_REGISTER_TEST(syscall_getrandom_basic);
    JARVIS_REGISTER_TEST(syscall_getrandom_zero);
    JARVIS_REGISTER_TEST(syscall_getrandom_large);
    JARVIS_REGISTER_TEST(syscall_getrandom_invalid_flags);
}
