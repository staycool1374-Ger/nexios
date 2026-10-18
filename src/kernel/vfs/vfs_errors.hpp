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

/// @file vfs_errors.hpp
/// @brief VFS error code definitions and error-string conversion.

#include <types.hpp>
#include <assert.hpp>

namespace kernel::errors {

#define VFS_ERROR_CODES                                                        \
    X(OK, 0, "OK")                                                             \
    X(INVALID_FD, 1, "Invalid file descriptor")                                \
    X(FD_TABLE_FULL, 2, "File descriptor table full")                          \
    X(NOT_FOUND, 3, "Path or vnode not found")                                 \
    X(NOT_DIR, 4, "Not a directory")                                           \
    X(IS_DIR, 5, "Is a directory")                                             \
    X(EXISTS, 6, "File or directory already exists")                           \
    X(NOT_EMPTY, 7, "Directory not empty")                                     \
    X(NO_DEVICE, 8, "No such device or mount point")                           \
    X(NO_SPACE, 9, "No space left on device")                                  \
    X(PERMISSION, 10, "Permission denied")                                     \
    X(INVALID_ARGS, 11, "Invalid arguments")                                   \
    X(IO_ERROR, 12, "I/O error")                                               \
    X(NOT_SUPPORTED, 13, "Operation not supported")                            \
    X(MOUNT_POINT_BUSY, 14, "Mount point busy")                                \
    X(NO_SUCH_FS, 15, "Filesystem type not found")

// NOLINTNEXTLINE(performance-enum-size)
enum VfsError : uint64_t {
#define X(name, num, msg) VFS_ERR_##name = (num),
    VFS_ERROR_CODES
#undef X
};

/// @brief Return a human-readable string for a VFS error code.
template <> inline const char *error_string(VfsError e) {
    switch (e) {
#define X(name, num, msg)                                                      \
    case VFS_ERR_##name:                                                       \
        return msg;
        VFS_ERROR_CODES
#undef X
    }
    return "Unknown VFS error";
}

} // namespace kernel::errors