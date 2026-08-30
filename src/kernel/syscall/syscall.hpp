#pragma once

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

/// @file syscall.hpp
/// @brief System call interface and dispatcher.

#pragma once

#include <types.hpp>

namespace kernel {

/// @brief System call numbers recognised by the kernel.
enum class SyscallNumber : uint8_t {
    YIELD = 0,
    SEND = 1,
    RECEIVE = 2,
    SEND_SYNC = 3,
    PRINT = 4,
    GET_TICKS = 5,
    EXIT = 6,
    CREATE_MAILBOX = 7,
    DESTROY_MAILBOX = 8,
    OPEN = 9,
    READ = 10,
    CLOSE = 11,
    FSTAT = 12,
    WRITE = 13,
    LSEEK = 14,
    IOCTL = 15,
    READDIR = 16,
    STAT = 17,
    DUP = 18,
    CHDIR = 19,
    EXEC = 20,
    FORK = 21,
    WAITPID = 22,
    GETPID = 23,
    KILL = 24,
    PIPE = 25,
    DUP2 = 26,
    NOTIFY = 27,
    NOTIFY_WAIT = 28,
    EVENT_SET = 29,
    EVENT_WAIT = 30,
    SIGNAL = 31,
    SIGRETURN = 32,
    ALARM = 33,
    GETTOD = 34,
    UNAME = 35,
    PAUSE = 36,
    BUF_ALLOC = 37,
    BUF_FREE = 38,
    BUF_MAP = 39,
    BUF_UNMAP = 40,
    MKDIR = 41,
    UNLINK = 42,
    RMDIR = 43,
    BRK = 44,
    GETRLIMIT = 45,
    SETRLIMIT = 46,
    GETRANDOM = 47,
    KLOG = 48,
    REBOOT = 49,
    HALT = 50,
    CAP_GRANT = 51,
    CAP_COPY = 52,
    CAP_REVOKE = 53,
    CAP_MINT = 54,
    IOPORT_GRANT = 55, ///< x86_64 fine-grained I/O delegation (issue #3)
    CAP_RETYPE = 56,   ///< Untyped sub-range carve + child remainder (issue #1)
    IRQ_REGISTER = 57, ///< arm user-space IRQ delivery for an IrqCap (issue #2)
    IRQ_WAIT = 58,     ///< block until the registered IRQ fires (issue #2)
    MAX_SYSCALL = 59,
};

/// @brief System call handler function signature.
using SyscallHandler = uint64_t (*)(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                    uint64_t arg3, uint64_t *regs);

/// @brief v0.4.0 MP-3 canary-trip latch.  Set by the syscall canary verify
///        when a segment canary mismatch is detected in test mode; the
///        syscall then returns -1 so the harness survives.
struct CanaryTrip {
    uint64_t task_id;
    uint8_t segment;
    uint64_t rip;
    uint64_t count;
};

/// @brief System call handler and dispatcher.
class Syscall {
  public:
    /// @brief Initialize syscall table and MSRs (STAR, LSTAR, FMASK).
    static void init();
    /// @brief Dispatch a system call to the appropriate handler.
    /// @param number Syscall number (see SyscallNumber).
    /// @param arg0-3 Arguments passed from user space.
    /// @param regs Pointer to register save area (for context-modifying calls).
    /// @return The syscall return value.
    static uint64_t handle(uint64_t number, uint64_t arg0, uint64_t arg1,
                           uint64_t arg2, uint64_t arg3,
                           uint64_t *regs = nullptr);

