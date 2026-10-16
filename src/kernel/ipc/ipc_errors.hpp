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

/// @file ipc_errors.hpp
/// @brief IPC error codes (X-macro table with human-readable strings).

#include <types.hpp>
#include <assert.hpp>

namespace kernel::errors {

#define IPC_ERROR_CODES                                                        \
    X(OK, 0, "OK")                                                             \
    X(NO_QUEUE, 1, "No message queue for task")                                \
    X(QUEUE_FULL, 2, "Message queue is full")                                  \
    X(QUEUE_EMPTY, 3, "Message queue is empty")                                \
    X(NO_DEST, 4, "Destination task not found")                                \
    X(NO_REPLY, 5, "No reply received (send_sync)")                            \
    X(TIMEOUT, 6, "Operation timed out")                                       \
    X(INVALID_MSG, 7, "Invalid message format")                                \
    X(NO_BUFFER, 8, "No buffer available for zero-copy")                       \
    X(INVALID_ARGS, 9, "Invalid arguments")

/// @brief IPC subsystem error codes.
// NOLINTNEXTLINE(performance-enum-size)
enum IpcError : uint64_t {
#define X(name, num, msg) IPC_ERR_##name = (num),
    IPC_ERROR_CODES
#undef X
};

/// @brief OK anchor (issue #348-C): success is zero by convention.
static_assert(IPC_ERR_OK == 0, "error OK must be 0");

/// @brief Return a human-readable string for an IPC error code.
template <> inline const char *error_string(IpcError e) {
    switch (e) {
#define X(name, num, msg)                                                      \
    case IPC_ERR_##name:                                                       \
        return msg;
        IPC_ERROR_CODES
#undef X
    }
    return "Unknown IPC error";
}

} // namespace kernel::errors