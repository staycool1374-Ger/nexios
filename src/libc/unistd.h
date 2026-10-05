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

#include <sys/types.h>
#include <time.h>
#include <sys/utsname.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_NONBLOCK 0x800
#define O_CREAT    0x200
#define O_RAWTTY   0x1000

extern char** environ;

pid_t getpid(void);
int open(const char* path, int flags);
ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int dup(int fd);
int dup2(int oldfd, int newfd);
int chdir(const char* path);
int pipe(int fds[2]);

pid_t fork(void);
pid_t waitpid(pid_t pid, int* status, int options);

void _exit(int status);
unsigned int sleep(unsigned int seconds);
unsigned int alarm(unsigned int seconds);
int gettimeofday(struct timeval* tv, void* tz);
int uname(struct utsname* buf);
int pause(void);
int mkdir(const char* path, unsigned int mode);
int unlink(const char* path);

void* sbrk(long increment);
int brk(void* addr);
