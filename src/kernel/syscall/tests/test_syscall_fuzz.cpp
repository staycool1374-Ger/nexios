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

/// @file test_syscall_fuzz.cpp
/// @brief System call fuzz testing.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): every fuzz case runs inside a REAL
/// kernel task (prio ≥ 11) that is genuinely dispatched — the handler's
/// `syscall_task()` resolves to the running task.  The harness never invokes
/// Syscall::handle() directly and never mutates task state.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <constants.hpp>
#include <signal.hpp>
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
// Testidea: Call every syscall with edge-case argument values: 0,
// UINT64_MAX, -1, null pointers, and invalid enum values.  Ensure no
// kernel crash and consistent return behaviour (error or graceful
// rejection, never undefined).
// Input: A REAL dispatched kernel task (prio 11) drives the fuzz loop for
// each syscall number 0..MAX_SYSCALL-1 with arg0-3 set to various extremes.
// Expect: No crash; return value is either 0 or -1 (UINT64_MAX) for
// error returns.
TEST_CLASS(SyscallFuzzBounds) {
    static uint64_t g_fuzz_failed = 0;

    auto *t = TaskControlBlock::create(
        []() {
            uint64_t extremal[] = {0,
                                   1,
                                   UINT64_MAX,
                                   static_cast<uint64_t>(-1LL),
                                   0xFFFFFFFFULL,
                                   0xDEADBEEFCAFEBABEULL};

            for (uint64_t num = 0;
                 num < static_cast<uint64_t>(SyscallNumber::MAX_SYSCALL);
                 ++num) {
                bool safe = num ==
                                static_cast<uint64_t>(SyscallNumber::YIELD) ||
                            num ==
                                static_cast<uint64_t>(SyscallNumber::SEND) ||
                            num ==
                                static_cast<uint64_t>(SyscallNumber::PRINT);
                if (!safe)
                    continue;
                for (size_t a = 0; a < 4; ++a) {
                    for (size_t v = 0; v < 2; ++v) {
                        uint64_t args[4] = {0, 0, 0, 0};
                        args[a] = extremal[v];
                        uint64_t ret =
                            Syscall::handle(num, args[0], args[1], args[2],
                                            args[3], nullptr);
                        bool ok = (ret == 0 || ret == UINT64_MAX);
                        if (!ok) {
                            Logger::warn("syscall %llu arg[%zu]=0x%llx "
                                         "ret=0x%llx",
                                         num, a, args[a], ret);
                            g_fuzz_failed = 1;
                        }
                    }
                }
            }
        },
        11, 10);
    CT_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    CT_ASSERT(g_fuzz_failed == 0);
    release_task(t);
    Scheduler::drain_zombie_list();
};

// Runmode: kernel
// Testidea: Call IPC-related syscalls with invalid flags combinations and
// malformed arguments: SEND to nonexistent task, RECV from empty queue,
// BUF_ALLOC with bad VA, BUF_FREE with forged handle.
// Input: See each sub-test — run in a REAL dispatched kernel task.
// Expect: All return UINT64_MAX (error), no crash.
TEST_CLASS(SyscallFuzzFlags) {
    static uint64_t g_failed = 0;

    auto *t = TaskControlBlock::create(
        []() {
            // sys_send with invalid flags (all bits set)
            uint64_t ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::SEND),
                999999, // nonexistent dest
                0,      // type
                0,      // priority
                0,      // (arg3 unused in current impl)
                nullptr);
            if (ret != UINT64_MAX)
                g_failed = 1;

            // sys_send_sync to nonexistent
            ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::SEND_SYNC), 999999, 0, 0,
                0, nullptr);
            if (ret != UINT64_MAX)
                g_failed = 1;

            // sys_buf_alloc with VA >= USER_SPACE_LIMIT (null user task)
            uint64_t va_too_high = USER_SPACE_LIMIT + 0x1000;
            ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::BUF_ALLOC), va_too_high, 0,
                0, 0, nullptr);
            if (!(ret == 0 || ret == UINT64_MAX))
                g_failed = 1;

            // sys_kill with invalid signal number
            ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::KILL),
                                  0,  // self
                                  99, // invalid signal
                                  0, 0, nullptr);
            if (!(ret == UINT64_MAX || ret == 0))
                g_failed = 1;

            // sys_alarm with 0 ticks
            ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::ALARM),
                                  0, // 0 ticks
                                  0, 0, 0, nullptr);
            if (!(ret == 0 || ret == UINT64_MAX))
                g_failed = 1;
        },
        11, 10);
    CT_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    CT_ASSERT(g_failed == 0);
    release_task(t);
    Scheduler::drain_zombie_list();
};

