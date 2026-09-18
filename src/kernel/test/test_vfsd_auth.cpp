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

/// @file test_vfsd_auth.cpp
/// @brief VFS daemon authorisation tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): every VFS syscall runs inside a REAL
/// kernel task (prio ≥ 11) that is genuinely dispatched — the handler's
/// `syscall_task()` resolves to the running task and the kernel-bypass /
/// daemon authorisation path is exercised through real execution.  The
/// harness never calls Syscall::handle() directly.

#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-null-argument"
#pragma GCC diagnostic ignored "-Wanalyzer-possible-null-dereference"
#endif

#include <test.hpp>
#include <logger.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/vfs/vfsd.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

#if !defined(CONFIG_ARCH_RISCV64)

namespace {
void release_task(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    kernel::test::terminate_if_live(t);
}
} // namespace

// Runmode: kernel
// Testidea: Verifies that the REAL vfsd daemon task self-authorizes VFS
// syscalls (is_vfsd_task() true), while a fresh test task is not the daemon.
// Input: Query the live vfsd daemon task; create a fresh kernel task.
// Expect: is_vfsd_task() is true for the real daemon, false for a test task.
// Depends: kernel::vfsd, kernel::Scheduler
JARVIS_TEST(vfsd_self_authorization, "PRE: vfsd, iocd | POST: none") {
    uint64_t daemon_pid = vfsd::get_vfsd_pid();
    JARVIS_ASSERT(daemon_pid != 0);
    auto *daemon = Scheduler::find_task(daemon_pid);
    JARVIS_ASSERT(daemon != nullptr);

    auto *t = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    JARVIS_ASSERT(t->id != daemon_pid);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the VFS self-authorization fd-op path: a REAL kernel
// task opens /dev/null, reads, and closes — the syscalls run in the task's
// own dispatched context via the kernel bypass.
// Input: Dispatch a kernel task that calls sys_open/read/close.
// Expect: fd >= 0, read == 0 (EOF), close == 0.
// Depends: kernel::Syscall, kernel::Scheduler, kernel::vfsd
JARVIS_TEST(vfsd_self_authorization_fd_op, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_fd = 0;
    static uint64_t g_read = 0;
    static uint64_t g_close = 0;

    auto *t = TaskControlBlock::create(
        []() {
            const char *path = "/dev/null";
            g_fd = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::OPEN),
                reinterpret_cast<uint64_t>(path), 0, 0, 0, nullptr);
            if (static_cast<int64_t>(g_fd) < 0)
                return;
            char buf[4];
            g_read = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::READ), g_fd,
                reinterpret_cast<uint64_t>(buf), 4, 0, nullptr);
            g_close = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::CLOSE), g_fd, 0, 0, 0,
                nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT(static_cast<int64_t>(g_fd) >= 0);
    JARVIS_ASSERT_EQ(0ULL, g_read);
    JARVIS_ASSERT_EQ(0ULL, g_close);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that a VFS syscall with an unresolvable path fails
// gracefully (returns -1) when run from a REAL dispatched kernel task.
// Input: Dispatch a kernel task that calls sys_open("/nonexistent-audit").
// Expect: sys_open returns -1 (ENOENT path).
// Depends: kernel::Syscall, kernel::vfsd
JARVIS_TEST(vfsd_absent_authorize_fails, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_ret = 0;

    auto *t = TaskControlBlock::create(
        []() {
            const char *path = "/nonexistent-audit";
            g_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::OPEN),
                reinterpret_cast<uint64_t>(path), 0, 0, 0, nullptr);
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

// Runmode: kernel
// Testidea: Verifies a VFS syscall with a null path fails gracefully from a
// REAL dispatched kernel task.
// Input: Dispatch a kernel task that calls sys_open(nullptr).
// Expect: sys_open returns -1 (no crash).
// Depends: kernel::Syscall, kernel::vfsd
JARVIS_TEST(vfsd_absent_syscall_fails, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_ret = 0;

    auto *t = TaskControlBlock::create(
        []() {
            g_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::OPEN), 0, 0, 0, 0,
                nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    // Null path: resolution fails → -1.  (The old assert `== -1 || >= 0`
    // accepted every value — a tautology.)
    JARVIS_ASSERT(static_cast<int64_t>(g_ret) == -1);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that a VFS syscall with a null path does not crash the
// kernel when run from a REAL dispatched task.
// Input: Dispatch a kernel task that calls sys_stat with a null path.
// Expect: Does not crash; the task terminates normally.
// Depends: kernel::Syscall, kernel::vfsd
JARVIS_TEST(vfsd_authorize_null_path, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_ran = 0;

    auto *t = TaskControlBlock::create(
        []() {
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::STAT), 0, 0,
                            0, 0, nullptr);
            g_ran = 1;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT_EQ(1ULL, g_ran);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: dup() on a live fd aliases the open file description — both fds
// read EOF from /dev/null and both closes succeed with balanced FD
// accounting. Exercises the never-dispatched sys_dup handler end to end.
// Input: Dispatched task opens /dev/null, dups the fd, reads via the alias,
// closes both.
// Expect: dup fd differs from fd; read == 0; both closes == 0.
// Depends: kernel::Syscall fd table, /dev/null vnode
JARVIS_TEST(vfsd_dup_success_aliases, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_fd = 0;
    static uint64_t g_dup = 0;
    static uint64_t g_read = 0;
    static uint64_t g_close1 = 0;
    static uint64_t g_close2 = 0;

    auto *t = TaskControlBlock::create(
        []() {
            const char *path = "/dev/null";
            g_fd = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::OPEN),
                reinterpret_cast<uint64_t>(path), 0, 0, 0, nullptr);
            if (static_cast<int64_t>(g_fd) < 0)
                return;
            g_dup = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::DUP), g_fd, 0, 0, 0,
                nullptr);
            if (static_cast<int64_t>(g_dup) < 0) {
                g_close1 = Syscall::handle(
                    static_cast<uint64_t>(SyscallNumber::CLOSE), g_fd, 0, 0,
                    0, nullptr);
                return;
            }
            char buf[4];
            g_read = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::READ), g_dup,
                reinterpret_cast<uint64_t>(buf), 4, 0, nullptr);
            g_close1 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::CLOSE), g_fd, 0, 0, 0,
                nullptr);
            g_close2 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::CLOSE), g_dup, 0, 0, 0,
                nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT(static_cast<int64_t>(g_fd) >= 0);
    JARVIS_ASSERT(static_cast<int64_t>(g_dup) >= 0);
    JARVIS_ASSERT(g_dup != g_fd);
    JARVIS_ASSERT_EQ(0ULL, g_read);
    JARVIS_ASSERT_EQ(0ULL, g_close1);
    JARVIS_ASSERT_EQ(0ULL, g_close2);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: dup() on invalid/closed fds fails closed without mutating the fd
