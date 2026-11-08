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

/// @file test_vfsd.cpp
/// @brief VFS daemon tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): every kernel-bypass syscall runs
/// inside a REAL kernel task (prio ≥ 11) that is genuinely dispatched — the
/// handler's `syscall_task()` resolves to the running task and the bypass
/// path is exercised through real execution.  The harness never calls
/// Syscall::handle() directly.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/vfsd.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/daemon/daemon_mgr.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {
void release_task(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    kernel::test::terminate_if_live(t);
}

/// @brief Dispatch a REAL kernel task (prio ≥ 11) running @p entry and wait
///        for genuine termination.
void run_kernel_task(void (*entry)()) {
    auto *t = TaskControlBlock::create(entry, 11, 10);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(t->is_user_ == false); // kernel task (MP-1)
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    release_task(t);
    Scheduler::drain_zombie_list();
}

/// @brief One vfsd request/response over live IPC from a dispatched kernel
///        task (fixture-safe: the daemon replies to sender_id, so the
///        caller passes its own live pid; the embedded type always matches
///        the IPC type or the daemon drops the message without reply).
///        Returns true on transport success and fills @p reply_out.
bool vfsd_roundtrip(uint64_t self_pid, uint64_t type, uint64_t arg0,
                    uint64_t arg1, const char *path, vfsd::Reply &reply_out) {
    const uint64_t daemon_pid = vfsd::get_vfsd_pid();
    if (daemon_pid == 0)
        return false;
    vfsd::Msg req{};
    req.sender_id = self_pid;
    req.type = type;
    req.arg0 = arg0;
    req.arg1 = arg1;
    if (path != nullptr) {
        size_t idx = 0;
        while (path[idx] != '\0' && idx < sizeof(req.path) - 1) {
            req.path[idx] = path[idx];
            ++idx;
        }
        req.path[idx] = '\0';
    }
    Message send{};
    send.sender_id = self_pid;
    send.type = type;
    send.data_size = sizeof(req);
    __builtin_memcpy(send.data, &req, sizeof(req));
    Message reply_msg{};
    if (!IPC::send_sync(daemon_pid, send, reply_msg))
        return false;
    if (reply_msg.data_size < sizeof(vfsd::Reply))
        return false;
    __builtin_memcpy(&reply_out, reply_msg.data, sizeof(reply_out));
    return true;
}
} // namespace

