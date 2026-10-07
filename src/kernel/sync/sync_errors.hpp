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

/// @file sync_errors.hpp
/// @brief Synchronisation error codes (X-macro table with human-readable
/// strings).

#include <types.hpp>
#include <assert.hpp>

namespace kernel::errors {

#define SYNC_ERROR_CODES                                                       \
    X(OK, 0, "OK")                                                             \
    X(NO_TASK, 1, "No current task context")                                   \
    X(MAX_WAITERS, 2, "Maximum waiters exceeded")                              \
    X(ALREADY_INITIALIZED, 3, "Object already initialized")                    \
    X(NOT_OWNER, 4, "Not the lock/sync object owner")                          \
    X(NOT_LOCKED, 5, "Lock/sync object not held")                              \
    X(QUEUE_FULL, 6, "Message queue is full")                                  \
    X(QUEUE_EMPTY, 7, "Message queue is empty")                                \
    X(MSG_TOO_LARGE, 8, "Message exceeds maximum size")                        \
    X(ALREADY_WAITING, 9, "Task already waiting on this object")               \
    X(INVALID_ARGS, 10, "Invalid arguments")                                   \
    X(BUFFER_FULL, 11, "Buffer is full")                                       \
    X(BUFFER_EMPTY, 12, "Buffer is empty")                                     \
    X(NO_WAITER, 13, "No waiter to notify")                                    \
    X(INTERRUPTED, 14, "Operation interrupted by signal")

/// @brief Synchronisation subsystem error codes.
// NOLINTNEXTLINE(performance-enum-size)
enum SyncError : uint64_t {
#define X(name, num, msg) SYNC_ERR_##name = (num),
    SYNC_ERROR_CODES
#undef X
};

/// @brief Return a human-readable string for a sync error code.
template <> inline const char *error_string(SyncError e) {
    switch (e) {
#define X(name, num, msg)                                                      \
    case SYNC_ERR_##name:                                                      \
        return msg;
        SYNC_ERROR_CODES
#undef X
    }
    return "Unknown sync error";
}

} // namespace kernel::errors