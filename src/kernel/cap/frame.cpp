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
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/test/resource_tracker.hpp>

// Placement new (defined in lib/new.cpp, no <new> header in freestanding)
inline void *operator new(unsigned long, void *p) noexcept {
    return p;
}

namespace kernel::cap {

FrameCap *FrameCap::create(uint64_t phys, size_t count, bool is_user) {
    auto *fc = static_cast<FrameCap *>(MemPool::alloc(sizeof(FrameCap)));
    if (!fc)
        return nullptr;
    new (fc) FrameCap;
    fc->mark_pool_backed();
    fc->phys = phys;
    fc->count = count;
    fc->is_user = is_user;
    kernel::test::ResourceTracker::instance().track_cap_object_add();
    return fc;
}

void FrameCap::dispose() noexcept {
    for (size_t i = 0; i < count; ++i)
        PMM::free_page(phys + i * arch::PAGE_SIZE);
    kernel::test::ResourceTracker::instance().track_cap_object_remove();
    MemPool::free(this);
}

} // namespace kernel::cap
