/*
 * NexIOS RTOS — Generated syscall stubs (issue #171 P2).
 *
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

/// @file gen/nexios_syscalls.c
/// @brief Generated uniform Ring-3 stubs (#171 P2).
/// @details GENERATED FILE — do not edit. Regenerate with
/// `tools/gen_syscall_stubs.py`. Every stub routes
/// (arg0..arg3) through `__syscall5r` with an explicit zero
/// fifth register (deterministic trap state on all arches);
/// the kernel documents reply_buf == 0 as no-reply for its
/// only reader (SEND_SYNC). Failures map through section 2
/// predicate: -4095 maps to errno, -4097 stays success.

#include "nexios_syscalls.h"
#include <errno.h>
#include <syscall.h>

long nexios_stub_map_err(long ret) {
    if (ret < 0 && (unsigned long)ret > -4096UL) {
        errno = (int)(-ret);
        return -1;
    }
    return ret;
}

long nexios_sys_yield(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_YIELD, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_send(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_SEND, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_receive(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_RECEIVE, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_send_sync(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_SEND_SYNC, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_print(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_PRINT, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_get_ticks(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_GET_TICKS, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_exit(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_EXIT, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_create_mailbox(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_CREATE_MAILBOX, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_destroy_mailbox(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_DESTROY_MAILBOX, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_open(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_OPEN, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_read(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_READ, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_close(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_CLOSE, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_fstat(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_FSTAT, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_write(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_WRITE, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_lseek(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_LSEEK, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_ioctl(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_IOCTL, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_readdir(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_READDIR, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_stat(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_STAT, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_dup(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_DUP, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_chdir(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_CHDIR, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_exec(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_EXEC, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_fork(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_FORK, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_waitpid(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_WAITPID, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_getpid(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_GETPID, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_kill(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_KILL, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_pipe(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_PIPE, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_dup2(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_DUP2, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_notify(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_NOTIFY, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_notify_wait(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_NOTIFY_WAIT, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_event_set(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_EVENT_SET, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_event_wait(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_EVENT_WAIT, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_signal(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_SIGNAL, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_sigreturn(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_SIGRETURN, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_alarm(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_ALARM, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_gettod(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_GETTOD, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_uname(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_UNAME, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_pause(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_PAUSE, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_buf_alloc(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_BUF_ALLOC, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_buf_free(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_BUF_FREE, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_buf_map(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_BUF_MAP, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_buf_unmap(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_BUF_UNMAP, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_mkdir(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_MKDIR, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_unlink(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_UNLINK, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_rmdir(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_RMDIR, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_brk(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_BRK, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_getrlimit(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_GETRLIMIT, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_setrlimit(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_SETRLIMIT, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_getrandom(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_GETRANDOM, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_klog(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_KLOG, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_reboot(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_REBOOT, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_halt(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_HALT, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_cap_grant(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_CAP_GRANT, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_cap_copy(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_CAP_COPY, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_cap_revoke(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_CAP_REVOKE, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_cap_mint(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_CAP_MINT, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_ioport_grant(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_IOPORT_GRANT, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_cap_retype(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_CAP_RETYPE, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_irq_register(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_IRQ_REGISTER, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_irq_wait(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_IRQ_WAIT, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_iommu_map(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_IOMMU_MAP, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_iommu_unmap(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_IOMMU_UNMAP, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_mmio_map(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_MMIO_MAP, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_mmio_unmap(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_MMIO_UNMAP, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_frame_create(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_FRAME_CREATE, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_frame_map(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_FRAME_MAP, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_frame_unmap(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_FRAME_UNMAP, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_death_watch(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_DEATH_WATCH, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_death_recv(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_DEATH_RECV, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_death_unwatch(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_DEATH_UNWATCH, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_pager_register(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_PAGER_REGISTER, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_pager_recv(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_PAGER_RECV, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_pager_map(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_PAGER_MAP, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_pager_abort(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_PAGER_ABORT, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_pager_unregister(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_PAGER_UNREGISTER, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_set_affinity(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_SET_AFFINITY, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_get_affinity(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_GET_AFFINITY, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_times(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_TIMES, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_clock_gettime(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_CLOCK_GETTIME, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_nanosleep(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_NANOSLEEP, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_timer_create(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_TIMER_CREATE, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_timerfd_create(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_TIMERFD_CREATE, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_abi_version(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_ABI_VERSION, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_tls_set(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_TLS_SET, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_watchdog_create(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_WATCHDOG_CREATE, arg0, arg1, arg2, arg3, 0));
}

long nexios_sys_watchdog_kick(
    long arg0, long arg1, long arg2, long arg3) {
    return nexios_stub_map_err(__syscall5r(
        SYS_WATCHDOG_KICK, arg0, arg1, arg2, arg3, 0));
}

