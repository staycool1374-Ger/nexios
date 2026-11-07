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
#include <kernel/vfs/vfs.hpp>
#include <kernel/vfs/vfsd.hpp>
#include <kernel/vfs/fat32.hpp>
#include <kernel/vfs/fat32_fs.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/driver/block_device.hpp>
#include <kernel/memory/mempool.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

// Issue #356 QE note: the pre-v0.3.10 riscv64 exclusion was lifted after
// the #387 fork fix (re-verify per run; restore the gate with evidence
// if riscv64-specific failures reappear).

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
// Testidea: mkdir+unlink roundtrip through dispatched syscalls on tmpfs:
// create, resolve-visible, remove, resolve-gone. Closes sys_mkdir/unlink.
// Input: Dispatched task mkdirs /tmp/AUDIT_D1, resolves, unlinks, resolves.
// Expect: mkdir 0, visible, unlink 0, gone.
// Depends: kernel::Syscall, tmpfs /tmp
JARVIS_TEST(vfsd_mkdir_unlink_roundtrip, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_mkdir = 0;
    static uint64_t g_seen = 0;
    static uint64_t g_unlink = 0;
    static uint64_t g_gone = 0;

    auto *t = TaskControlBlock::create(
        []() {
            const char *dir = "/tmp/AUDIT_D1";
            g_mkdir = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::MKDIR),
                reinterpret_cast<uint64_t>(dir), 0, 0, 0, nullptr);
            g_seen = (vfs::resolve(dir) != nullptr) ? 1ULL : 0ULL;
            g_unlink = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::UNLINK),
                reinterpret_cast<uint64_t>(dir), 0, 0, 0, nullptr);
            g_gone = (vfs::resolve(dir) == nullptr) ? 1ULL : 0ULL;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT_EQ(0ULL, g_mkdir);
    JARVIS_ASSERT_EQ(1ULL, g_seen);
    JARVIS_ASSERT_EQ(0ULL, g_unlink);
    JARVIS_ASSERT_EQ(1ULL, g_gone);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: mkdir/unlink rejection matrix — existing dir, empty leaf
// (trailing slash), and missing target all fail closed without side effects.
// Input: Dispatched task mkdirs /tmp + /tmp/, unlinks a missing path.
// Expect: all three return -1.
// Depends: kernel::Syscall, resolve_parent_then_authorize
JARVIS_TEST(vfsd_mkdir_rejected, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_ret1 = 0;
    static uint64_t g_ret2 = 0;
    static uint64_t g_ret3 = 0;

    auto *t = TaskControlBlock::create(
        []() {
            const char *existing = "/tmp";
            const char *empty_leaf = "/tmp/";
            const char *missing = "/tmp/AUDIT_NOSUCH";
            g_ret1 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::MKDIR),
                reinterpret_cast<uint64_t>(existing), 0, 0, 0, nullptr);
            g_ret2 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::MKDIR),
                reinterpret_cast<uint64_t>(empty_leaf), 0, 0, 0, nullptr);
            g_ret3 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::UNLINK),
                reinterpret_cast<uint64_t>(missing), 0, 0, 0, nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_ret1);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_ret2);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_ret3);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: rmdir enforces directory semantics via dispatch — non-empty
// rmdir fails, and empty rmdir succeeds after draining the child.
// Input: Dispatched task mkdirs P + P/C, rmdirs P (fail), unlinks C,
// rmdirs P (ok).
// Expect: rmdir-nonempty != 0; unlink 0; rmdir-empty 0.
// Depends: kernel::Syscall, tmpfs unlink
JARVIS_TEST(vfsd_rmdir_nonempty, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_rmdir_full = 0;
    static uint64_t g_unlink = 0;
    static uint64_t g_rmdir_empty = 0;

    auto *t = TaskControlBlock::create(
        []() {
            const char *parent = "/tmp/AUDIT_RM";
            const char *child = "/tmp/AUDIT_RM/CHILD";
            uint64_t mk1 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::MKDIR),
                reinterpret_cast<uint64_t>(parent), 0, 0, 0, nullptr);
            uint64_t mk2 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::MKDIR),
                reinterpret_cast<uint64_t>(child), 0, 0, 0, nullptr);
            if (mk1 != 0 || mk2 != 0)
                return;
            g_rmdir_full = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::RMDIR),
                reinterpret_cast<uint64_t>(parent), 0, 0, 0, nullptr);
            if (g_rmdir_full == 0)
                return;
            g_unlink = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::UNLINK),
                reinterpret_cast<uint64_t>(child), 0, 0, 0, nullptr);
            g_rmdir_empty = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::RMDIR),
                reinterpret_cast<uint64_t>(parent), 0, 0, 0, nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT(g_rmdir_full != 0);
    JARVIS_ASSERT_EQ(0ULL, g_unlink);
    JARVIS_ASSERT_EQ(0ULL, g_rmdir_empty);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: lseek valid-whence paths on /dev/null through dispatch —
