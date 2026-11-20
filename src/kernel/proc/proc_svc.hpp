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

/// @file proc_svc.hpp
/// @brief Proc-mgr service layer (F11 Stage A, #564): owns the CURRENT
///        process-syscall bodies (fork/exec/waitpid/getpid/kill/signal/
///        sigreturn), moved statement-identical out of
///        syscall_handlers_process.cpp / syscall_handlers_misc.cpp.
///        The Syscall::sys_* wrappers keep their exact signatures, FAST/
///        FULL classification and caller-context mechanism, and delegate
///        here — behavior-identity with the knob OFF (INV-PROC-7). Stage B
///        adds the proxy verdict call inside the bodies (still in caller
///        context, INV-PROC-1); handle_decision() is the verdict validator
///        for that path.

#pragma once

#include <types.hpp>
#include <kernel/proc/proc_errors.hpp>
#include <kernel/proc/proc_wire.hpp>

namespace kernel::proc {

/// @brief Service owner of the process-syscall mechanisms (all static,
///        syscall-context only, never ISR).
class ProcSvc {
  public:
    /// @brief Clone the calling task (moved from Syscall::sys_fork).
    /// @param regs Caller trap frame (child context derives from it).
    /// @return Child id, or SYS_ERR_GENERIC_FAIL.
    static uint64_t fork(uint64_t, uint64_t, uint64_t, uint64_t,
                         uint64_t *regs);

    /// @brief Reap or await a child (moved from Syscall::sys_waitpid).
    /// @param target_pid Child id, or (uint64_t)-1 for any child.
    /// @param status_va User out-param for the exit code (validated).
    /// @param wait_flags WNOHANG bit (bit 0) and future flags.
    /// @return Reaped child id, 0 (WNOHANG, nothing ready), or a typed fail.
    static uint64_t waitpid(uint64_t target_pid, uint64_t status_va,
                            uint64_t wait_flags, uint64_t, uint64_t *);

    /// @brief Replace the calling image (moved from Syscall::sys_exec).
    /// @param regs Caller trap frame (restored into the new image).
    /// @return SYS_ERR_OK on success (no return to caller), fail otherwise.
    static uint64_t exec(uint64_t, uint64_t, uint64_t, uint64_t,
                         uint64_t *regs);

    /// @brief Deliver a signal (moved from Syscall::sys_kill).
    /// @return SYS_ERR_OK on delivery, SYS_ERR_GENERIC_FAIL otherwise.
    static uint64_t kill(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t *);

    /// @brief Install a signal handler (moved from Syscall::sys_signal).
    /// @return SYS_ERR_OK on install, SYS_ERR_GENERIC_FAIL otherwise.
    static uint64_t signal(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t *);

    /// @brief Restore a signal-interrupted trap frame (moved from
    ///        Syscall::sys_sigreturn; mechanism only, no proxy leg).
    /// @param regs Caller trap frame (restored in place).
    /// @return SYS_ERR_OK on restore, SYS_ERR_GENERIC_FAIL otherwise.
    static uint64_t sigreturn(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t *regs);

    /// @brief O(1) identity read (moved from Syscall::sys_getpid).
    /// @return Caller id, or 0 when no calling task exists.
    static uint64_t getpid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t *);

    /// @brief Validate a proc-mgr verdict (Stage B entry point).
    ///        Unknown op or non-zero reserved fields refuse with
    ///        BAD_REQUEST; STOP/RESUME refuse with STOP_REFUSED until a PD
    ///        policy owns the disposition (Stage A has no PD yet).
    /// @return The mapped ProcError (OK = verdict accepted for dispatch).
    static errors::ProcError
    handle_decision(const ProcMgrRpc &decision) noexcept;
};

} // namespace kernel::proc
