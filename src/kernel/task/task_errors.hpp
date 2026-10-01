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

/// @file task_errors.hpp
/// @brief Task module error codes and string lookup.

#pragma once

#include <types.hpp>
#include <assert.hpp>

namespace kernel::errors {

#define TASK_ERROR_CODES                                                       \
    X(OK, 0, "OK")                                                             \
    X(OOM, 1, "Task creation failed, out of memory")                           \
    X(TABLE_FULL, 2, "Task creation failed, maximum of MAX_TASKS reached")     \
    X(STACK_ALLOC, 3, "Kernel stack allocation failed")                        \
    X(USTACK_ALLOC, 4, "User stack allocation failed")                         \
    X(PML4_CLONE, 5, "PML4 clone failed")                                      \
    X(NOT_FOUND, 6, "Task not found")                                          \
    X(INVALID_ARG, 7, "Invalid argument")                                      \
    X(INVALID_STATE, 8, "Invalid task state")                                  \
    X(CORRUPTED, 9, "Task control block corrupt (bad magic)")

// NOLINTNEXTLINE(performance-enum-size)
enum TaskError : uint64_t {
#define X(name, num, msg) TASK_ERR_##name = (num),
    TASK_ERROR_CODES
#undef X
};

/// @brief Return a human-readable string for a task error code.
template <> inline const char *error_string(TaskError error) {
    switch (error) {
#define X(name, num, msg)                                                      \
    case TASK_ERR_##name:                                                      \
        return msg;
        TASK_ERROR_CODES
#undef X
    }
    return "Unknown task error";
}

} // namespace kernel::errors