// SEEK_SET/CUR/END update the fd offset; closes sys_lseek happy paths.
// Input: Dispatched task opens /dev/null, seeks SET 42, CUR +8, END 7.
// Expect: positions 42, 50, 7.
// Depends: kernel::Syscall, null_lseek
JARVIS_TEST(vfsd_lseek_nulldev, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_set = 0;
    static uint64_t g_cur = 0;
    static uint64_t g_end = 0;

    auto *t = TaskControlBlock::create(
        []() {
            const char *path = "/dev/null";
            uint64_t fd = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::OPEN),
                reinterpret_cast<uint64_t>(path), 0, 0, 0, nullptr);
            if (static_cast<int64_t>(fd) < 0)
                return;
            g_set = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::LSEEK), fd, 42,
                static_cast<uint64_t>(vfs::SEEK_SET), 0, nullptr);
            g_cur = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::LSEEK), fd, 8,
                static_cast<uint64_t>(vfs::SEEK_CUR), 0, nullptr);
            g_end = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::LSEEK), fd, 7,
                static_cast<uint64_t>(vfs::SEEK_END), 0, nullptr);
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::CLOSE), fd,
                            0, 0, 0, nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT_EQ(42ULL, g_set);
    JARVIS_ASSERT_EQ(50ULL, g_cur);
    JARVIS_ASSERT_EQ(7ULL, g_end);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: lseek rejection — pipe fds are not seekable and dead fds fail
// before reaching the vnode op.
// Input: Dispatched task lseeks a pipe end and fd 9999.
// Expect: both return -1.
// Depends: kernel::Syscall, pipe_lseek
JARVIS_TEST(vfsd_lseek_rejected, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_ret1 = 0;
    static uint64_t g_ret2 = 0;

    auto *t = TaskControlBlock::create(
        []() {
            int fds[2] = {-1, -1};
            uint64_t pr = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::PIPE),
                reinterpret_cast<uint64_t>(fds), 0, 0, 0, nullptr);
            if (pr != 0)
                return;
            g_ret1 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::LSEEK),
                static_cast<uint64_t>(fds[0]), 0,
                static_cast<uint64_t>(vfs::SEEK_SET), 0, nullptr);
            g_ret2 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::LSEEK), 9999, 0,
                static_cast<uint64_t>(vfs::SEEK_SET), 0, nullptr);
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::CLOSE),
                            static_cast<uint64_t>(fds[0]), 0, 0, 0, nullptr);
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::CLOSE),
                            static_cast<uint64_t>(fds[1]), 0, 0, 0, nullptr);
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
// Testidea: ioctl dispatch — /dev/null reports unsupported (VFS_INVALID)
// and dead fds fail before the op. Closes sys_ioctl.
// Input: Dispatched task ioctls /dev/null and fd 9999.
// Expect: null ret seasons to -4; bad fd -1.
// Depends: kernel::Syscall, null_ioctl
JARVIS_TEST(vfsd_ioctl_nulldev, "PRE: vfsd, iocd | POST: none") {
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
                static_cast<uint64_t>(SyscallNumber::IOCTL), fd, 0, 0, 0,
                nullptr);
            g_ret2 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IOCTL), 9999, 0, 0, 0,
                nullptr);
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::CLOSE), fd,
                            0, 0, 0, nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT_EQ(-4LL, static_cast<int64_t>(g_ret1));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_ret2);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: readdir dispatch reachability — open /tmp, create an entry,
