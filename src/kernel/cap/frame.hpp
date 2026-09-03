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

/// @file frame.hpp
/// @brief Capability-wrapped physical memory frame (shared-heap KernelObject).

#pragma once

#include <types.hpp>
#include <kernel/memory/kernel_object.hpp>

namespace kernel::cap {

/// @brief A capability-gated physical frame (or contiguous range).  Owns the
///        PMM pages it wraps: the last reference frees them via dispose().
///        Shared-heap class (PipeBuffer pattern).
class FrameCap : public KernelObject {
  public:
    uint64_t phys = 0;
    size_t count = 0;
    bool is_user = false;

    /// @brief Allocates a FrameCap from the MemPool and pool-marks it.
    ///        Returns nullptr on failure.
    static FrameCap *create(uint64_t phys, size_t count, bool is_user);

    /// @brief Final teardown: returns every wrapped frame to the PMM and the
    ///        block to the MemPool.  Also retroactively removes this cap's
    ///        user frame mappings (issue #106 revocation closure).
    void dispose() noexcept override;

    /// @brief Marks the cap revoked; retroactively removes this cap's user
    ///        frame mappings (issue #106).  Called by cap::revoke.
    void revoke() noexcept override;

    /// @brief Genuinely shared (referenced by capability slots).
    bool is_shared() const noexcept override {
        return true;
    }
};

} // namespace kernel::cap
