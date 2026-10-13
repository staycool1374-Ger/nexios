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

/// @file picolib_stubs.c
/// @brief picolibc/newlib syscall stubs over __syscall5 (issue #71).
/// Newlib-convention `_`-prefixed adapters with §2 errno mapping
/// (docs/specs/syscall-abi-picolibc.md §5). No `_exit` here — it lives
/// in unistd.c (redefining it would be a multiple-definition link
/// error in LIBC_A). `errno` is a plain global until #74 (TLS).

#include <syscall.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stddef.h>
#include <unistd.h>

/// @brief §2 error predicate: map a raw syscall return to errno/-1.
static long map_err(long ret) {
    if (ret < 0 && (unsigned long)ret > -4096UL) {
        errno = (int)(-ret);
        return -1;
    }
    return ret;
}

int _write(int fd, char *buf, int count) {
    return (int)map_err(__syscall5(SYS_WRITE, (long)fd, (long)buf,
                                   (long)count, 0));
}

int _read(int fd, char *buf, int count) {
    return (int)map_err(__syscall5(SYS_READ, (long)fd, (long)buf,
                                   (long)count, 0));
}

int _close(int fd) {
    return (int)map_err(__syscall5(SYS_CLOSE, (long)fd, 0, 0, 0));
}

off_t _lseek(int fd, off_t offset, int whence) {
    return (off_t)map_err(__syscall5(SYS_LSEEK, (long)fd, (long)offset,
                                     (long)whence, 0));
}

/* Newlib/picolibc open-flag values (sysroot sys/_default_fcntl.h):
 * _FCREAT 0x40, _FTRUNC 0x200. Kernel VFS OpenFlags (vfs.hpp):
 * O_CREAT 0x200, no O_TRUNC.Bits 0-1 (access mode) match. */
#define PICOLIBC_O_CREAT 0x40
#define PICOLIBC_O_TRUNC 0x200
#define KERNEL_O_CREAT 0x200

int _fstat(int fd, struct stat *buf); // defined below; _open needs it

int _open(const char *path, int flags, int mode) {
    (void)mode; // kernel OPEN takes path+flags only
    // Issue #341: translate here — the single picolibc→kernel owner.
    // Kernel-side translation is impossible: bit 0x200 means O_CREAT
    // to in-tree callers (kernel values) but O_TRUNC to picolibc
    // callers, so the kernel cannot disambiguate by value.
    int kflags = flags & 0x3;
    const int want_creat = (flags & PICOLIBC_O_CREAT) != 0;
    const int want_trunc = (flags & PICOLIBC_O_TRUNC) != 0;
    if (want_creat)
        kflags |= KERNEL_O_CREAT;
    // _FTRUNC aliases kernel O_CREAT and must never pass through
    // (it would create instead of truncating). Anything else passes
    // through unchanged (pre-existing behavior for EXCL/APPEND/etc,
    // which the kernel neither maps nor honors).
    kflags |= (flags & ~(0x3 | PICOLIBC_O_CREAT | PICOLIBC_O_TRUNC));
    int fd = (int)map_err(__syscall5(SYS_OPEN, (long)path, (long)kflags,
                                     0, 0));
    if (fd < 0 || !want_trunc)
        return fd;
    // O_TRUNC: the kernel has no truncate primitive. A new or empty
    // file is already truncated — succeed. A non-empty existing file
    // cannot be truncated: close and fail ENOSYS (honest, not silent).
    // An unstatable file proceeds (pre-existing behavior; truncation
    // need unproven rather than assumed impossible).
    struct stat st;
    if (_fstat(fd, &st) == 0 && st.st_size > 0) {
        _close(fd);
        errno = ENOSYS;
        return -1;
    }
    return fd;
}

int _fstat(int fd, struct stat *buf) {
    return (int)map_err(__syscall5(SYS_FSTAT, (long)fd, (long)buf, 0, 0));
}

int _getpid(void) {
    return (int)map_err(__syscall5(SYS_GETPID, 0, 0, 0, 0));
}

int _kill(int pid, int sig) {
    return (int)map_err(__syscall5(SYS_KILL, (long)pid, (long)sig, 0, 0));
}

void *_sbrk(ptrdiff_t incr) {
    // Delegate to sbrk() (stdlib.c): single owner of break arithmetic
    // and the STACK_VADDR red-zone cap (MP-2.2); no logic duplicated.
    void *prev = sbrk((long)incr);
    if (prev == (void *)-1)
        errno = ENOMEM;
    return prev;
}