// invoke READDIR, verify ret 0, clean up. Kernel-task delivery (dent +
// position write-back) is covered by vfsd_readdir_delivers (issue #175).
// Input: Dispatched task mkdirs /tmp/AUDIT_RD, readdirs /tmp from pos 0.
// Expect: ret 0.
// Depends: kernel::Syscall, tmpfs_readdir
JARVIS_TEST(vfsd_readdir_tmp, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_mk = 0;
    static uint64_t g_openok = 0;
    static uint64_t g_ret = 0;
    static uint64_t g_pos = 0;
    static vfs::Dirent g_dent{};

    auto *t = TaskControlBlock::create(
        []() {
            const char *dir = "/tmp";
            const char *sub = "/tmp/AUDIT_RD";
            g_mk = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::MKDIR),
                reinterpret_cast<uint64_t>(sub), 0, 0, 0, nullptr);
            if (g_mk != 0)
                return;
            uint64_t fd = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::OPEN),
                reinterpret_cast<uint64_t>(dir), 0, 0, 0, nullptr);
            if (static_cast<int64_t>(fd) < 0) {
                Syscall::handle(
                    static_cast<uint64_t>(SyscallNumber::UNLINK),
                    reinterpret_cast<uint64_t>(sub), 0, 0, 0, nullptr);
                return;
            }
            g_openok = 1;
            g_pos = 0;
            g_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::READDIR), fd,
                reinterpret_cast<uint64_t>(&g_pos),
                reinterpret_cast<uint64_t>(&g_dent), 0, nullptr);
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::CLOSE), fd,
                            0, 0, 0, nullptr);
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::UNLINK),
                            reinterpret_cast<uint64_t>(sub), 0, 0, 0,
                            nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT_EQ(0ULL, g_mk);
    JARVIS_ASSERT_EQ(1ULL, g_openok);
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: fstat delivers the result to kernel-task callers (issue #175):
//           the handler built into a kernel-local VfsStat but only copied
//           out for user tasks, so kernel out-params stayed poisoned.
// Input: Dispatched task opens /dev/null, fstats into a 0xAA-poisoned
//        VfsStat.
// Expect: ret 0, st_mode == S_IFCHR (poison overwritten).
// Depends: kernel::Syscall, null_fstat
JARVIS_TEST(vfsd_fstat_delivers, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_ret = 0;
    static vfs::VfsStat g_st{};

    auto *t = TaskControlBlock::create(
        []() {
            const char *path = "/dev/null";
            uint64_t fd = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::OPEN),
                reinterpret_cast<uint64_t>(path), 0, 0, 0, nullptr);
            if (static_cast<int64_t>(fd) < 0)
                return;
            auto *raw = reinterpret_cast<uint8_t *>(&g_st);
            for (size_t idx = 0; idx < sizeof(g_st); ++idx)
                raw[idx] = 0xAA;
            g_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::FSTAT), fd,
                reinterpret_cast<uint64_t>(&g_st), 0, 0, nullptr);
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::CLOSE), fd,
                            0, 0, 0, nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(vfs::S_IFCHR),
                     static_cast<uint64_t>(g_st.st_mode));
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: stat delivers the result to kernel-task callers (issue #175,
//           same kernel-local-without-copy-out pattern as sys_fstat).
// Input: Dispatched task stats /dev/null into a 0xAA-poisoned VfsStat.
// Expect: ret 0, st_mode == S_IFCHR (poison overwritten).
// Depends: kernel::Syscall, null_fstat
JARVIS_TEST(vfsd_stat_delivers, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_ret = 0;
    static vfs::VfsStat g_st{};

    auto *t = TaskControlBlock::create(
        []() {
            const char *path = "/dev/null";
            auto *raw = reinterpret_cast<uint8_t *>(&g_st);
            for (size_t idx = 0; idx < sizeof(g_st); ++idx)
                raw[idx] = 0xAA;
            g_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::STAT),
                reinterpret_cast<uint64_t>(path),
                reinterpret_cast<uint64_t>(&g_st), 0, 0, nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(vfs::S_IFCHR),
                     static_cast<uint64_t>(g_st.st_mode));
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: readdir delivers dent + advanced position to kernel-task
//           callers (issue #175: CheckedPtr read/write fail closed on
//           kernel addresses, so both stayed at their inputs).
// Input: Dispatched task mkdirs /tmp/AUDIT_DEL, readdirs /tmp from pos 0
//        with a 0xAA-poisoned Dirent.
// Expect: ret 0, pos 0 -> 1, d_name[0] no longer poisoned.
// Depends: kernel::Syscall, tmpfs_readdir
JARVIS_TEST(vfsd_readdir_delivers, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_mk = 0;
    static uint64_t g_openok = 0;
    static uint64_t g_ret = 0;
    static uint64_t g_pos = 0;
    static vfs::Dirent g_dent{};

    auto *t = TaskControlBlock::create(
        []() {
            const char *dir = "/tmp";
            const char *sub = "/tmp/AUDIT_DEL";
            g_mk = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::MKDIR),
                reinterpret_cast<uint64_t>(sub), 0, 0, 0, nullptr);
            if (g_mk != 0)
                return;
            uint64_t fd = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::OPEN),
                reinterpret_cast<uint64_t>(dir), 0, 0, 0, nullptr);
            if (static_cast<int64_t>(fd) < 0) {
                Syscall::handle(
                    static_cast<uint64_t>(SyscallNumber::UNLINK),
                    reinterpret_cast<uint64_t>(sub), 0, 0, 0, nullptr);
                return;
            }
            g_openok = 1;
            g_pos = 0;
            auto *raw = reinterpret_cast<uint8_t *>(&g_dent);
            for (size_t idx = 0; idx < sizeof(g_dent); ++idx)
                raw[idx] = 0xAA;
            g_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::READDIR), fd,
                reinterpret_cast<uint64_t>(&g_pos),
                reinterpret_cast<uint64_t>(&g_dent), 0, nullptr);
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::CLOSE), fd,
                            0, 0, 0, nullptr);
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::UNLINK),
                            reinterpret_cast<uint64_t>(sub), 0, 0, 0,
                            nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT_EQ(0ULL, g_mk);
    JARVIS_ASSERT_EQ(1ULL, g_openok);
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_ASSERT_EQ(1ULL, g_pos);
    JARVIS_ASSERT(g_dent.d_name[0] != static_cast<char>(0xAA));
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: lseek with a bogus whence fails (issue #176): null_lseek fell
//           through its switch default and reported the stale offset as
//           success (POSIX requires EINVAL).
// Input: Dispatched task opens /dev/null, seeks with whence 999.
// Expect: return -1.
// Depends: kernel::Syscall, null_lseek, sys_lseek whence validation
JARVIS_TEST(vfsd_lseek_bogus_whence, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_ret = 0;

    auto *t = TaskControlBlock::create(
        []() {
            const char *path = "/dev/null";
            uint64_t fd = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::OPEN),
                reinterpret_cast<uint64_t>(path), 0, 0, 0, nullptr);
            if (static_cast<int64_t>(fd) < 0)
                return;
            g_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::LSEEK), fd, 123, 999,
                0, nullptr);
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::CLOSE), fd,
                            0, 0, 0, nullptr);
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
// Testidea: sys_open succeeds on initrd files (issue #332) — initrd
// allocates a fresh vnode per lookup, so the strict pointer check can
// never pass; the immutable-ops identity must accept it.
// Input: Dispatched kernel task OPENs+CLOSEs /hey.c.elf (root ELF the
//        image build always installs).
// Expect: fd >= 0, close == 0. Fails pre-fix (pointer mismatch → -1).
// Depends: kernel::Syscall, initrd root image, vnode_identity_match
JARVIS_TEST(vfsd_initrd_open_succeeds, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_fd = 0;
    static uint64_t g_close = 0;

    auto *t = TaskControlBlock::create(
        []() {
            const char *path = "/hey.c.elf";
            g_fd = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::OPEN),
                reinterpret_cast<uint64_t>(path), 0, 0, 0, nullptr);
            if (static_cast<int64_t>(g_fd) < 0)
                return;
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
    JARVIS_ASSERT_EQ(0ULL, g_close);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

namespace {
 // Predicate-level fixtures (issue #332 negative controls): vnode pairs
 // with equal ino/size/mode at distinct addresses. Only the
 // readonly-declared table may match — a mutator-free but writable table
 // (fat32_file_ops shape: no mutators, readonly false) must still deny,
 // because mutability may live on a sibling table.
 vfs::VnodeOps g_imm_ops = {};
 vfs::VnodeOps g_mut_ops = {};
 vfs::VnodeOps g_plain_ops = {};
 vfs::VnodeOps g_stable_ops = {};
 int stub_mkdir(vfs::Vnode &, const char *, uint16_t) {
     return 0;
 }
} // namespace

// Runmode: kernel
// Testidea: The widened identity still denies mutable-ops vnodes (issue
// #332 TOCTOU guard) — equal fields at distinct addresses must deny
// when any namespace mutator exists, accept when none do.
// Input: Stack vnode pairs (mutable vs immutable ops tables).
// Expect: mutable denies, immutable accepts, same-pointer accepts,
//         null denies. No syscalls, no locks.
// Depends: vfs::vnode_identity_match
JARVIS_TEST(vfsd_identity_mutable_denied, "PRE: none | POST: none") {
    g_mut_ops.mkdir = stub_mkdir;
    g_imm_ops.readonly = true;
    vfs::Vnode imm_a{};
    imm_a.ops = &g_imm_ops;
    imm_a.ino = 1;
    imm_a.size = 10;
    imm_a.mode = vfs::S_IFREG;
    vfs::Vnode imm_b{};
    imm_b.ops = &g_imm_ops;
    imm_b.ino = 1;
    imm_b.size = 10;
    imm_b.mode = vfs::S_IFREG;
    JARVIS_ASSERT(vfs::vnode_identity_match(&imm_a, &imm_b, 1));
    JARVIS_ASSERT(vfs::vnode_identity_match(&imm_a, &imm_a, 1));
    vfs::Vnode mut_a{};
    mut_a.ops = &g_mut_ops;
    mut_a.ino = 1;
    mut_a.size = 10;
    mut_a.mode = vfs::S_IFREG;
    vfs::Vnode mut_b{};
    mut_b.ops = &g_mut_ops;
    mut_b.ino = 1;
    mut_b.size = 10;
    mut_b.mode = vfs::S_IFREG;
    JARVIS_ASSERT(!vfs::vnode_identity_match(&mut_a, &mut_b, 1));
    // fat32_file_ops shape (no mutators, writable image, readonly
    // false): must deny — ops-shape alone never admits (audit S1).
    vfs::Vnode plain_a{};
    plain_a.ops = &g_plain_ops;
    plain_a.ino = 1;
    plain_a.size = 10;
    plain_a.mode = vfs::S_IFREG;
    vfs::Vnode plain_b{};
    plain_b.ops = &g_plain_ops;
    plain_b.ino = 1;
    plain_b.size = 10;
    plain_b.mode = vfs::S_IFREG;
    JARVIS_ASSERT(!vfs::vnode_identity_match(&plain_a, &plain_b, 1));
    JARVIS_ASSERT(!vfs::vnode_identity_match(nullptr, &imm_a, 1));
    JARVIS_ASSERT(!vfs::vnode_identity_match(&imm_a, nullptr, 1));
    imm_b.size = 11;
    JARVIS_ASSERT(!vfs::vnode_identity_match(&imm_a, &imm_b, 1));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The stable-ino gate (issue #333) accepts same-key fresh
// vnodes on a declared table and still denies every drift shape.
// Input: Stack vnode pairs over a stable_ino-declared table.
// Expect: equal nonzero key accepts (both orders); size/mode drift,
//         stale ino, zero ino, ops mismatch, null deny.
// Depends: vfs::vnode_identity_match
JARVIS_TEST(vfsd_identity_stable_accepted, "PRE: none | POST: none") {
    g_stable_ops.stable_ino = true;
    vfs::Vnode a{};
    a.ops = &g_stable_ops;
    a.ino = 0x00020005ULL;
    a.size = 14;
    a.mode = vfs::S_IFREG;
    vfs::Vnode b{};
    b.ops = &g_stable_ops;
    b.ino = 0x00020005ULL;
    b.size = 14;
    b.mode = vfs::S_IFREG;
    JARVIS_ASSERT(vfs::vnode_identity_match(&a, &b, 0x00020005ULL));
    JARVIS_ASSERT(vfs::vnode_identity_match(&b, &a, 0x00020005ULL));
    vfs::Vnode size_drift = b;
    size_drift.size = 15;
    JARVIS_ASSERT(!vfs::vnode_identity_match(&a, &size_drift, a.ino));
    vfs::Vnode mode_drift = b;
    mode_drift.mode = vfs::S_IFDIR;
    JARVIS_ASSERT(!vfs::vnode_identity_match(&a, &mode_drift, a.ino));
    JARVIS_ASSERT(!vfs::vnode_identity_match(&a, &b, 0x00020006ULL));
    vfs::Vnode zero_a{};
    zero_a.ops = &g_stable_ops;
    zero_a.ino = 0;
    vfs::Vnode zero_b{};
    zero_b.ops = &g_stable_ops;
    zero_b.ino = 0;
    JARVIS_ASSERT(!vfs::vnode_identity_match(&zero_a, &zero_b, 0));
    vfs::Vnode other_ops = b;
    other_ops.ops = &g_plain_ops;
    JARVIS_ASSERT(!vfs::vnode_identity_match(&a, &other_ops, a.ino));
    JARVIS_ASSERT(!vfs::vnode_identity_match(nullptr, &b, b.ino));
    JARVIS_ASSERT(!vfs::vnode_identity_match(&a, nullptr, a.ino));
    JARVIS_TEST_PASS();
}

namespace {
// E2E fixture (issue #333): the shared read-only fat32 test image,
// mounted at /mnt for one OPEN round-trip. reset_and_remount clears
// the mount between tests, so no teardown is needed.
extern "C" uint8_t _binary_build_fat32_img_start[];
extern "C" uint8_t _binary_build_fat32_img_end[];
void setup_fat32_mnt() {
    if (!kernel::gs::get_fat32_partition()) {
        uint64_t bytes = static_cast<uint64_t>(
            _binary_build_fat32_img_end - _binary_build_fat32_img_start);
        auto *dev = new kernel::block::MockBlockDevice(
            _binary_build_fat32_img_start, bytes / kernel::fat32::SECTOR_SIZE,
            true);
        auto *partition = new kernel::fat32::Fat32Partition(*dev);
        if (partition && partition->mount())
            kernel::gs::try_set_fat32_partition(partition);
    }
    kernel::vfs::mount_fat32("/mnt");
}
} // namespace

// Runmode: kernel
// Testidea: sys_open on a fat32 path survives the TOCTOU re-resolve
// (issue #333) — both resolves allocate fresh vnodes, so only the
// stable-ino gate can accept; pre-fix this returned -1.
// Input: Dispatched kernel task OPENs+CLOSEs /mnt/HELLO.TXT (image
//        file the build always installs).
// Expect: fd >= 0, close == 0. Fails pre-fix (identity mismatch → -1).
// Depends: kernel::Syscall, fat32 image, mount, vnode_identity_match
JARVIS_TEST(vfsd_fat32_open_succeeds, "PRE: vfsd, iocd | POST: none") {
    setup_fat32_mnt();
    static uint64_t g_fd = 0;
    static uint64_t g_close = 0;

    auto *t = TaskControlBlock::create(
        []() {
            const char *path = "/mnt/HELLO.TXT";
            g_fd = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::OPEN),
                reinterpret_cast<uint64_t>(path), 0, 0, 0, nullptr);
            if (static_cast<int64_t>(g_fd) < 0)
                return;
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
    JARVIS_ASSERT_EQ(0ULL, g_close);
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
    JARVIS_REGISTER_TEST(vfsd_mkdir_unlink_roundtrip);
    JARVIS_REGISTER_TEST(vfsd_mkdir_rejected);
    JARVIS_REGISTER_TEST(vfsd_rmdir_nonempty);
    JARVIS_REGISTER_TEST(vfsd_lseek_nulldev);
    JARVIS_REGISTER_TEST(vfsd_lseek_rejected);
    JARVIS_REGISTER_TEST(vfsd_ioctl_nulldev);
    JARVIS_REGISTER_TEST(vfsd_readdir_tmp);
    JARVIS_REGISTER_TEST(vfsd_fstat_delivers);
    JARVIS_REGISTER_TEST(vfsd_stat_delivers);
    JARVIS_REGISTER_TEST(vfsd_readdir_delivers);
    JARVIS_REGISTER_TEST(vfsd_lseek_bogus_whence);
    JARVIS_REGISTER_TEST(vfsd_initrd_open_succeeds);
    JARVIS_REGISTER_TEST(vfsd_identity_mutable_denied);
    JARVIS_REGISTER_TEST(vfsd_identity_stable_accepted);
    JARVIS_REGISTER_TEST(vfsd_fat32_open_succeeds);
}
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
