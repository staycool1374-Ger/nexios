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

/// @file syscall_handlers_tcb.cpp
/// @brief F4 TCB control syscalls (issue #557): TCB_CONFIGURE (99),
///        TCB_SUSPEND (100), TCB_RESUME (101).  Four-arg form only.
///        Every path is resolve -> authorize -> mechanism, every return
///        is a typed code (TCBCTL_* via the 1000-block, affinity denials
///        via the existing 800 block); no raw -1.  N1 calls
///        set_priority / set_affinity_err OUTSIDE the scheduler lock
///        (each takes it itself — INV-ADM-8).

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_errors.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/tcb_control.hpp>
#include <kernel/task/tcb_control_errors.hpp>

namespace kernel {

namespace {

/// @brief Field-mask bits for TCB_CONFIGURE (N1).
inline constexpr uint64_t kTcbCfgPriority = 1ULL << 0;
inline constexpr uint64_t kTcbCfgAffinity = 1ULL << 1;
inline constexpr uint64_t kTcbCfgSchedCtx = 1ULL << 2;
inline constexpr uint64_t kTcbCfgFaultEp = 1ULL << 3;
inline constexpr uint64_t kTcbCfgKnownMask = 0xFULL;

/// @brief Map a TcbCtlError to the syscall return encoding (0 or
///        -(1000-block)).  Zero side effects.
uint64_t tcb_ctl_return(errors::TcbCtlError err) noexcept {
    if (err == errors::TCBCTL_ERR_OK) {
        return static_cast<uint64_t>(errors::SYS_ERR_OK);
    }
    return static_cast<uint64_t>(-errors::from_tcb_control_error(
        static_cast<uint64_t>(err)));
}

/// @brief Shared resolve + authorize prologue.  Returns the resolved
///        target (or nullptr with @p out set to the typed refusal).
/// @param handle TcbCap handle (arg0).
/// @param out Receives the typed outcome.
/// @return Live authorized target, or nullptr on any refusal.
TaskControlBlock *tcb_prologue(uint64_t handle,
                               errors::TcbCtlError &out) noexcept {
    TaskControlBlock *caller = syscall_task();
    if (caller == nullptr) {
        out = errors::TCBCTL_ERR_INVALID_ARGS;
        return nullptr;
    }
    tcb_control::TcbControlResolved resolved =
        tcb_control::tcb_control_resolve(*caller, handle);
    if (resolved.err != errors::TCBCTL_ERR_OK ||
        resolved.target == nullptr) {
        out = resolved.err;
        return nullptr;
    }
    out = tcb_control::tcb_control_authorize(*caller, *resolved.target,
                                            resolved.cap);
    if (out != errors::TCBCTL_ERR_OK) {
        return nullptr;
    }
    return resolved.target;
}

} // namespace

uint64_t Syscall::sys_tcb_configure(uint64_t arg0, uint64_t arg1,
                                    uint64_t arg2, uint64_t arg3,
                                    uint64_t *) {
    const uint64_t mask = arg1;
    // No silent ignore (S3-b face): empty mask or bits outside the
    // four defined fields are INVALID_ARGS; the F4-reserved fields
    // (schedctx bind, fault endpoint) are UNSUPPORTED, not ignored.
    if (mask == 0 || (mask & ~kTcbCfgKnownMask) != 0) {
        return tcb_ctl_return(errors::TCBCTL_ERR_INVALID_ARGS);
    }
    if ((mask & (kTcbCfgSchedCtx | kTcbCfgFaultEp)) != 0) {
        return tcb_ctl_return(errors::TCBCTL_ERR_UNSUPPORTED);
    }
    errors::TcbCtlError auth = errors::TCBCTL_ERR_OK;
    TaskControlBlock *target = tcb_prologue(arg0, auth);
    if (target == nullptr) {
        return tcb_ctl_return(auth);
    }
    if ((mask & kTcbCfgPriority) != 0) {
        if (arg2 > CONFIG_PRIORITY_CEILING) {
            return tcb_ctl_return(errors::TCBCTL_ERR_INVALID_ARGS);
        }
        // Outside the scheduler lock (INV-ADM-8: set_priority takes
        // it itself).  A parked target stays configurable: the
        // re-bucket is a no-op while dequeued and takes effect on
        // resume (the target re-enqueues at the new priority).
        Scheduler::set_priority(*target, arg2);
    }
    if ((mask & kTcbCfgAffinity) != 0) {
        // Same lock discipline as priority.  Fallible denials map
        // through the existing 800 block (admission/WCET), not the
        // 1000-block — the denial is the scheduler's, not the gate's.
        const errors::SchedulerError adm =
            Scheduler::set_affinity_err(*target, arg3);
        if (adm != errors::SCHED_ERR_OK) {
            return static_cast<uint64_t>(-errors::from_sched_error(
                static_cast<uint64_t>(adm)));
        }
    }
    return static_cast<uint64_t>(errors::SYS_ERR_OK);
}

uint64_t Syscall::sys_tcb_suspend(uint64_t arg0, uint64_t arg1,
                                  uint64_t arg2, uint64_t arg3, uint64_t *) {
    if ((arg1 | arg2 | arg3) != 0) {
        return tcb_ctl_return(errors::TCBCTL_ERR_INVALID_ARGS);
    }
    errors::TcbCtlError auth = errors::TCBCTL_ERR_OK;
    TaskControlBlock *target = tcb_prologue(arg0, auth);
    if (target == nullptr) {
        return tcb_ctl_return(auth);
    }
    return tcb_ctl_return(Scheduler::admin_suspend_err(*target));
}

uint64_t Syscall::sys_tcb_resume(uint64_t arg0, uint64_t arg1,
                                 uint64_t arg2, uint64_t arg3, uint64_t *) {
    if ((arg1 | arg2 | arg3) != 0) {
        return tcb_ctl_return(errors::TCBCTL_ERR_INVALID_ARGS);
    }
    errors::TcbCtlError auth = errors::TCBCTL_ERR_OK;
    TaskControlBlock *target = tcb_prologue(arg0, auth);
    if (target == nullptr) {
        return tcb_ctl_return(auth);
    }
    return tcb_ctl_return(Scheduler::admin_resume_err(*target));
}

} // namespace kernel
