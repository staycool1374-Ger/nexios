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

/// @file vfsd.hpp
/// @brief VFS daemon IPC message types and PID tracking.

#include <types.hpp>

namespace kernel {

struct TaskControlBlock;

namespace vfsd {

static constexpr uint64_t VFS_OPEN = 100;
static constexpr uint64_t VFS_CLOSE = 101;
static constexpr uint64_t VFS_READ = 102;
static constexpr uint64_t VFS_WRITE = 103;
static constexpr uint64_t VFS_STAT = 104;
static constexpr uint64_t VFS_FSTAT = 105;
static constexpr uint64_t VFS_MOUNT = 106;
static constexpr uint64_t VFS_CHDIR = 107;
static constexpr uint64_t VFS_GETCWD = 108;
static constexpr uint64_t VFS_READDIR = 109;
static constexpr uint64_t VFS_MKDIR = 110;
static constexpr uint64_t VFS_UNLINK = 111;
static constexpr uint64_t VFS_RMDIR = 112;
// Issue #341: create-intent open. The kernel's sys_open O_CREAT branch is
// the ONLY caller that authorizes a not-yet-existing path; it uses this
// op (never VFS_OPEN) so the daemon can tell creation apart from plain
// open. Any future VFS_OPEN caller on a missing path must use this op.
static constexpr uint64_t VFS_OPEN_CREATE = 113;

struct Msg {
    uint64_t sender_id; ///< Task ID of the sender.
    uint64_t type;      ///< Request type (VFS_OPEN, VFS_READ, etc.).
    uint64_t arg0;      ///< First argument (varies by type).
    uint64_t arg1;      ///< Second argument (varies by type).
    char path[32];      ///< Path buffer for path-based requests.
};

struct Reply {
    int64_t result; ///< Return code (0 on success, negative on error).
    uint64_t data0; ///< Response data field 0.
    uint64_t data1; ///< Response data field 1.
    uint64_t data2; ///< Response data field 2.
    uint64_t data3; ///< Response data field 3.
};

/// @brief Record the PID of the VFS daemon task.
void set_vfsd_pid(uint64_t pid);
/// @brief Get the recorded VFS daemon PID.
/// @return The PID, or 0 if not yet set.
uint64_t get_vfsd_pid();
/// @brief Check if the current task is the VFS daemon.
/// @return true if the current task's PID matches the daemon PID.
bool is_vfsd_task();

} // namespace vfsd
} // namespace kernel