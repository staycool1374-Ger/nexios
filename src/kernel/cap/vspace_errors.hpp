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

/// @file vspace_errors.hpp
/// @brief VSpace mapping error codes (F5, issue #558): X-macro table
///        with human-readable strings (tcb_control_errors.hpp pattern).
///        The kernel API reports these through the VSpaceMap link path;
///        the syscall face is the 1100-block via from_vspace_error
///        (syscall_errors.hpp), never a raw -1.  Every code is reachable
///        from exactly one link/validate branch — no dead enum.

#include <types.hpp>
#include <assert.hpp>

namespace kernel::errors {

#define VSPACE_ERROR_CODES                                                     \
    X(OK, 0, "OK")                                                             \
    X(INVALID_ARGS, 1, "Invalid arguments")                                    \
    X(BAD_HANDLE, 2, "VSpace or PageTable handle invalid or revoked")          \
    X(BAD_OBJECT, 3, "Handle resolves but is not the expected carved kind")    \
    X(OWNERSHIP_MISMATCH, 4, "VSpace and PageTable PMM ownership differ")      \
    X(BAD_LEVEL, 5, "Path length is zero or exceeds the arch walk depth")      \
    X(KERNEL_HALF, 6, "Path root component names the kernel half")             \
    X(SLOT_OCCUPIED, 7, "Destination table entry already holds a link")       \
    X(ALIASED, 8, "PageTable page already linked under a parent")              \
    X(NOT_TABLE, 9, "Path component holds a leaf, not a table")                \
    X(UNMAPPED, 10, "Path component absent while descending")                  \
    X(REGISTRY_FULL, 11, "No free VSpace mapping registry slot")               \
    X(ACTIVE_SPACE, 12, "Target root is the currently active address space")  \
    X(ACTIVATED, 13, "Target root is bound to a live task")                   \
    X(SUBTREE_INVALID, 14, "Carved subtree fails activation re-validation")   \
    X(TARGET_REJECTED, 15, "Activation target is not an idle carved task")

/// @brief VSpace mapping error codes (F5 object-link mechanism).
// NOLINTNEXTLINE(performance-enum-size)
enum VspaceError : uint64_t {
#define X(name, num, msg) VSPACE_ERR_##name = (num),
    VSPACE_ERROR_CODES
#undef X
};

/// @brief OK anchor (issue #348-C): success is zero by convention.
static_assert(VSPACE_ERR_OK == 0, "error OK must be 0");

/// @brief ABI-face anchors (issue #564, F11 Stage C): the syscall face is
///        arithmetic (1100 + code), so the new codes pin their faces here.
static_assert(VSPACE_ERR_ACTIVATED == 13, "ACTIVATED must be code 13");
static_assert(VSPACE_ERR_SUBTREE_INVALID == 14,
              "SUBTREE_INVALID must be code 14");
static_assert(VSPACE_ERR_TARGET_REJECTED == 15,
              "TARGET_REJECTED must be code 15");

/// @brief Return a human-readable string for a VSpace mapping error.
template <> inline const char *error_string(VspaceError e) {
    switch (e) {
#define X(name, num, msg)                                                      \
    case VSPACE_ERR_##name:                                                    \
        return msg;
        VSPACE_ERROR_CODES
#undef X
    }
    return "Unknown VSpace mapping error";
}

} // namespace kernel::errors
