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

extern int errno;

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
