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

/// @file cap.hpp
/// @brief CSpace core engine: CNode, slot operations, handle lookup, revoke.

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>
#include <kernel/cap/cap_types.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>

namespace kernel {
struct TaskControlBlock;
}

namespace kernel::cap {

/// @brief A capability node: a bounded table of slots.  Itself a KernelObject
///        (shared-heap class, PipeBuffer pattern).  A task's root CNode is its
///        CSpace and is attached to the TCB intrusive object list so teardown
///        releases it deterministically.
///
/// @note dispose() must never run while holding a cap spinlock (it calls
///       MemPool::free + ResourceTracker which take their own locks), so the
///       revoke path collects targets under the lock and releases them after
///       unlocking.
class CNode : public KernelObject {
  public:
    CSlot slots[CONFIG_CSLOT_COUNT];
    uint32_t cspace_id = 0;
    CNode *parent_ = nullptr;
    uint32_t depth_ = 0;
    sync::SpinLock lock_;

    /// @brief Allocates a CNode from the MemPool and marks it pool-backed.
    ///        Returns nullptr on allocation failure.
    static CNode *create(uint32_t cspace_id);

    /// @brief Final teardown on the last release: releases every slot target
    ///        exactly once, drops the ResourceTracker counter and returns the
    ///        block to the MemPool.
    void dispose() noexcept override;

    /// @brief Genuinely shared (may be referenced by multiple tasks/pins).
    bool is_shared() const noexcept override {
        return true;
    }

    /// @brief Cascade revoke: marks this node revoked and invalidates every
    ///        capability reachable through it (iterative, depth-bounded).
    void revoke() noexcept override;

    /// @brief Installs @p obj into the first free slot with the given type
    ///        and rights.  Takes a strong reference (acquire()) on success.
    /// @return slot index on success, -1 when full or the target is revoked.
    int install(KernelObject *obj, CapType type, uint32_t rights) noexcept;

    /// @brief Removes the slot at @p idx (clears and releases the target).
    ///        Idempotent: an already-free slot is a no-op.  The target is
    ///        released OUTSIDE the slot-table lock (deferred release list).
    void remove(uint32_t idx) noexcept;

    /// @brief Returns the slot at @p idx if it is occupied and matches the
    ///        requested type; nullptr otherwise.  Does NOT take a reference.
    KernelObject *peek(uint32_t idx, CapType want) const noexcept;

    /// @brief Returns the current generation of the slot at @p idx.
    uint32_t slot_gen(uint32_t idx) const noexcept;

    /// @brief Clears the GRANT right on the slot at @p idx (mint-once after
    ///        a grant).  No-op on an unoccupied slot.
    void clear_grant(uint32_t idx) noexcept;
};

/// @brief Looks up @p handle in @p cspace and returns a PREFIX-reference
///        (target->acquire() already taken) if the slot is occupied, matches
///        @p want, carries @p need_rights and the generation matches.
/// @return The pinned target (caller must release() it) or nullptr on any
///         validation failure.  Never returns a target without acquire().
KernelObject *lookup(CNode *cspace, uint64_t handle, CapType want,
                     uint32_t need_rights) noexcept;

/// @brief Revokes the capability at @p handle in @p cspace.  Idempotent.
/// @return true when a live capability was revoked, false when the slot was
///         already free (or the handle was invalid).
bool revoke(CNode *cspace, uint64_t handle) noexcept;

/// @brief Number of occupied slots (debug/leak audit helper).
size_t occupied_count(const CNode *cspace) noexcept;

/// @brief Capability lifecycle primitives (task context only).
///        All return -1 on failure (invalid handle, wrong type, missing
///        rights, revoked target, full destination).  On success they install
///        exactly one new slot and return its index.

/// @brief Copies the capability at @p src_handle of @p src into @p dst.
///        The destination slot inherits the source rights (COPY-capped).
int copy(CNode *src, uint64_t src_handle, CNode *dst) noexcept;

/// @brief Grants the capability at @p src_handle of @p src into @p dst.
///        Requires CAP_RIGHT_GRANT on the source slot; clears the source
///        GRANT right after use (mint-once semantics).
int grant(CNode *src, uint64_t src_handle, CNode *dst) noexcept;

/// @brief Copies the capability with a reduced rights mask and, for
///        endpoints, a new badge.
int mint(CNode *src, uint64_t src_handle, CNode *dst, uint32_t rights_mask,
         uint32_t badge) noexcept;

} // namespace kernel::cap
