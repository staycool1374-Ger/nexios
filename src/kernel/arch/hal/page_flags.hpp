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

/// @file page_flags.hpp
/// @brief Architecture-independent page flag constants for page table entries.

#include <types.hpp>

namespace kernel {

/// @brief Architecture-independent page flags for page table operations.
/// Used by VMM and ArchPageTable to build page table entries.
struct PageFlags {
    /// @brief Page is present in memory.
    static constexpr uint64_t PRESENT = 1ULL << 0;
    /// @brief Page is writable.
    static constexpr uint64_t WRITE = 1ULL << 1;
    /// @brief Page is user-accessible (EL0).
    static constexpr uint64_t USER = 1ULL << 2;
    /// @brief No-execute — disables instruction fetch.
    static constexpr uint64_t NX = 1ULL << 3;
    /// @brief Uncached / device memory attribute.
    static constexpr uint64_t CACHE_DISABLED = 1ULL << 4;
    /// @brief Huge page (2 MiB or 1 GiB, detected via page-table level).
    static constexpr uint64_t HUGE = 1ULL << 7;
};

} // namespace kernel
