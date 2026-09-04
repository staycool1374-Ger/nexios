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

/// @file frame.cpp
/// @brief Capability-wrapped physical memory frame implementation.

#include <kernel/cap/frame.hpp>
#include <kernel/cap/frame_map.hpp>
#include <kernel/ipc/pager_registry.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/test/resource_tracker.hpp>

// Placement new (defined in lib/new.cpp, no <new> header in freestanding)
inline void *operator new(unsigned long, void *placement) noexcept {
    return placement;
}

namespace kernel::cap {

FrameCap *FrameCap::create(uint64_t phys, size_t count, bool is_user) {
    auto *frame = static_cast<FrameCap *>(MemPool::alloc(sizeof(FrameCap)));
    if (!frame)
        return nullptr;
    new (frame) FrameCap;
    frame->mark_pool_backed();
    frame->phys = phys;
    frame->count = count;
    frame->is_user = is_user;
    kernel::test::ResourceTracker::instance().track_cap_object_add();
    return frame;
}

void FrameCap::dispose() noexcept {
    // Issue #106 Part B revocation closure: before the cap block is freed,
    // retroactively remove this cap's user frame mappings so a live task's
    // mapped shared frames cannot outlive the capability.  The cap pointer is
    // non-owning (equality-match only) in the map registry — never
    // dereferenced after this point.
    FrameUserMap::invalidate_cap(this);
    // Issue #107: same closure for the pager registry's fault ledger — a
    // pager-mapped page backed by this cap must be unmapped + unpinned before
    // the frames are freed.
    ipc::PagerRegistry::invalidate_cap(this);
    for (size_t i = 0; i < count; ++i)
        PMM::free_page(phys + i * arch::PAGE_SIZE);
    kernel::test::ResourceTracker::instance().track_cap_object_remove();
    MemPool::free(this);
}

void FrameCap::revoke() noexcept {
    // Issue #106 Part B: same closure as dispose — a revoked capability must
    // not leave stale frame mappings behind (fail-closed, MMIO parity).
    FrameUserMap::invalidate_cap(this);
    // Issue #107: pager-ledger closure (same rationale as dispose).
    ipc::PagerRegistry::invalidate_cap(this);
    KernelObject::revoke();
}

} // namespace kernel::cap
