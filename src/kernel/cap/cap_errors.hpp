#pragma once

/*
 * NexIOS RTOS — Capability-Based Access Control (CSpace)
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

/// @file cap_errors.hpp
/// @brief Retype error codes (F1, issue #554): X-macro table with
///        human-readable strings (sync_errors.hpp pattern).  The kernel
///        API reports these through the retype out-param; the syscall
///        face stays the frozen -1 encoding (T-1/Q3).

#include <types.hpp>
#include <assert.hpp>

namespace kernel::errors {

#define CAP_ERROR_CODES                                                        \
    X(OK, 0, "OK")                                                             \
    X(INVALID_ARGS, 1, "Invalid arguments")                                    \
    X(BAD_HANDLE, 2, "Untyped handle invalid or revoked")                       \
    X(POLICY_DENIED, 3, "Retype denied by CNode policy")                       \
    X(TARGET_UNSUPPORTED, 4, "Target type is not carvable")                    \
    X(SIZE_ZERO, 5, "Retype size must be non-zero")                            \
    X(SIZE_UNALIGNED, 6, "Retype size is not PAGE-aligned")                     \
    X(RANGE_EXCEEDED, 7, "Retype size exceeds the Untyped region")              \
    X(SHORT_RANGE, 8, "Untyped region smaller than the fixed-size object")      \
    X(SIZE_MISMATCH, 9, "Explicit size differs from the fixed object size")     \
    X(NO_CSLOT, 10, "No free CNode slot for the retype")                       \
    X(ALREADY_RETYPED, 11, "Untyped region already retyped")                    \
    X(OBJECT_EXHAUSTED, 12, "Target object allocation failed")                  \
    X(CHILD_UNAVAILABLE, 13, "Child Untyped allocation failed")                \
    X(INSTALL_FAILED, 14, "Target slot install failed")                         \
    X(DERIVATION_FULL, 15, "Derivation table full")                             \
    X(DEPTH_EXCEEDED, 16, "Derivation depth bound exceeded")                    \
    X(REVOKE_INCOMPLETE, 17, "Revoke incomplete: call again to continue")      \
    X(DERIVATION_CYCLE, 18, "Derivation table corrupt: revoke step bound hit")

/// @brief Retype error codes (F1 object model).
// NOLINTNEXTLINE(performance-enum-size)
enum CapError : uint64_t {
#define X(name, num, msg) CAP_ERR_##name = (num),
    CAP_ERROR_CODES
#undef X
};

/// @brief OK anchor (issue #348-C): success is zero by convention.
static_assert(CAP_ERR_OK == 0, "error OK must be 0");

/// @brief Return a human-readable string for a retype error code.
template <> inline const char *error_string(CapError e) {
    switch (e) {
#define X(name, num, msg)                                                      \
    case CAP_ERR_##name:                                                       \
        return msg;
        CAP_ERROR_CODES
#undef X
    }
    return "Unknown capability error";
}

} // namespace kernel::errors
