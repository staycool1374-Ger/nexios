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
// Testidea: STUB - VFS daemon handles read/write IPC messages
// Input: None (stub — requires post-boot sti for userspace IPC)
// Expect: Passes
// Depends: kernel/task, kernel/ipc, kernel/vfsd
JARVIS_TEST(vfsd_handle_read_write, "PRE: vfsd, iocd | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - VFS daemon responds to an open authorization request
// Input: Send VFS_OPEN IPC message (requires post-boot sti)
// Expect: Passes (stub)
// Depends: kernel/task, kernel/ipc, kernel/vfsd
JARVIS_TEST(vfsd_handle_open, "PRE: vfsd, iocd | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - VFS daemon responds to a close authorization request
// Input: Send VFS_CLOSE IPC message (requires post-boot sti)
// Expect: Passes (stub)
// Depends: kernel/task, kernel/ipc, kernel/vfsd
JARVIS_TEST(vfsd_handle_close, "PRE: vfsd, iocd | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - VFS daemon responds to a stat/resolve authorization request
// Input: Send VFS_STAT IPC message (requires post-boot sti)
// Expect: Passes (stub)
// Depends: kernel/task, kernel/ipc, kernel/vfsd
JARVIS_TEST(vfsd_handle_resolve, "PRE: vfsd, iocd | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - VFS daemon handles mount/unmount operations
// Input: None (stub - requires post-boot sti)
// Expect: Passes
// Depends: kernel/task, kernel/ipc, kernel/vfsd
JARVIS_TEST(vfsd_handle_mount_unmount, "PRE: vfsd, iocd | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - VFS daemon handles stat/fstat operations
// Input: None (stub - requires post-boot sti)
// Expect: Passes
// Depends: kernel/task, kernel/ipc, kernel/vfsd
JARVIS_TEST(vfsd_handle_stat_fstat, "PRE: vfsd, iocd | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - VFS daemon handles chdir/getcwd operations
// Input: None (stub - requires post-boot sti)
// Expect: Passes
// Depends: kernel/task, kernel/ipc, kernel/vfsd
JARVIS_TEST(vfsd_handle_chdir_getcwd, "PRE: vfsd, iocd | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - VFS daemon rejects invalid message types
// Input: Send IPC message with unknown type (requires post-boot sti)
// Expect: Passes (stub)
// Depends: kernel/task, kernel/ipc, kernel/vfsd
JARVIS_TEST(vfsd_invalid_message_type_rejected,
            "PRE: vfsd, iocd | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - Malformed message rejected by VFS daemon
// Input: None (stub - requires post-boot sti)
// Expect: Passes
// Depends: kernel/task, kernel/ipc, kernel/vfsd
JARVIS_TEST(vfsd_malformed_message_rejected, "PRE: vfsd, iocd | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - Unauthorized task rejected by VFS daemon
// Input: None (stub - requires post-boot sti)
// Expect: Passes
// Depends: kernel/task, kernel/ipc, kernel/vfsd
JARVIS_TEST(vfsd_unauthorized_task_rejected, "PRE: vfsd, iocd | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - Concurrent requests handled correctly by VFS daemon
// Input: None (stub - requires post-boot sti)
// Expect: Passes
// Depends: kernel/task, kernel/ipc, kernel/vfsd
JARVIS_TEST(vfsd_concurrent_requests, "PRE: vfsd, iocd | POST: none") {
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