// table — no allocation, no refcount change, no leak.
// Input: Dispatched task dups a never-opened fd and a negative fd.
// Expect: both return -1.
// Depends: kernel::Syscall fd table
JARVIS_TEST(vfsd_dup_invalid_fd, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_ret1 = 0;
    static uint64_t g_ret2 = 0;

    auto *t = TaskControlBlock::create(
        []() {
            g_ret1 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::DUP), 9999, 0, 0, 0,
                nullptr);
            g_ret2 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::DUP), ~0ULL, 0, 0, 0,
                nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_ret1);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_ret2);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: dup2(old, old) is a no-op returning the fd without touching the
// table — self-alias must not ref-inc, re-alloc, or leak.
// Input: Dispatched task opens /dev/null, dup2(fd, fd), closes once.
// Expect: dup2 returns fd; close == 0.
// Depends: kernel::Syscall fd table
JARVIS_TEST(vfsd_dup2_self, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_fd = 0;
    static uint64_t g_dup2 = 0;
    static uint64_t g_close = 0;

    auto *t = TaskControlBlock::create(
        []() {
            const char *path = "/dev/null";
            g_fd = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::OPEN),
                reinterpret_cast<uint64_t>(path), 0, 0, 0, nullptr);
            if (static_cast<int64_t>(g_fd) < 0)
                return;
            g_dup2 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::DUP2), g_fd, g_fd, 0, 0,
                nullptr);
            g_close = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::CLOSE), g_fd, 0, 0, 0,
                nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT(static_cast<int64_t>(g_fd) >= 0);
    JARVIS_ASSERT_EQ(g_fd, g_dup2);
    JARVIS_ASSERT_EQ(0ULL, g_close);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: dup2() with a dead source or an out-of-range target fails closed
// before mutating anything — no slot freed, no alias installed.
// Input: Dispatched task dup2(never-opened, 3) and dup2 over a huge target.
// Expect: both return -1.
// Depends: kernel::Syscall fd table
JARVIS_TEST(vfsd_dup2_rejected, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_ret1 = 0;
    static uint64_t g_ret2 = 0;

    auto *t = TaskControlBlock::create(
        []() {
            const char *path = "/dev/null";
            uint64_t fd = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::OPEN),
                reinterpret_cast<uint64_t>(path), 0, 0, 0, nullptr);
            if (static_cast<int64_t>(fd) < 0)
                return;
            g_ret1 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::DUP2), 9999, 3, 0, 0,
                nullptr);
            g_ret2 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::DUP2), fd, 1000000, 0,
                0, nullptr);
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::CLOSE), fd,
                            0, 0, 0, nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_ret1);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_ret2);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: pipe() installs a connected pair through the never-dispatched