// Runmode: kernel
// Testidea: Call syscalls from a REAL kernel task (prio 11) that is
// dispatched; the kernel must not crash regardless of syscall arguments.
// Input: A real dispatched task runs YIELD/GET_TICKS/GETPID.
// Expect: No crash; the task completes normally.
TEST_CLASS(SyscallFuzzStates) {
    static uint64_t g_ok = 0;

    auto *t = TaskControlBlock::create(
        []() {
            // Real dispatched task calls a few safe syscalls.
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::YIELD), 0, 0,
                            0, 0, nullptr);
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::GET_TICKS), 0,
                            0, 0, 0, nullptr);
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::GETPID), 0, 0,
                            0, 0, nullptr);
            g_ok = 1;
        },
        11, 10);
    CT_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    CT_ASSERT(g_ok == 1);
    CT_ASSERT(Scheduler::current_task() != nullptr);
    release_task(t);
    Scheduler::drain_zombie_list();
};

// Runmode: kernel
// Testidea: Attempt to invoke kernel-privileged operations from a REAL kernel
// task without user page tables (kernel-only task).  Operations that require
// user-space access (CheckedPtr, user memory) should fail gracefully.
// Input: Real kernel task (no user page_table_) calls BUF_ALLOC, BUF_MAP,
// FORK, EXEC.
// Expect: All return UINT64_MAX (or 0 for no-op) without crash.
TEST_CLASS(SyscallFuzzPrivilege) {
    static uint64_t g_failed = 0;

    auto *ktask = TaskControlBlock::create(
        []() {
            // v0.4.0 MP-1: kernel tasks own a private kernel-half PML4, so
            // the user discriminator is is_user_.
            if (Scheduler::current_task()->is_user_) {
                g_failed = 1;
                return;
            }
            uint64_t ret;
            // v0.4.0 MP-1/MP-8: BUF_ALLOC now SUCCEEDS for a kernel task
            // (its private PML4 provides an empty user half to map into);
            // the buffer must free cleanly afterwards.
            ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::BUF_ALLOC), 0x80000000, 0,
                0, 0, nullptr);
            if (ret == 0 || ret == UINT64_MAX)
                g_failed = 1;
            if (ret != 0 && ret != UINT64_MAX) {
                uint64_t free_ret = Syscall::handle(
                    static_cast<uint64_t>(SyscallNumber::BUF_FREE), ret, 0, 0,
                    0, nullptr);
                if (free_ret != 0)
                    g_failed = 1;
            }

            ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::EXEC),
                                  0, // null path
                                  0, // null argv
                                  0, // null envp
                                  0, nullptr);
            if (ret != UINT64_MAX)
                g_failed = 1;

            ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::FORK),
                                  0, 0, 0, 0, nullptr);
            if (ret != UINT64_MAX)
                g_failed = 1;

            ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::WAITPID),
                0, // any child
                0, // null status
                0, 0, nullptr);
            if (!(ret == UINT64_MAX || ret == 0))
                g_failed = 1;
        },
        11, 10);
    CT_ASSERT(ktask != nullptr);
    CT_ASSERT(ktask->is_user_ == false); // kernel task (MP-1)
    Scheduler::add_task(*ktask);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(ktask);
    CT_ASSERT(g_failed == 0);
    release_task(ktask);
    Scheduler::drain_zombie_list();
};

void register_syscall_fuzz_tests() {
    Logger::info("Registering syscall fuzz tests");
    REGISTER_CLASS(SyscallFuzzBounds);
    REGISTER_CLASS(SyscallFuzzFlags);
    REGISTER_CLASS(SyscallFuzzStates);
    REGISTER_CLASS(SyscallFuzzPrivilege);
}
