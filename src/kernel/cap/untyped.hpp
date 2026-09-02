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

/// @file untyped.hpp
/// @brief Untyped memory object (ROADMAP 0.4.1 item 3): owns a contiguous PMM
///        region and may be retyped at most once into a capability.

#pragma once

#include <types.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/cap/cap_types.hpp>

namespace kernel::cap {

/// @brief Owns a contiguous PMM region and may be retyped into capabilities.
///
/// Shared-heap class (PipeBuffer/CNode pattern).  The retype guard
/// (`retyped_`) is the single ownership bit: while false the Untyped owns the
/// whole region and dispose() frees it; after a successful retype it owns
/// nothing and the carved-out capabilities own the frames.  Retype uses an
/// exhaustion model (issue #1): a sub-range carve creates a child Untyped for
/// the remainder, so a region can be split repeatedly (each child is itself
/// retypable).  No path frees the same frame twice.
class UntypedMem : public KernelObject {
  public:
    uint64_t phys = 0;                       ///< first frame of the owned region
    size_t size = 0;                         ///< region size in bytes (PAGE_SIZE multiple)
    bool is_user = false;                    ///< PMM ownership class (FrameCap parity)
    CapType retype_target = CapType::Frame;  ///< supported target type

    /// @brief Allocates a contiguous PMM region and a MemPool-backed UntypedMem.
    ///        Validates size > 0 and PAGE_SIZE-aligned; enforces
    ///        CONFIG_CAP_MAX_UNTYPED.  Returns nullptr on any failure (the
    ///        region is returned to PMM on partial failure).
    static UntypedMem *create(size_t size, bool is_user,
                              CapType target = CapType::Frame);

    /// @brief Wraps an already-owned, PAGE-aligned sub-range as a new Untyped
    ///        WITHOUT allocating frames (the child remainder of a sub-range
    ///        carve).  Enforces the shared CONFIG_CAP_MAX_UNTYPED live bound
    ///        and cap-object tracking.  Returns nullptr on any failure.
    static UntypedMem *create_subrange(uint64_t phys, size_t size,
                                       bool is_user,
                                       CapType target = CapType::Frame);

    /// @brief CAS false->true on the retype guard.  True iff this caller wins
    ///        the transfer of this Untyped's region.
    bool claim_once() noexcept;

    /// @brief Rolls a failed claim back (only legal while no other retype is
    ///        mid-flight and no object over the region was ever created — the
    ///        guard was set by this caller and never consumed).
    void reset_claim() noexcept {
        __atomic_store_n(&retyped_, false, __ATOMIC_RELEASE);
    }

    /// @brief True iff this Untyped still owns its whole region (never
    ///        retyped).  After a carve the parent owns nothing.
    bool owns_region() const noexcept;

    /// @brief Final teardown: frees the region ONLY if owns_region(); the
    ///        block is always returned to the MemPool.
    void dispose() noexcept override;

    /// @brief Genuinely shared (referenced by capability slots).
    bool is_shared() const noexcept override {
        return true;
    }

  private:
    volatile bool retyped_ = false;
};

} // namespace kernel::cap