  private:
    static uint64_t sys_yield(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t *);
    static uint64_t sys_send(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t *);
    static uint64_t sys_receive(uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t *);
    static uint64_t sys_send_sync(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t *);
    static uint64_t sys_print(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t *);
    static uint64_t sys_get_ticks(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t *);
    static uint64_t sys_exit(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t *);
    static uint64_t sys_create_mailbox(uint64_t, uint64_t, uint64_t, uint64_t,
                                       uint64_t *);
    static uint64_t sys_destroy_mailbox(uint64_t, uint64_t, uint64_t, uint64_t,
                                        uint64_t *);
    static uint64_t sys_open(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t *);
    static uint64_t sys_read(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t *);
    static uint64_t sys_close(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t *);
    static uint64_t sys_fstat(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t *);
    static uint64_t sys_write(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t *);
    static uint64_t sys_lseek(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t *);
    static uint64_t sys_ioctl(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t *);
    static uint64_t sys_readdir(uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t *);
    static uint64_t sys_stat(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t *);
    static uint64_t sys_dup(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t *);
    static uint64_t sys_chdir(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t *);
    static uint64_t sys_exec(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t *);
    static uint64_t sys_fork(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t *);
    static uint64_t sys_waitpid(uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t *);
    static uint64_t sys_getpid(uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t *);
    static uint64_t sys_kill(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t *);
    static uint64_t sys_pipe(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t *);
    static uint64_t sys_dup2(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t *);
    static uint64_t sys_notify(uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t *);
    static uint64_t sys_notify_wait(uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t *);
    static uint64_t sys_event_set(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t *);
    static uint64_t sys_event_wait(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t *);
    static uint64_t sys_signal(uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t *);
    static uint64_t sys_sigreturn(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t *);
    static uint64_t sys_alarm(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t *);
    static uint64_t sys_gettod(uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t *);
    static uint64_t sys_uname(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t *);
    static uint64_t sys_pause(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t *);
    static uint64_t sys_buf_alloc(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t *);
    static uint64_t sys_buf_free(uint64_t, uint64_t, uint64_t, uint64_t,
                                 uint64_t *);
    static uint64_t sys_buf_map(uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t *);
    static uint64_t sys_buf_unmap(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t *);
    static uint64_t sys_mkdir(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t *);
    static uint64_t sys_unlink(uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t *);
    static uint64_t sys_rmdir(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t *);
    static uint64_t sys_brk(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t *);
    static uint64_t sys_getrlimit(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t *);
    static uint64_t sys_setrlimit(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t *);
    static uint64_t sys_getrandom(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t *);
    static uint64_t sys_klog(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t *);
    static uint64_t sys_reboot(uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t *);
    static uint64_t sys_halt(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t *);
    static uint64_t sys_cap_grant(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t *);
    static uint64_t sys_cap_copy(uint64_t, uint64_t, uint64_t, uint64_t,
                                 uint64_t *);
    static uint64_t sys_cap_revoke(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t *);
    static uint64_t sys_cap_mint(uint64_t, uint64_t, uint64_t, uint64_t,
                                 uint64_t *);
    static uint64_t sys_ioport_grant(uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t *);
    static uint64_t sys_cap_retype(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t *);
    static uint64_t sys_irq_register(uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t *);
    static uint64_t sys_irq_wait(uint64_t, uint64_t, uint64_t, uint64_t,
                                 uint64_t *);

    static constexpr SyscallHandler
        syscall_table_[static_cast<size_t>(SyscallNumber::MAX_SYSCALL)] = {
            &Syscall::sys_yield,
            &Syscall::sys_send,
            &Syscall::sys_receive,
            &Syscall::sys_send_sync,
            &Syscall::sys_print,
            &Syscall::sys_get_ticks,
            &Syscall::sys_exit,
            &Syscall::sys_create_mailbox,
            &Syscall::sys_destroy_mailbox,
            &Syscall::sys_open,
            &Syscall::sys_read,
            &Syscall::sys_close,
            &Syscall::sys_fstat,
            &Syscall::sys_write,
            &Syscall::sys_lseek,
            &Syscall::sys_ioctl,
            &Syscall::sys_readdir,
            &Syscall::sys_stat,
            &Syscall::sys_dup,
            &Syscall::sys_chdir,
            &Syscall::sys_exec,
            &Syscall::sys_fork,
            &Syscall::sys_waitpid,
            &Syscall::sys_getpid,
            &Syscall::sys_kill,
            &Syscall::sys_pipe,
            &Syscall::sys_dup2,
            &Syscall::sys_notify,
            &Syscall::sys_notify_wait,
            &Syscall::sys_event_set,
            &Syscall::sys_event_wait,
            &Syscall::sys_signal,
            &Syscall::sys_sigreturn,
            &Syscall::sys_alarm,
            &Syscall::sys_gettod,
            &Syscall::sys_uname,
            &Syscall::sys_pause,
            &Syscall::sys_buf_alloc,
            &Syscall::sys_buf_free,
            &Syscall::sys_buf_map,
            &Syscall::sys_buf_unmap,
            &Syscall::sys_mkdir,
            &Syscall::sys_unlink,
            &Syscall::sys_rmdir,
            &Syscall::sys_brk,
            &Syscall::sys_getrlimit,
            &Syscall::sys_setrlimit,
            &Syscall::sys_getrandom,
            &Syscall::sys_klog,
            &Syscall::sys_reboot,
            &Syscall::sys_halt,
            &Syscall::sys_cap_grant,
            &Syscall::sys_cap_copy,
            &Syscall::sys_cap_revoke,
            &Syscall::sys_cap_mint,
            &Syscall::sys_ioport_grant,
            &Syscall::sys_cap_retype,
            &Syscall::sys_irq_register,
            &Syscall::sys_irq_wait,
    };
};

} // namespace kernel