// Runmode: kernel
// Testidea: Verifies that the VFS daemon boots and registers its PID
// Input: None
// Expect: vfsd::get_vfsd_pid() returns a non-zero PID
// Depends: kernel/vfs/vfsd
JARVIS_TEST(vfsd_boots_and_registers, "PRE: vfsd, iocd | POST: none") {
    uint64_t pid = vfsd::get_vfsd_pid();
    Logger::info("[TEST:vfsd_boots_and_registers] pid=%u", pid);
    JARVIS_ASSERT(pid != 0);
    auto *task = Scheduler::find_task(pid);
    JARVIS_ASSERT(task != nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A REAL kernel task (no page_table_) calls sys_open; the
// authorization bypass runs in its own dispatched context.
// Input: Dispatch a kernel task that calls sys_open("/dev/null")
// Expect: Returns valid fd (bypass returns true without IPC)
// Depends: kernel::Syscall, kernel::Scheduler
JARVIS_TEST(vfsd_kernel_bypass_open, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_ret = 0;

    run_kernel_task([]() {
        const char *path = "/dev/null";
        g_ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::OPEN),
                                reinterpret_cast<uint64_t>(path), 0, 0, 0,
                                nullptr);
    });
    JARVIS_ASSERT(static_cast<int64_t>(g_ret) >= 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A REAL kernel task calls sys_read via the authorization bypass.
// Input: Dispatch a kernel task that opens /dev/null and reads from it.
// Expect: read returns 0 (EOF)
// Depends: kernel::Syscall, kernel::Scheduler
JARVIS_TEST(vfsd_kernel_bypass_read, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_ret = 0;

    run_kernel_task([]() {
        const char *path = "/dev/null";
        uint64_t fd =
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::OPEN),
                            reinterpret_cast<uint64_t>(path), 0, 0, 0, nullptr);
        if (static_cast<int64_t>(fd) < 0) {
            g_ret = static_cast<uint64_t>(-1);
            return;
        }
        char buf[4];
        g_ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::READ), fd,
                                reinterpret_cast<uint64_t>(buf), 4, 0, nullptr);
        uint64_t close_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CLOSE), fd, 0, 0, 0, nullptr);
        if (close_ret != 0)
            g_ret = static_cast<uint64_t>(-1);
    });
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A REAL kernel task calls sys_write via the authorization bypass.
// Input: Dispatch a kernel task that opens /dev/null and writes to it.
// Expect: write returns count (4)
// Depends: kernel::Syscall, kernel::Scheduler
JARVIS_TEST(vfsd_kernel_bypass_write, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_ret = 0;

    run_kernel_task([]() {
        const char *path = "/dev/null";
        uint64_t fd =
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::OPEN),
                            reinterpret_cast<uint64_t>(path), 1, 0, 0, nullptr);
        if (static_cast<int64_t>(fd) < 0) {
            g_ret = static_cast<uint64_t>(-1);
            return;
        }
        const char *msg = "test";
        g_ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::WRITE),
                                fd, reinterpret_cast<uint64_t>(msg), 4, 0,
                                nullptr);
        uint64_t close_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CLOSE), fd, 0, 0, 0, nullptr);
        if (close_ret != 0)
            g_ret = static_cast<uint64_t>(-1);
    });
    JARVIS_ASSERT_EQ(4ULL, g_ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A REAL kernel task calls sys_stat via the authorization bypass.
// Input: Dispatch a kernel task that calls sys_stat("/dev/null").
// Expect: Returns 0 (success)
// Depends: kernel::Syscall, kernel::Scheduler
JARVIS_TEST(vfsd_kernel_bypass_stat, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_ret = 0;

    run_kernel_task([]() {
        const char *path = "/dev/null";
        vfs::VfsStat st{};
        g_ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::STAT),
                                reinterpret_cast<uint64_t>(path),
                                reinterpret_cast<uint64_t>(&st), 0, 0, nullptr);
    });
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A REAL kernel task calls sys_fstat via the authorization bypass.
// Input: Dispatch a kernel task that opens /dev/null and calls fstat on it.
// Expect: Returns 0 (success)
// Depends: kernel::Syscall, kernel::Scheduler
JARVIS_TEST(vfsd_kernel_bypass_fstat, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_ret = 0;

    run_kernel_task([]() {
        const char *path = "/dev/null";
        uint64_t fd =
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::OPEN),
                            reinterpret_cast<uint64_t>(path), 0, 0, 0, nullptr);
        if (static_cast<int64_t>(fd) < 0) {
            g_ret = static_cast<uint64_t>(-1);
            return;
        }
        vfs::VfsStat st{};
        g_ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::FSTAT),
                                fd, reinterpret_cast<uint64_t>(&st), 0, 0,
                                nullptr);
        uint64_t close_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CLOSE), fd, 0, 0, 0, nullptr);
        if (close_ret != 0)
            g_ret = static_cast<uint64_t>(-1);
    });
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A REAL kernel task calls sys_chdir via the authorization bypass.
// Input: Dispatch a kernel task that calls sys_chdir("/").
// Expect: Returns 0 (success)
// Depends: kernel::Syscall, kernel::Scheduler
JARVIS_TEST(vfsd_kernel_bypass_chdir, "PRE: vfsd, iocd | POST: none") {
    static uint64_t g_ret = 0;

    run_kernel_task([]() {
        const char *path = "/";
        g_ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::CHDIR),
                                reinterpret_cast<uint64_t>(path), 0, 0, 0,
                                nullptr);
    });
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: VFS daemon answers read/write authorization over live IPC: a
// REAL dispatched kernel task sends VFS_READ + VFS_WRITE and the daemon's
// auth-only handlers acknowledge (result 0, no mutation — the kernel
// performs the actual I/O after approval).
// Input: dispatched task drives VFS_READ then VFS_WRITE via send_sync;
// harness joins and asserts
// Expect: both transports succeed and both replies carry result 0
// Depends: vfsd daemon, live IPC, Scheduler
JARVIS_TEST(vfsd_handle_read_write, "PRE: vfsd, iocd | POST: none") {
    static bool g_read_ok = false;
    static int64_t g_read_result = -99;
    static bool g_write_ok = false;
    static int64_t g_write_result = -99;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            const uint64_t self = cur ? cur->id : 0;
            vfsd::Reply reply{};
            g_read_ok =
                vfsd_roundtrip(self, vfsd::VFS_READ, 0, 0, nullptr, reply);
            g_read_result = reply.result;
            g_write_ok =
                vfsd_roundtrip(self, vfsd::VFS_WRITE, 0, 0, nullptr, reply);
            g_write_result = reply.result;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    release_task(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(g_read_ok);
    JARVIS_ASSERT_FMT(g_read_result == 0, "VFS_READ: %ld", g_read_result);
    JARVIS_ASSERT(g_write_ok);
    JARVIS_ASSERT_FMT(g_write_result == 0, "VFS_WRITE: %ld", g_write_result);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: VFS daemon answers open authorization over live IPC: VFS_OPEN
// of an existing path is granted (daemon opens + closes in its own
// context, replies the fd), while VFS_OPEN of a missing path stays
// denied (creation is reachable only through VFS_OPEN_CREATE, #341).
// Input: dispatched task drives VFS_OPEN "/dev/null" then
// VFS_OPEN "/nonexistent-vfsd-probe"; harness joins and asserts
// Expect: existing path result >= 0; missing path result -1
// Depends: vfsd daemon, live IPC, Scheduler
JARVIS_TEST(vfsd_handle_open, "PRE: vfsd, iocd | POST: none") {
    static bool g_open_ok = false;
    static int64_t g_open_result = -99;
    static bool g_missing_ok = false;
    static int64_t g_missing_result = -99;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            const uint64_t self = cur ? cur->id : 0;
            vfsd::Reply reply{};
            g_open_ok = vfsd_roundtrip(self, vfsd::VFS_OPEN, 0, 0,
                                       "/dev/null", reply);
            g_open_result = reply.result;
            g_missing_ok = vfsd_roundtrip(self, vfsd::VFS_OPEN, 0, 0,
                                          "/nonexistent-vfsd-probe", reply);
            g_missing_result = reply.result;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    release_task(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(g_open_ok);
    JARVIS_ASSERT_FMT(g_open_result >= 0, "VFS_OPEN /dev/null: %ld",
                      g_open_result);
    JARVIS_ASSERT(g_missing_ok);
    JARVIS_ASSERT_FMT(g_missing_result == -1, "VFS_OPEN missing: %ld",
                      g_missing_result);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: VFS daemon close path acknowledges over live IPC: close is
// infallible by FdTable design (free() silently ignores unused and
// out-of-range fds), so the daemon reports success (result 0) even for
// a wild fd — the probe pins the round-trip and the infallible-close
// contract, never handing the daemon a live fd.
// Input: dispatched task drives VFS_CLOSE with fd 4242; harness asserts
// Expect: transport succeeds, reply result 0 (infallible close)
// Depends: vfsd daemon, live IPC, FdTable::free, Scheduler
JARVIS_TEST(vfsd_handle_close, "PRE: vfsd, iocd | POST: none") {
    static bool g_close_ok = false;
    static int64_t g_close_result = -99;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            const uint64_t self = cur ? cur->id : 0;
            vfsd::Reply reply{};
            g_close_ok = vfsd_roundtrip(self, vfsd::VFS_CLOSE, 4242, 0,
                                        nullptr, reply);
            g_close_result = reply.result;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    release_task(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(g_close_ok);
    JARVIS_ASSERT_FMT(g_close_result == 0, "VFS_CLOSE wild: %ld",
                      g_close_result);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: VFS daemon answers resolve/stat over live IPC: VFS_STAT of an
// existing path succeeds, of a missing path fails — the resolve half of
// every authorized stat path.
// Input: dispatched task drives VFS_STAT "/dev/null" then
// VFS_STAT "/nonexistent-vfsd-probe"; harness joins and asserts
// Expect: existing path result 0; missing path result -1
// Depends: vfsd daemon, live IPC, Scheduler
JARVIS_TEST(vfsd_handle_resolve, "PRE: vfsd, iocd | POST: none") {
    static bool g_stat_ok = false;
    static int64_t g_stat_result = -99;
    static bool g_missing_ok = false;
    static int64_t g_missing_result = -99;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            const uint64_t self = cur ? cur->id : 0;
            vfsd::Reply reply{};
            g_stat_ok = vfsd_roundtrip(self, vfsd::VFS_STAT, 0, 0,
                                       "/dev/null", reply);
            g_stat_result = reply.result;
            g_missing_ok = vfsd_roundtrip(self, vfsd::VFS_STAT, 0, 0,
                                          "/nonexistent-vfsd-probe", reply);
            g_missing_result = reply.result;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    release_task(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(g_stat_ok);
    JARVIS_ASSERT_FMT(g_stat_result == 0, "VFS_STAT /dev/null: %ld",
                      g_stat_result);
    JARVIS_ASSERT(g_missing_ok);
    JARVIS_ASSERT_FMT(g_missing_result == -1, "VFS_STAT missing: %ld",
                      g_missing_result);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: VFS daemon denies mount/unmount over live IPC: the daemon has
// no mount implementation and must fail closed (result -1), never
// silently succeed.
// Input: dispatched task drives VFS_MOUNT; harness joins and asserts
// Expect: transport succeeds, reply result -1
// Depends: vfsd daemon, live IPC, Scheduler
JARVIS_TEST(vfsd_handle_mount_unmount, "PRE: vfsd, iocd | POST: none") {
    static bool g_mount_ok = false;
    static int64_t g_mount_result = -99;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            const uint64_t self = cur ? cur->id : 0;
            vfsd::Reply reply{};
            g_mount_ok = vfsd_roundtrip(self, vfsd::VFS_MOUNT, 0, 0,
                                        nullptr, reply);
            g_mount_result = reply.result;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    release_task(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(g_mount_ok);
    JARVIS_ASSERT_FMT(g_mount_result == -1, "VFS_MOUNT: %ld", g_mount_result);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: VFS daemon answers stat/fstat over live IPC: VFS_FSTAT is an
// auth-only ack (result 0 — the kernel stats the caller's own vnode after
// approval), while VFS_STAT of /dev/null succeeds and carries a
// non-empty mode word.
// Input: dispatched task drives VFS_FSTAT then VFS_STAT "/dev/null";
// harness joins and asserts
// Expect: FSTAT result 0; STAT result 0 with data1 (mode) != 0
// Depends: vfsd daemon, live IPC, Scheduler
JARVIS_TEST(vfsd_handle_stat_fstat, "PRE: vfsd, iocd | POST: none") {
    static bool g_fstat_ok = false;
    static int64_t g_fstat_result = -99;
    static bool g_stat_ok = false;
    static int64_t g_stat_result = -99;
    static uint64_t g_stat_mode = 0;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            const uint64_t self = cur ? cur->id : 0;
            vfsd::Reply reply{};
            g_fstat_ok = vfsd_roundtrip(self, vfsd::VFS_FSTAT, 0, 0,
                                        nullptr, reply);
            g_fstat_result = reply.result;
            g_stat_ok = vfsd_roundtrip(self, vfsd::VFS_STAT, 0, 0,
                                       "/dev/null", reply);
            g_stat_result = reply.result;
            g_stat_mode = reply.data1;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    release_task(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(g_fstat_ok);
    JARVIS_ASSERT_FMT(g_fstat_result == 0, "VFS_FSTAT: %ld", g_fstat_result);
    JARVIS_ASSERT(g_stat_ok);
    JARVIS_ASSERT_FMT(g_stat_result == 0, "VFS_STAT /dev/null: %ld",
                      g_stat_result);
    JARVIS_ASSERT_FMT(g_stat_mode != 0, "STAT mode word empty");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: VFS daemon answers chdir/getcwd over live IPC: VFS_CHDIR of a
// directory succeeds, of a non-directory fails, and VFS_GETCWD is denied
// (unimplemented — fails closed).
// Input: dispatched task drives VFS_CHDIR "/", VFS_CHDIR "/dev/null",
// VFS_GETCWD; harness joins and asserts
// Expect: "/" result 0; "/dev/null" result -1; GETCWD result -1
// Depends: vfsd daemon, live IPC, Scheduler
JARVIS_TEST(vfsd_handle_chdir_getcwd, "PRE: vfsd, iocd | POST: none") {
    static bool g_dir_ok = false;
    static int64_t g_dir_result = -99;
    static bool g_file_ok = false;
    static int64_t g_file_result = -99;
    static bool g_getcwd_ok = false;
    static int64_t g_getcwd_result = -99;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            const uint64_t self = cur ? cur->id : 0;
            vfsd::Reply reply{};
            g_dir_ok = vfsd_roundtrip(self, vfsd::VFS_CHDIR, 0, 0, "/",
                                      reply);
            g_dir_result = reply.result;
            g_file_ok = vfsd_roundtrip(self, vfsd::VFS_CHDIR, 0, 0,
                                       "/dev/null", reply);
            g_file_result = reply.result;
            g_getcwd_ok = vfsd_roundtrip(self, vfsd::VFS_GETCWD, 0, 0,
                                         nullptr, reply);
            g_getcwd_result = reply.result;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    release_task(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(g_dir_ok);
    JARVIS_ASSERT_FMT(g_dir_result == 0, "VFS_CHDIR /: %ld", g_dir_result);
    JARVIS_ASSERT(g_file_ok);
    JARVIS_ASSERT_FMT(g_file_result == -1, "VFS_CHDIR /dev/null: %ld",
                      g_file_result);
    JARVIS_ASSERT(g_getcwd_ok);
    JARVIS_ASSERT_FMT(g_getcwd_result == -1, "VFS_GETCWD: %ld",
                      g_getcwd_result);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: VFS daemon rejects an unknown message type over live IPC: the
// dispatch default denies (result -1) instead of hanging or crashing —
// the fail-closed contract for out-of-range op codes.
// Input: dispatched task drives op type 999; harness joins and asserts
// Expect: transport succeeds (daemon replies), reply result -1
// Depends: vfsd daemon, live IPC, Scheduler
JARVIS_TEST(vfsd_invalid_message_type_rejected,
            "PRE: vfsd, iocd | POST: none") {
    static bool g_msg_ok = false;
    static int64_t g_msg_result = -99;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            const uint64_t self = cur ? cur->id : 0;
            vfsd::Reply reply{};
            g_msg_ok =
                vfsd_roundtrip(self, 999, 0, 0, nullptr, reply);
            g_msg_result = reply.result;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    release_task(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(g_msg_ok);
    JARVIS_ASSERT_FMT(g_msg_result == -1, "op 999: %ld", g_msg_result);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: VFS daemon rejects a malformed open (empty path) over live
// IPC: open("") can never resolve, so the daemon must deny (result -1)
// instead of crashing on the empty name.
// Input: dispatched task drives VFS_OPEN with ""; harness joins and asserts
// Expect: transport succeeds, reply result -1
// Depends: vfsd daemon, live IPC, Scheduler
JARVIS_TEST(vfsd_malformed_message_rejected, "PRE: vfsd, iocd | POST: none") {
    static bool g_msg_ok = false;
    static int64_t g_msg_result = -99;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            const uint64_t self = cur ? cur->id : 0;
            vfsd::Reply reply{};
            g_msg_ok =
                vfsd_roundtrip(self, vfsd::VFS_OPEN, 0, 0, "", reply);
            g_msg_result = reply.result;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    release_task(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(g_msg_ok);
    JARVIS_ASSERT_FMT(g_msg_result == -1, "VFS_OPEN empty: %ld",
                      g_msg_result);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: VFS daemon denies unauthorized creation over live IPC: a plain
// VFS_OPEN of a missing path stays denied (creation needs the
// create-intent op, #341), VFS_OPEN_CREATE outside /tmp/ is denied, and
// dotfiles directly under /tmp/ are denied — while a well-formed
// /tmp/ leaf is approved (auth-only, no mutation by the daemon).
// Input: dispatched task drives the four opens; harness joins and asserts
// Expect: missing -1, outside-tmp -1, dotfile -1, /tmp/ leaf 0
// Depends: vfsd daemon, live IPC, Scheduler
JARVIS_TEST(vfsd_unauthorized_task_rejected, "PRE: vfsd, iocd | POST: none") {
    static bool g_missing_ok = false;
    static int64_t g_missing_result = -99;
    static bool g_outside_ok = false;
    static int64_t g_outside_result = -99;
    static bool g_dot_ok = false;
    static int64_t g_dot_result = -99;
    static bool g_leaf_ok = false;
    static int64_t g_leaf_result = -99;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            const uint64_t self = cur ? cur->id : 0;
            vfsd::Reply reply{};
            g_missing_ok = vfsd_roundtrip(self, vfsd::VFS_OPEN, 0, 0,
                                          "/no-such-vfsd-file", reply);
            g_missing_result = reply.result;
            g_outside_ok = vfsd_roundtrip(self, vfsd::VFS_OPEN_CREATE, 0, 0,
                                          "/etc/vfsd-nope", reply);
            g_outside_result = reply.result;
            g_dot_ok = vfsd_roundtrip(self, vfsd::VFS_OPEN_CREATE, 0, 0,
                                      "/tmp/.hidden", reply);
            g_dot_result = reply.result;
            g_leaf_ok = vfsd_roundtrip(self, vfsd::VFS_OPEN_CREATE, 0, 0,
                                       "/tmp/qe-probe", reply);
            g_leaf_result = reply.result;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    release_task(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(g_missing_ok);
    JARVIS_ASSERT_FMT(g_missing_result == -1, "OPEN missing: %ld",
                      g_missing_result);
    JARVIS_ASSERT(g_outside_ok);
    JARVIS_ASSERT_FMT(g_outside_result == -1, "CREATE outside /tmp: %ld",
                      g_outside_result);
    JARVIS_ASSERT(g_dot_ok);
    JARVIS_ASSERT_FMT(g_dot_result == -1, "CREATE dotfile: %ld",
                      g_dot_result);
    JARVIS_ASSERT(g_leaf_ok);
    JARVIS_ASSERT_FMT(g_leaf_result == 0, "CREATE /tmp leaf: %ld",
                      g_leaf_result);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: VFS daemon serves concurrent requests over live IPC: two REAL
// dispatched kernel tasks each drive VFS_STAT "/dev/null" and both must
// be approved (result 0) — the daemon serializes without dropping or
// cross-wiring replies.
// Input: two dispatched tasks drive VFS_STAT concurrently; harness joins
// both and asserts
// Expect: both transports succeed, both replies carry result 0
// Depends: vfsd daemon, live IPC, Scheduler
JARVIS_TEST(vfsd_concurrent_requests, "PRE: vfsd, iocd | POST: none") {
    static bool g_first_ok = false;
    static int64_t g_first_result = -99;
    static bool g_second_ok = false;
    static int64_t g_second_result = -99;

    auto *first = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            const uint64_t self = cur ? cur->id : 0;
            vfsd::Reply reply{};
            g_first_ok = vfsd_roundtrip(self, vfsd::VFS_STAT, 0, 0,
                                        "/dev/null", reply);
            g_first_result = reply.result;
        },
        11, 10);
    auto *second = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            const uint64_t self = cur ? cur->id : 0;
            vfsd::Reply reply{};
            g_second_ok = vfsd_roundtrip(self, vfsd::VFS_STAT, 0, 0,
                                         "/dev/null", reply);
            g_second_result = reply.result;
        },
        11, 10);
    JARVIS_ASSERT(first != nullptr);
    JARVIS_ASSERT(second != nullptr);
    Scheduler::add_task(*first);
    Scheduler::add_task(*second);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(first);
    kernel::test::wait_for_termination_safe(second);
    release_task(first);
    release_task(second);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(g_first_ok);
    JARVIS_ASSERT_FMT(g_first_result == 0, "first STAT: %ld",
                      g_first_result);
    JARVIS_ASSERT(g_second_ok);
    JARVIS_ASSERT_FMT(g_second_result == 0, "second STAT: %ld",
                      g_second_result);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all VFS daemon tests with the test framework
// Input: None
// Expect: All VFS daemon tests registered
// Depends: test framework
void register_vfsd_tests() {
    Logger::info("Registering VFS daemon tests");
    JARVIS_REGISTER_TEST(vfsd_boots_and_registers);
    JARVIS_REGISTER_TEST(vfsd_kernel_bypass_open);
    JARVIS_REGISTER_TEST(vfsd_kernel_bypass_read);
    JARVIS_REGISTER_TEST(vfsd_kernel_bypass_write);
    JARVIS_REGISTER_TEST(vfsd_kernel_bypass_stat);
    JARVIS_REGISTER_TEST(vfsd_kernel_bypass_fstat);
    JARVIS_REGISTER_TEST(vfsd_kernel_bypass_chdir);
    JARVIS_REGISTER_TEST(vfsd_handle_read_write);
    JARVIS_REGISTER_TEST(vfsd_handle_open);
    JARVIS_REGISTER_TEST(vfsd_handle_close);
    JARVIS_REGISTER_TEST(vfsd_handle_resolve);
    JARVIS_REGISTER_TEST(vfsd_handle_mount_unmount);
    JARVIS_REGISTER_TEST(vfsd_handle_stat_fstat);
    JARVIS_REGISTER_TEST(vfsd_handle_chdir_getcwd);
    JARVIS_REGISTER_TEST(vfsd_invalid_message_type_rejected);
    JARVIS_REGISTER_TEST(vfsd_malformed_message_rejected);
    JARVIS_REGISTER_TEST(vfsd_unauthorized_task_rejected);
    JARVIS_REGISTER_TEST(vfsd_concurrent_requests);
    // DISABLED: deliberately kills daemon manually, leaving daemon lifecycle
    // in a non-standard state that snapshot_restore cannot clean up
    // JARVIS_REGISTER_TEST(vfsd_crash_restarts);
    // DISABLED: exhausts daemon restart budget, leaving daemon permanently
    // dead which breaks all subsequent tests that need vfsd
    // JARVIS_REGISTER_TEST(vfsd_exhaust_restart_limit);
}
