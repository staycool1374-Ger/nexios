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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/// @file mem_handover_errors.hpp
/// @brief Mem-mgr handover error codes (M4, #563; X-macro table).
#pragma once

#include <types.hpp>
#include <assert.hpp>

namespace kernel::errors {

#define MEMHANDOVER_ERROR_CODES                                                \
    X(OK, 0, "OK")                                                             \
    X(ALREADY_FROZEN, 1, "Handover already completed")                         \
    X(NO_ROOT, 2, "Root task has no CSpace")                                   \
    X(CSPACE_FULL, 3, "No free CNode slot for handover")                    \
    X(UNTYPED_CAP_FULL, 4, "Untyped live bound reached")                       \
    X(NO_FREE_RUNS, 5, "No free runs to hand over")                            \
    X(REGISTRY_FULL, 6, "Handover registry full")                              \
    X(BAD_TARGET, 7, "Bad handover target")                                    \
    X(ALREADY_TRANSFERRED, 8, "Handover already moved")                    \
    X(STALE_SLOT, 9, "Handover slot no longer names its cap")              \
    X(BOUNDS, 10, "Handover demand exceeds a configured bound")

/// @brief Mem-mgr handover errors.
// NOLINTNEXTLINE(performance-enum-size)
enum MemHandoverError : uint64_t {
#define X(name, num, msg) MEM_HANDOVER_ERR_##name = (num),
    MEMHANDOVER_ERROR_CODES
#undef X
};

/// @brief OK anchor: success is zero by convention.
static_assert(MEM_HANDOVER_ERR_OK == 0, "error OK must be 0");

/// @brief Return a human-readable string for a handover error code.
template <> inline const char *error_string(MemHandoverError e) {
    switch (e) {
#define X(name, num, msg)                                                      \
    case MEM_HANDOVER_ERR_##name:                                              \
        return msg;
        MEMHANDOVER_ERROR_CODES
#undef X
    }
    return "Unknown mem-handover error";
}

} // namespace kernel::errors
