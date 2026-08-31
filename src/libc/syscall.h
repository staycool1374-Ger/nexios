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

#pragma once

#ifndef __LIBK_SYSCALL_H
#define __LIBK_SYSCALL_H

#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>

struct rlimit {
    unsigned long rlim_cur;
    unsigned long rlim_max;
};

#define SYS_YIELD       0
#define SYS_SEND        1
#define SYS_RECEIVE     2
#define SYS_SEND_SYNC   3
#define SYS_PRINT       4
#define SYS_GET_TICKS   5
#define SYS_EXIT        6
#define SYS_CREATE_MAILBOX 7
#define SYS_DESTROY_MAILBOX 8
#define SYS_OPEN        9
#define SYS_READ        10
#define SYS_CLOSE       11
#define SYS_FSTAT       12
#define SYS_WRITE       13
#define SYS_LSEEK       14
#define SYS_IOCTL       15
#define SYS_READDIR     16
#define SYS_STAT        17
#define SYS_DUP         18
#define SYS_CHDIR       19
#define SYS_EXEC        20
#define SYS_FORK        21
#define SYS_WAITPID     22
#define SYS_GETPID      23
#define SYS_KILL        24
#define SYS_PIPE        25
#define SYS_DUP2        26
#define SYS_NOTIFY      27
#define SYS_NOTIFY_WAIT 28
#define SYS_EVENT_SET   29
#define SYS_EVENT_WAIT  30
#define SYS_SIGNAL      31
#define SYS_SIGRETURN   32
#define SYS_ALARM       33
#define SYS_GETTOD      34
#define SYS_UNAME       35
#define SYS_PAUSE       36
#define SYS_MKDIR       41
#define SYS_UNLINK      42
#define SYS_RMDIR       43
#define SYS_BRK         44
#define SYS_GETRLIMIT   45
#define SYS_SETRLIMIT   46
#define SYS_GETRANDOM   47

static inline long __syscall5(long num, long a0, long a1, long a2, long a3) {
    long ret;
    // ABI contract (single source of truth):
    //   x86_64: number in rax, args in rbx/rcx/rdx/rsi (int $0x80)
    //   aarch64: number in x8, args in x0-x3 (Linux AArch64 convention)
    //   riscv64: number in a7, args in a0-a3 (ecall)
    // The arch syscall entry stubs must match these register choices.
#if defined(__x86_64__)
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a0), "c"(a1), "d"(a2), "S"(a3)
        : "memory", "cc"
    );
#elif defined(__aarch64__)
    register long x8 asm("x8") = num;
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    register long x3 asm("x3") = a3;
    register long x4 asm("x4") = 0;
    asm volatile(
        "svc #0"
        : "+r"(x0)
        : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
        : "memory"
    );
    ret = x0;
#elif defined(__riscv) && __riscv_xlen == 64
    register long a7 asm("a7") = num;
    register long a0_reg asm("a0") = a0;
    register long a1_reg asm("a1") = a1;
    register long a2_reg asm("a2") = a2;
    register long a3_reg asm("a3") = a3;
    asm volatile(
        "ecall"
        : "+r"(a0_reg)
        : "r"(a7), "r"(a1_reg), "r"(a2_reg), "r"(a3_reg)
        : "memory"
    );
    ret = a0_reg;
#else
#  error "Unsupported architecture for __syscall5"
#endif
    return ret;
}

static inline long sys_alarm(unsigned int seconds) {
    return __syscall5(SYS_ALARM, seconds, 0, 0, 0);
}

static inline long sys_alarm_us(unsigned int seconds, unsigned int microseconds) {
    return __syscall5(SYS_ALARM, seconds, microseconds, 0, 0);
}

static inline long sys_gettod(struct timeval* tv) {
    return __syscall5(SYS_GETTOD, (long)tv, 0, 0, 0);
}

static inline long sys_uname(struct utsname* buf) {
    return __syscall5(SYS_UNAME, (long)buf, 0, 0, 0);
}

static inline long sys_kill(pid_t pid, int sig) {
    return __syscall5(SYS_KILL, pid, sig, 0, 0);
}

static inline long sys_signal(int signum, void (*handler)(int)) {
    return __syscall5(SYS_SIGNAL, signum, (long)handler, 0, 0);
}

static inline long sys_sigreturn(void) {
    return __syscall5(SYS_SIGRETURN, 0, 0, 0, 0);
}

#define SYS_REBOOT      49
#define SYS_HALT        50

static inline long sys_pause(void) {
    return __syscall5(SYS_PAUSE, 0, 0, 0, 0);
}

static inline long sys_yield(void) {
    return __syscall5(SYS_YIELD, 0, 0, 0, 0);
}

static inline long sys_reboot(void) {
    return __syscall5(SYS_REBOOT, 0, 0, 0, 0);
}

static inline long sys_halt(void) {
    return __syscall5(SYS_HALT, 0, 0, 0, 0);
}

static inline void* sys_brk(void* addr) {
    long ret = __syscall5(SYS_BRK, (long)addr, 0, 0, 0);
    return (void*)ret;
}

static inline long sys_getrlimit(int resource, struct rlimit* rlp) {
    return __syscall5(SYS_GETRLIMIT, resource, (long)rlp, 0, 0);
}

static inline long sys_setrlimit(int resource, const struct rlimit* rlp) {
    return __syscall5(SYS_SETRLIMIT, resource, (long)rlp, 0, 0);
}

static inline long sys_getrandom(void* buf, unsigned long len, unsigned int flags) {
    return __syscall5(SYS_GETRANDOM, (long)buf, (long)len, (long)flags, 0);
}

#endif
