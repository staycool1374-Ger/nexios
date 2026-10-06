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

/// @file vmm_errors.hpp
/// @brief Virtual Memory Manager error codes and string lookup.

#include <types.hpp>
#include <assert.hpp>

namespace kernel::errors {

#define VMM_ERROR_CODES                                                        \
    X(OK, 0, "OK")                                                             \
    X(PAGE_ALLOC, 1, "Failed to allocate page table page")                     \
    X(PML4_ALLOC, 2, "Failed to allocate PML4 page for clone")                 \
    X(INVALID_ADDR, 3, "Invalid virtual address")                              \
    X(NOT_MAPPED, 4, "Virtual address not mapped")

/// @brief Error codes for the Virtual Memory Manager.
// NOLINTNEXTLINE(performance-enum-size)
enum VmmError : uint64_t {
#define X(name, num, msg) VMM_ERR_##name = (num),
    VMM_ERROR_CODES
#undef X
};

/// @brief Return a human-readable string for a VMM error code.
template <> inline const char *error_string(VmmError e) {
    switch (e) {
#define X(name, num, msg)                                                      \
    case VMM_ERR_##name:                                                       \
        return msg;
        VMM_ERROR_CODES
#undef X
    }
    return "Unknown VMM error";
}

} // namespace kernel::errors
