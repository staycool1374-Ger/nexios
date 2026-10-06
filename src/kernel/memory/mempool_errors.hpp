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

#include <types.hpp>
#include <assert.hpp>

namespace kernel::errors {

#define MEMPOOL_ERROR_CODES                                                    \
    X(OK, 0, "OK")                                                             \
    X(OOM, 1, "Out of memory in pool")                                         \
    X(TOO_LARGE, 2, "Requested size exceeds maximum pool block")               \
    X(INVALID_PTR, 3, "Pointer not owned by any pool")                         \
    X(DOUBLE_FREE, 4, "Block already freed")                                   \
    X(POOL_UNINIT, 5, "Pool not initialized")                                  \
    X(INVALID_POOL, 6, "Invalid pool index")                                   \
    X(CORRUPTED, 7, "Pool metadata corrupted")

/// @brief Error codes for the MemPool allocator.
// NOLINTNEXTLINE(performance-enum-size)
enum MemPoolError : uint64_t {
#define X(name, num, msg) MEMPOOL_ERR_##name = (num),
    MEMPOOL_ERROR_CODES
#undef X
};

/// @brief Return a human-readable string for a MemPool error code.
template <> inline const char *error_string(MemPoolError e) {
    switch (e) {
#define X(name, num, msg)                                                      \
    case MEMPOOL_ERR_##name:                                                   \
        return msg;
        MEMPOOL_ERROR_CODES
#undef X
    }
    return "Unknown MemPool error";
}

} // namespace kernel::errors