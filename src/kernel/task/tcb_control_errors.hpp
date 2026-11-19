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

/// @file tcb_control_errors.hpp
/// @brief TCB control error codes (F4, issue #557): X-macro table with
///        human-readable strings (cap_errors.hpp pattern).  The kernel
///        API reports these through the TCBCTL_* syscall face; the face
///        is a NEW number range (1000-block), never a frozen encoding.

#include <types.hpp>
#include <assert.hpp>

namespace kernel::errors {

#define TCB_CONTROL_ERROR_CODES                                                \
    X(OK, 0, "OK")                                                             \
    X(INVALID_ARGS, 1, "Invalid arguments")                                    \
    X(BAD_HANDLE, 2, "TCB cap handle invalid or revoked")                      \
    X(STALE_TARGET, 3, "TCB cap target recycled (generation mismatch)")        \
    X(DEAD, 4, "Target is TERMINATED or REAPED")                               \
    X(SELF, 5, "Target is the calling task")                                  \
    X(KERNEL_TASK, 6, "Target is not a user task")                             \
    X(PROTECTED_ROLE, 7, "Target holds a protected role (never mem-mgr)")      \
    X(NO_AUTHORITY, 8, "Caller is neither creator nor supervisor")             \
    X(NOT_SUSPENDED, 9, "Target is not suspended")                             \
    X(ALREADY_SUSPENDED, 10, "Target is already suspended")                    \
    X(BUSY, 11, "Target is debugger-parked or latch-armed")                   \
    X(UNSUPPORTED, 12, "Field not configurable at F4 (schedctx/endpoint)")     \
    X(IDLE, 13, "Target is the idle task")

/// @brief TCB control error codes (F4 admin stop).
// NOLINTNEXTLINE(performance-enum-size)
enum TcbCtlError : uint64_t {
#define X(name, num, msg) TCBCTL_ERR_##name = (num),
    TCB_CONTROL_ERROR_CODES
#undef X
};

/// @brief OK anchor (issue #348-C): success is zero by convention.
static_assert(TCBCTL_ERR_OK == 0, "error OK must be 0");

/// @brief Return a human-readable string for a TCB control error code.
template <> inline const char *error_string(TcbCtlError e) {
    switch (e) {
#define X(name, num, msg)                                                      \
    case TCBCTL_ERR_##name:                                                    \
        return msg;
        TCB_CONTROL_ERROR_CODES
#undef X
    }
    return "Unknown TCB control error";
}

} // namespace kernel::errors
