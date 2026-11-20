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

/// @file syscall_handlers_process.cpp
/// @brief Syscall handlers for process operations: exec, fork, waitpid,
///        kill, signal, affinity, times. The seven proc-mgr mechanisms
///        (fork/waitpid/exec/kill/signal/sigreturn) live in
///        kernel::proc::ProcSvc (proc_svc.cpp, statement-identical bodies);
///        the wrappers below keep the exact Syscall signatures and delegate
///        — with CONFIG_PROC_MGR=0 the kernel path runs unchanged
///        (INV-PROC-7). Affinity/times stay here (not proc-mgr ops).

#include <kernel/proc/proc_svc.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_errors.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/test/test_isolate.hpp>
#if defined(CONFIG_DEBUG)
#include <kernel/test/wait_publish_seam.hpp>
#endif
#include <signal.hpp>
#include <constants.hpp>
#include <string.hpp>

namespace kernel {

uint64_t Syscall::sys_fork(uint64_t, uint64_t, uint64_t, uint64_t,
                           uint64_t *regs) {
    return proc::ProcSvc::fork(0, 0, 0, 0, regs);
}

uint64_t Syscall::sys_waitpid(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                              uint64_t arg3, uint64_t *regs) {
    return proc::ProcSvc::waitpid(arg0, arg1, arg2, arg3, regs);
}

uint64_t Syscall::sys_exec(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                           uint64_t arg3, uint64_t *regs) {
    return proc::ProcSvc::exec(arg0, arg1, arg2, arg3, regs);
}

uint64_t Syscall::sys_kill(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                           uint64_t arg3, uint64_t *regs) {
    return proc::ProcSvc::kill(arg0, arg1, arg2, arg3, regs);
}

uint64_t Syscall::sys_signal(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                             uint64_t arg3, uint64_t *regs) {
    return proc::ProcSvc::signal(arg0, arg1, arg2, arg3, regs);
}

/// @brief Set a task's CPU affinity mask (issue #61).
///        arg0 = pid (0 = caller), arg1 = mask.  Clamp policy lives in
///        Scheduler::set_affinity (empty→CPU0, user→CPU0, beyond-up
///        bits cut); unknown pid fails with -1.  Mask passed by value —
///        no user-pointer dereference (FULL path only: scheduler call).
uint64_t Syscall::sys_set_affinity(uint64_t arg0, uint64_t arg1, uint64_t,
                                   uint64_t, uint64_t *) {
    // Issue #374: fail fast on never-issued IDs without probing.
    if (arg0 != 0 && !Scheduler::validate_id(arg0))
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    auto *t = (arg0 == 0) ? syscall_task() : Scheduler::find_task(arg0);
    if (t == nullptr)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    // Issue #23: destination-CPU admission denial propagates as -1
    // (fail-closed); clamp/no-op paths still succeed with 0.
    if (Scheduler::set_affinity_err(*t, arg1) != errors::SCHED_ERR_OK)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    return static_cast<uint64_t>(errors::SYS_ERR_OK);
}

/// @brief Get a task's CPU affinity mask (issue #61).
///        arg0 = pid (0 = caller).  Unknown pid fails with -1.
uint64_t Syscall::sys_get_affinity(uint64_t arg0, uint64_t, uint64_t,
                                   uint64_t, uint64_t *) {
    // Issue #374: fail fast on never-issued IDs without probing.
    if (arg0 != 0 && !Scheduler::validate_id(arg0))
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    auto *t = (arg0 == 0) ? syscall_task() : Scheduler::find_task(arg0);
    if (t == nullptr)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    return Scheduler::get_affinity(*t);
}

/// @brief Read a task's execution-time accounting (issue #21).
///        arg0 = pid (0 = caller), arg1 = user TaskTimes* out-param.
///        Snapshot via Scheduler::read_times (charge-before-read for the
///        running task); copied out with safe_copy_to_user (stac-wrapped).
///        Unknown pid or bad buffer fails with -1 (fuzz-safe).  Non-blocking;
///        FULL path only (dereferences a user pointer — never FAST).
uint64_t Syscall::sys_times(uint64_t arg0, uint64_t arg1, uint64_t, uint64_t,
                            uint64_t *) {
    // Issue #374: fail fast on never-issued IDs without probing.
    if (arg0 != 0 && !Scheduler::validate_id(arg0))
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    auto *t = (arg0 == 0) ? syscall_task() : Scheduler::find_task(arg0);
    if (t == nullptr)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *out = reinterpret_cast<TaskTimes *>(arg1);
    auto chk = checked(out, 1);
    if (!chk.valid())
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    TaskTimes kt{};
    Scheduler::read_times(*t, kt);
    if (!safe_copy_to_user(chk.unsafe_ptr(), &kt, 1))
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    return static_cast<uint64_t>(errors::SYS_ERR_OK);
}

uint64_t Syscall::sys_sigreturn(uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t *regs) {
    return proc::ProcSvc::sigreturn(0, 0, 0, 0, regs);
}

} // namespace kernel
