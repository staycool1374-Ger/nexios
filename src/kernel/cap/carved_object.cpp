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

/// @file carved_object.cpp
/// @brief Fixed-size carved capability object implementation (F1 #554).

#include <kernel/cap/carved_object.hpp>
#include <kernel/cap/vspace_map.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/task/task.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <constants.hpp>

// Placement new (defined in lib/new.cpp, no <new> header in freestanding)
inline void *operator new(unsigned long, void *placement) noexcept {
    return placement;
}

// Compile-time fail-closed: the carved TCB storage must fit the worst-case
// TaskControlBlock on every arch (complements task.cpp:946-955 and
// task.hpp:1403, does not replace them).
static_assert(sizeof(kernel::TaskControlBlock) <=
                  kernel::cap::kObjectBytesTcb,
              "TCB object storage too small");

namespace kernel::cap {

CarvedObjectCap *CarvedObjectCap::create(CapType kind, uint64_t phys,
                                         size_t num_bytes,
                                         bool is_user) {
    const ObjectSizing sizing = object_sizing(kind);
    if (!sizing.carvable || sizing.variable_size)
        return nullptr;
    if (num_bytes != sizing.bytes)
        return nullptr;
    auto *carved =
        static_cast<CarvedObjectCap *>(MemPool::alloc(sizeof(CarvedObjectCap)));
    if (!carved)
        return nullptr;
    new (carved) CarvedObjectCap;
    carved->mark_pool_backed();
    carved->kind = kind;
    carved->phys = phys;
    carved->bytes = num_bytes;
    carved->is_user = is_user;
    kernel::test::ResourceTracker::instance().track_cap_object_add();
    // Mandatory bounded zeroing (INV-RT-4, at most kObjectBytesMax): the
    // HHDM write pattern is ramdiskd.cpp:82-84 (same translation the
    // pager paths use).  A stale page-table root handed to a future
    // VSpace/PageTable consumer is a privilege-escalation primitive.
    __builtin_memset(reinterpret_cast<void *>(arch::HHDM_OFFSET + phys), 0,
                     num_bytes);
    return carved;
}

void CarvedObjectCap::dispose() noexcept {
    // F5 (issue #558, INV-VS-4): destroy the table link BEFORE the
    // payload pages go back to the PMM.  Reversing that order hands a
    // live parent a page the allocator may re-issue (S1).  The hook is
    // idempotent, so revoke-then-dispose is safe.
    if (kind == CapType::PageTable)
        VSpaceMap::invalidate_table(phys);
    else if (kind == CapType::VSpace)
        VSpaceMap::invalidate_vspace(phys);
    const size_t page_count = bytes / arch::PAGE_SIZE;
    for (size_t page_idx = 0; page_idx < page_count; ++page_idx)
        PMM::free_page(phys + page_idx * arch::PAGE_SIZE);
    kernel::test::ResourceTracker::instance().track_cap_object_remove();
    MemPool::free(this);
}

void CarvedObjectCap::revoke() noexcept {
    // F5 (issue #558): a revoked carved object keeps no live mapping —
    // the same invalidation dispose() runs, before the revoked mark, so
    // no parent ever points at a page whose owner went away.
    if (kind == CapType::PageTable)
        VSpaceMap::invalidate_table(phys);
    else if (kind == CapType::VSpace)
        VSpaceMap::invalidate_vspace(phys);
    KernelObject::revoke();
}

} // namespace kernel::cap
