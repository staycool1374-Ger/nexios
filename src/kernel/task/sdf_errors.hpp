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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/// @file sdf_errors.hpp
/// @brief SDF compose-time bound-check errors (F10, #563; X-macro table).
///        Every refusal at the sdf_check_bounds() boundary carries one of
///        these codes — never a generic false.

#include <types.hpp>
#include <assert.hpp>

namespace kernel::errors {

#define SDF_ERROR_CODES                                                          \
    X(OK, 0, "OK")                                                               \
    X(BOUND_EXCEEDED, 1, "SDF row demand exceeds a configured bound")            \
    X(ROW_TOO_WIDE, 2, "SDF row wider than the compose stack array")             \
    X(OBJECTS_EXCEEDED, 3, "SDF row exceeds the task object budget")             \
    X(CSPACE_EXCEEDED, 4, "SDF row exceeds the CSpace slot bound")

/// @brief SDF compose-time bound errors.
// NOLINTNEXTLINE(performance-enum-size)
enum SdfError : uint64_t {
#define X(name, num, msg) SDF_ERR_##name = (num),
    SDF_ERROR_CODES
#undef X
};

/// @brief OK anchor: success is zero by convention.
static_assert(SDF_ERR_OK == 0, "error OK must be 0");

/// @brief Return a human-readable string for an SDF error code.
template <> inline const char *error_string(SdfError e) {
    switch (e) {
#define X(name, num, msg)                                                      \
    case SDF_ERR_##name:                                                       \
        return msg;
        SDF_ERROR_CODES
#undef X
    }
    return "Unknown SDF error";
}

} // namespace kernel::errors