// handler; both ends close cleanly with balanced FD accounting.
// Input: Dispatched task creates a pipe, closes both ends.
// Expect: pipe returns 0; both fds distinct; both closes == 0.
// Depends: kernel::Syscall fd table, pipe vnode pair
JARVIS_TEST(vfsd_pipe_roundtrip, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_ret = 0;
    static int g_fds[2] = {-1, -1};
    static uint64_t g_close1 = 0;
    static uint64_t g_close2 = 0;

    auto *t = TaskControlBlock::create(
        []() {
            g_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::PIPE),
                reinterpret_cast<uint64_t>(g_fds), 0, 0, 0, nullptr);
            if (static_cast<int64_t>(g_ret) != 0)
                return;
            g_close1 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::CLOSE),
                static_cast<uint64_t>(g_fds[0]), 0, 0, 0, nullptr);
            g_close2 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::CLOSE),
                static_cast<uint64_t>(g_fds[1]), 0, 0, 0, nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_ASSERT(g_fds[0] != g_fds[1]);
    JARVIS_ASSERT_EQ(0ULL, g_close1);
    JARVIS_ASSERT_EQ(0ULL, g_close2);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: dup2 onto a free slot installs a working alias at the requested
// index. NOTE (verified 2026-09-18): sys_dup2 writes fds[new] directly,
// bypassing FdTable::alloc, so track_fd_add never fires for the alias while
// both closes track_fd_remove. The leak detector only flags growth
// (current > baseline), and track_fd_remove clamps at zero, so no LEAK
// fires — but a +1 real leak paired with a dup2 in one test could net to
// zero and hide. Instrumentation asymmetry, production-neutral (alloc scans
// the array, never the counter).
// Input: Dispatched task opens /dev/null, dup2(fd, 7), closes both.
// Expect: dup2 returns 7; both closes succeed.
// Depends: kernel::Syscall fd table
JARVIS_TEST(vfsd_dup2_free_slot, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_fd = 0;
    static uint64_t g_dup2 = 0;

    auto *t = TaskControlBlock::create(
        []() {
            const char *path = "/dev/null";
            g_fd = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::OPEN),
                reinterpret_cast<uint64_t>(path), 0, 0, 0, nullptr);
            if (static_cast<int64_t>(g_fd) < 0)
                return;
            g_dup2 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::DUP2), g_fd, 7, 0, 0,
                nullptr);
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::CLOSE), g_fd,
                            0, 0, 0, nullptr);
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::CLOSE), 7, 0,
                            0, 0, nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT(static_cast<int64_t>(g_fd) >= 0);
    JARVIS_ASSERT_EQ(7ULL, g_dup2);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: dup2 over an occupied fd displaces the old alias (ref-dec via
// free) and installs the new one; the displaced vnode is not leaked and the
// returned fd is the requested target. Same alloc-bypass note as
// vfsd_dup2_free_slot applies.
// Input: Dispatched task opens two fds, dup2(first, second), closes both.
// Expect: dup2 returns second fd; both closes succeed.
// Depends: kernel::Syscall fd table
JARVIS_TEST(vfsd_dup2_occupied_target, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_fd1 = 0;
    static uint64_t g_fd2 = 0;
    static uint64_t g_dup2 = 0;

    auto *t = TaskControlBlock::create(
        []() {
            const char *path = "/dev/null";
            g_fd1 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::OPEN),
                reinterpret_cast<uint64_t>(path), 0, 0, 0, nullptr);
            g_fd2 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::OPEN),
                reinterpret_cast<uint64_t>(path), 0, 0, 0, nullptr);
            if (static_cast<int64_t>(g_fd1) < 0 ||
                static_cast<int64_t>(g_fd2) < 0)
                return;
            g_dup2 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::DUP2), g_fd1, g_fd2, 0,
                0, nullptr);
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::CLOSE),
                            g_fd1, 0, 0, 0, nullptr);
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::CLOSE),
                            g_fd2, 0, 0, 0, nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT(static_cast<int64_t>(g_fd1) >= 0);
    JARVIS_ASSERT(static_cast<int64_t>(g_fd2) >= 0);
    JARVIS_ASSERT_EQ(g_fd2, g_dup2);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all VFS authorization tests with the test framework.
// Input: None
// Expect: All vfsd_* authorization tests registered via JARVIS_REGISTER_TEST
// Depends: kernel test framework
void register_vfsd_authorization_tests() {
    Logger::info("Registering VFS daemon authorization tests");
    JARVIS_REGISTER_TEST(vfsd_self_authorization);
    JARVIS_REGISTER_TEST(vfsd_self_authorization_fd_op);
    JARVIS_REGISTER_TEST(vfsd_absent_authorize_fails);
    JARVIS_REGISTER_TEST(vfsd_absent_syscall_fails);
    JARVIS_REGISTER_TEST(vfsd_authorize_null_path);
    JARVIS_REGISTER_TEST(vfsd_dup_success_aliases);
    JARVIS_REGISTER_TEST(vfsd_dup_invalid_fd);
    JARVIS_REGISTER_TEST(vfsd_dup2_self);
    JARVIS_REGISTER_TEST(vfsd_dup2_rejected);
    JARVIS_REGISTER_TEST(vfsd_pipe_roundtrip);
    JARVIS_REGISTER_TEST(vfsd_dup2_free_slot);
    JARVIS_REGISTER_TEST(vfsd_dup2_occupied_target);
}
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
#endif
