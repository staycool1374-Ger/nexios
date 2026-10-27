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

/// @file gen/nexios_syscalls.h
/// @brief Generated uniform Ring-3 stubs (#171 P2).
/// @details GENERATED FILE — do not edit. Regenerate with
/// `tools/gen_syscall_stubs.py`; see that script for the
/// exclusion list and determinism contract.

#pragma once

/// @brief Section 2 error predicate shared by every stub.
/// @param ret raw syscall return value.
/// @return -1 with errno set for -4095..-1, else ret.
long nexios_stub_map_err(long ret);

/// @brief Generated stub for SYS_YIELD (0).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_yield(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_SEND (1).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_send(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_RECEIVE (2).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_receive(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_SEND_SYNC (3).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
/// @warning reply_buf is always 0 (no-reply requests only).
long nexios_sys_send_sync(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_PRINT (4).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_print(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_GET_TICKS (5).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_get_ticks(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_EXIT (6).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_exit(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_CREATE_MAILBOX (7).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_create_mailbox(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_DESTROY_MAILBOX (8).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_destroy_mailbox(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_OPEN (9).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_open(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_READ (10).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_read(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_CLOSE (11).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_close(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_FSTAT (12).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_fstat(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_WRITE (13).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_write(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_LSEEK (14).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_lseek(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_IOCTL (15).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_ioctl(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_READDIR (16).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_readdir(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_STAT (17).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_stat(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_DUP (18).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_dup(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_CHDIR (19).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_chdir(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_EXEC (20).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_exec(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_FORK (21).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_fork(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_WAITPID (22).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_waitpid(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_GETPID (23).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_getpid(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_KILL (24).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_kill(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_PIPE (25).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_pipe(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_DUP2 (26).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_dup2(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_NOTIFY (27).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_notify(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_NOTIFY_WAIT (28).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_notify_wait(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_EVENT_SET (29).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_event_set(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_EVENT_WAIT (30).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_event_wait(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_SIGNAL (31).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_signal(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_SIGRETURN (32).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_sigreturn(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_ALARM (33).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_alarm(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_GETTOD (34).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_gettod(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_UNAME (35).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_uname(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_PAUSE (36).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_pause(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_BUF_ALLOC (37).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_buf_alloc(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_BUF_FREE (38).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_buf_free(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_BUF_MAP (39).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_buf_map(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_BUF_UNMAP (40).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_buf_unmap(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_MKDIR (41).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_mkdir(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_UNLINK (42).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_unlink(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_RMDIR (43).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_rmdir(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_BRK (44).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_brk(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_GETRLIMIT (45).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_getrlimit(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_SETRLIMIT (46).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_setrlimit(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_GETRANDOM (47).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_getrandom(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_KLOG (48).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_klog(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_REBOOT (49).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_reboot(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_HALT (50).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_halt(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_CAP_GRANT (51).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_cap_grant(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_CAP_COPY (52).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_cap_copy(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_CAP_REVOKE (53).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_cap_revoke(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_CAP_MINT (54).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_cap_mint(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_IOPORT_GRANT (55).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_ioport_grant(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_CAP_RETYPE (56).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_cap_retype(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_IRQ_REGISTER (57).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_irq_register(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_IRQ_WAIT (58).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_irq_wait(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_IOMMU_MAP (59).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_iommu_map(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_IOMMU_UNMAP (60).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_iommu_unmap(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_MMIO_MAP (61).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_mmio_map(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_MMIO_UNMAP (62).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_mmio_unmap(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_FRAME_CREATE (63).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_frame_create(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_FRAME_MAP (64).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_frame_map(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_FRAME_UNMAP (65).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_frame_unmap(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_DEATH_WATCH (66).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_death_watch(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_DEATH_RECV (67).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_death_recv(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_DEATH_UNWATCH (68).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_death_unwatch(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_PAGER_REGISTER (69).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_pager_register(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_PAGER_RECV (70).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_pager_recv(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_PAGER_MAP (71).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_pager_map(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_PAGER_ABORT (72).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_pager_abort(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_PAGER_UNREGISTER (73).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_pager_unregister(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_SET_AFFINITY (77).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_set_affinity(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_GET_AFFINITY (78).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_get_affinity(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_TIMES (79).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_times(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_CLOCK_GETTIME (80).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_clock_gettime(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_NANOSLEEP (81).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_nanosleep(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_TIMER_CREATE (82).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_timer_create(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_TIMERFD_CREATE (83).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_timerfd_create(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_ABI_VERSION (84).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_abi_version(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_TLS_SET (85).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_tls_set(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_WATCHDOG_CREATE (91).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_watchdog_create(
    long arg0, long arg1, long arg2, long arg3);
/// @brief Generated stub for SYS_WATCHDOG_KICK (92).
/// @param arg0 first syscall argument.
/// @param arg1 second syscall argument.
/// @param arg2 third syscall argument.
/// @param arg3 fourth syscall argument.
/// @return raw success value, or -1 with errno set.
long nexios_sys_watchdog_kick(
    long arg0, long arg1, long arg2, long arg3);

