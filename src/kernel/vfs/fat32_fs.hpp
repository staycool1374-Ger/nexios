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

/// @file fat32_fs.hpp
/// @brief FAT32 VFS filesystem integration (mount, root vnode).

#pragma once

#include <kernel/vfs/vfs.hpp>
#include <kernel/vfs/fat32.hpp>

namespace kernel {
namespace vfs {

/// @brief Mount the FAT32 filesystem at the given mount point.
/// @return 0 on success, VFS_INVALID on error.
int mount_fat32(const char *mount_point);

/// @brief VFS filesystem descriptor for FAT32.
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern Filesystem fat32_fs;

} // namespace vfs
} // namespace kernel
