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

/// @file syscall_helpers.hpp
/// @brief Helpers used by syscall handlers — current task, path resolution, fd
/// management.

#pragma once

#include <types.hpp>
#include <kernel/task/task.hpp>
#include <kernel/vfs/vfs.hpp>

namespace kernel {

constexpr uint64_t SYSCALL_MAX_PATH = 256;

/// @brief Get the currently running task from the scheduler.
/// @return Pointer to the current TaskControlBlock.
TaskControlBlock *syscall_task();
/// @brief Check if the current task is a user-space task.
/// @return true if user task (has user page table).
bool syscall_is_user_task();
/// @brief Open a vnode as a file descriptor in the current task's fd table.
/// @return The fd index, or VFS_INVALID on failure.
int syscall_task_open(vfs::Vnode *vn, uint64_t flags);
/// @brief Resolve a path and open it as a file descriptor.
/// @return The fd index, or VFS_INVALID on failure.
int syscall_path_open(const char *path, uint64_t flags);
/// @brief True when a vnode is a POSIX timerfd (issue #76; defined in
/// syscall_handlers_posix_time.cpp; ops-pointer identity).
bool is_timerfd_vnode(const vfs::Vnode *node);
/// @brief Blocking timerfd read helper (issue #76; defined in
/// syscall_handlers_posix_time.cpp). Returns 8 on success or -errno.
/// @param nonblock True when O_NONBLOCK is set on the fd.
uint64_t timerfd_read_entry(vfs::Vnode &node, uint8_t *buffer, uint64_t count,
                            bool nonblock);

} // namespace kernel
