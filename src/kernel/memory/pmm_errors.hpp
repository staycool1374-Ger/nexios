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

/// @file pmm_errors.hpp
/// @brief Physical Memory Manager error codes and string lookup.

#include <types.hpp>
#include <assert.hpp>

namespace kernel::errors {

#define PMM_ERROR_CODES                                                        \
    X(OK, 0, "OK")                                                             \
    X(OOM, 1, "Out of memory — no free physical pages")                        \
    X(USER_OOM, 2, "Out of memory — no free user physical pages")              \
    X(TABLE_OOM, 3, "Out of memory — page table pool exhausted")               \
    X(INVALID, 4, "Invalid physical address")

/// @brief Error codes for the Physical Memory Manager.
// NOLINTNEXTLINE(performance-enum-size)
enum PmmError : uint64_t {
#define X(name, num, msg) PMM_ERR_##name = (num),
    PMM_ERROR_CODES
#undef X
};

/// @brief Return a human-readable string for a PMM error code.
template <> inline const char *error_string(PmmError e) {
    switch (e) {
#define X(name, num, msg)                                                      \
    case PMM_ERR_##name:                                                       \
        return msg;
        PMM_ERROR_CODES
#undef X
    }
    return "Unknown PMM error";
}

} // namespace kernel::errors
