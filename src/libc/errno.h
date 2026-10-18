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

/// @brief Per-task control block (issue #338 — thread-local errno).
/// Lives in task-private memory; the kernel loads its address into the
/// TP register on every dispatch (TLS_SET at crt0 startup, inherited
/// across fork by address-space copy). errno sits at offset 0 so
/// __errno_location() is a single TP read + dereference with no layout
/// dependency beyond this header. Future thread-private state grows
/// here (never reorder existing fields).
struct nexios_tcb {
    int errno_value;
};

/// @brief Address of the calling task's errno cell (TP + 0).
/// Never NULL after crt0 startup (block registered before main).
int *__errno_location(void);

/// @brief Thread-local errno (issue #338): each task reads its own
/// TP-published cell. Plain `errno = x` / `errno` uses keep working
/// through the dereference.
#define errno (*__errno_location())

#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define EIO     5
#define EAGAIN  11
#define EBADF   9
#define ENOMEM  12
#define EACCES  13
#define EEXIST  17
#define ENODEV  19
#define EISDIR  21
#define EINVAL  22
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define ERANGE  34
#define ENOSYS  38
#define ENOTDIR 50
#define ETIMEDOUT 110
