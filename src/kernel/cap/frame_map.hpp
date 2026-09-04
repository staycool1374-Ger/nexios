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

/// @file frame_map.hpp
/// @brief Capability-gated user-space frame (shared-memory) mapping registry
/// (issue #106 Part B).  Mirrors MmioUserMap (issue #8): maps a FrameCap's
/// physical frames into a fixed user VA window for Ring-3 shared-memory
/// producers/consumers via SYS_FRAME_MAP/SYS_FRAME_UNMAP.  Genuinely zero-copy
/// — two tasks mapping the same FrameCap share the same physical frames.

#pragma once

#include <types.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/sync/spinlock.hpp>

namespace kernel {
struct TaskControlBlock;
}

namespace kernel::cap {

/// @brief User-space frame mapping registry (issue #106 Part B).  A static
/// bounded table keyed by task id (MmioUserMap pattern) that maps a FrameCap's
/// frames into a fixed user VA window for Ring 3 shared memory via the
/// SYS_FRAME_MAP/SYS_FRAME_UNMAP syscall surface.  Keyed by owner_task_id + a
/// per-slot generation (recycled-slot stale-VA defense).  No dynamic
/// allocation on RT paths.  Lock order: scheduler_lock_ -> frame map lock
/// (never held across VMM map/unmap or reschedule).
class FrameUserMap {
  public:
    /// @brief Maps @p fc's frames into @p task's user VA window.
    /// @return The user VA base (CONFIG_USER_SHM_VA_BASE + slot*REGION), or
    ///         -1 on a revoked cap, a zero frame, a full registry, a missing
    ///         user page table, or a failed map.
    static uint64_t map(kernel::TaskControlBlock &task, cap::FrameCap &fc);

    /// @brief Unmaps the mapping at @p va if it belongs to @p task.
    /// @return true when a mapping was removed.
    static bool unmap(kernel::TaskControlBlock &task, uint64_t va);

    /// @brief Immediately removes every live mapping backed by @p fc
    ///        (revocation closure).  Called from FrameCap::dispose/revoke
    ///        before the cap block is freed.  Pointer-equality only — never
    ///        dereferenced.
    static void invalidate_cap(cap::FrameCap *fc);

    /// @brief Removes every live mapping owned by @p task.  Called from
    ///        TaskControlBlock::cleanup() (never leaves a dangling PTE in a
    ///        recycled PML4).
    static void drain_task(kernel::TaskControlBlock &task);

    /// @brief Clears the whole registry (test isolation snapshot restore).
    static void snapshot_reset();

    /// @brief Number of live mappings (test accessor).
    static size_t live_count();

    /// @brief Whether @p task currently owns the mapping at @p va (test
    ///        accessor).
    static bool is_owner(kernel::TaskControlBlock &task, uint64_t va);

  private:
    static constexpr size_t kMaxMaps = CONFIG_CAP_MAX_FRAME_MAPS;
    struct Slot {
        uint64_t owner_task_id = 0; ///< owning task (0 = free)
        uint64_t owner_gen = 0;     ///< TCB generation at map time
        cap::FrameCap *fc = nullptr; ///< backing cap (equality only)
        uint64_t va = 0;            ///< user VA base of the mapping
        uint64_t pml4 = 0;          ///< owning task's PML4 phys at map time
        bool occupied = false;      ///< slot in use
    };
    static Slot s_slots_[kMaxMaps];
    static sync::SpinLock s_lock_;
    static Slot *slot(size_t i);
};

} // namespace kernel::cap