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

/// @file pipe.hpp
/// @brief Pipe (anonymous FIFO) creation API.

#include <types.hpp>

namespace kernel {
namespace vfs {

/// @brief Create a pair of connected pipe file descriptors.
/// @param[out] fds Array of two descriptors: fds[0] for read, fds[1] for write.
/// @return 0 on success, or VFS_INVALID on failure.
int create_pipe(int fds[2]);

} // namespace vfs
} // namespace kernel