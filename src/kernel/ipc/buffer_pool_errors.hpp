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

/// @file buffer_pool_errors.hpp
/// @brief Buffer pool error codes (X-macro table with human-readable strings).

#include <types.hpp>
#include <assert.hpp>

namespace kernel::errors {

#define BUFPOOL_ERROR_CODES                                                    \
    X(OK, 0, "OK")                                                             \
    X(OOM, 1, "No free physical pages available")                              \
    X(MAX_BUFFERS, 2, "Maximum number of buffers reached")                     \
    X(INVALID_HANDLE, 3,                                                       \
      "Invalid buffer handle (wrong generation"                                \
      " or index)")                                                            \
    X(NOT_OWNER, 4, "Buffer not owned by calling task")                        \
    X(VA_IN_USE, 5, "Virtual address already mapped")                          \
    X(VA_OUT_OF_RANGE, 6, "Virtual address outside user space")                \
    X(NOT_MAPPED, 7, "Buffer is not mapped in this task")

/// @brief Buffer pool error codes.
// NOLINTNEXTLINE(performance-enum-size)
enum BufPoolError : uint64_t {
#define X(name, num, msg) BUF_ERR_##name = (num),
    BUFPOOL_ERROR_CODES
#undef X
};

/// @brief Return a human-readable string for a buffer pool error code.
template <> inline const char *error_string(BufPoolError e) {
    switch (e) {
#define X(name, num, msg)                                                      \
    case BUF_ERR_##name:                                                       \
        return msg;
        BUFPOOL_ERROR_CODES
#undef X
    }
    return "Unknown buffer pool error";
}

} // namespace kernel::errors
