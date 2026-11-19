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

/// @file carved_object.hpp
/// @brief Fixed-size carved capability object (F1, issue #554): MemPool
///        descriptor + PMM payload carved from an Untyped region for the
///        five fixed-size kinds (TCB, VSpace, PageTable, Notification,
///        SchedContext).  FrameCap storage pattern; ThreadCap tag pattern.

#pragma once

#include <types.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/object_sizing.hpp>

namespace kernel::cap {

/// @brief A fixed-size kernel object carved from an Untyped region.
///        Owns the PMM pages it wraps: the last reference frees them via
///        dispose().  Shared-heap class (FrameCap pattern).
///        F4 note (issue #557): a carved TCB carries no live binding at
///        F4 — the bind seam (retype-created TCB -> TcbCap authority) lands
///        with retype-only creation (M12/M14).  No code change here.
class CarvedObjectCap : public KernelObject {
  public:
    /// @brief Wrapping discriminator: stale-slot guard so a future
    /// Task-typed object can never be mistaken for a carved object
    /// after a static_cast from a lookup result.
    static constexpr uint32_t kCarvedObjectTag = 0x4F424A43U; // 'OBJC'

    /// @brief Tag word, always kCarvedObjectTag for a live object.
    uint32_t tag = kCarvedObjectTag;
    /// @brief Which fixed kind this object was carved as.
    CapType kind = CapType::Null;
    /// @brief First frame of the wrapped payload region.
    uint64_t phys = 0;
    /// @brief Payload size in bytes (the kind's fixed size).
    size_t bytes = 0;
    /// @brief PMM ownership class (FrameCap parity).
    bool is_user = false;

    /// @brief Carves a fixed-size object over [phys, phys + num_bytes).
    ///        Rejects non-carvable or variable-size kinds and any size
    ///        other than the kind's fixed size; zeroes the payload (a
    ///        stale page-table root is a privilege-escalation primitive,
    ///        spec §4.2 "no partial object").  Frame keeps raw contents
    ///        (existing contract, untouched).  Returns nullptr on any
    ///        failure.  All fields are set by construction; there is no
    ///        widening setter (S3-4).
    /// @return The carved object or nullptr on allocation failure.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    static CarvedObjectCap *create(CapType kind, uint64_t phys,
                                   size_t num_bytes, bool is_user);
    // NOLINTEND(bugprone-easily-swappable-parameters)

    /// @brief Final teardown: returns every wrapped page to the PMM and
    ///        the block to the MemPool.
    void dispose() noexcept override;

    /// @brief Marks the cap revoked.  F5 (issue #558) runs the VSpaceMap
    ///        invalidation here (same hooks as dispose()): a revoked
    ///        VSpace/PageTable keeps no live table link.  Both hooks are
    ///        idempotent, so revoke-then-dispose is safe.
    void revoke() noexcept override;

    /// @brief Genuinely shared (referenced by capability slots).
    bool is_shared() const noexcept override {
        return true;
    }
};

} // namespace kernel::cap
