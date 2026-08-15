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

/// @file endpoint.cpp
/// @brief Capability-backed IPC endpoint object implementation.

#include <kernel/cap/endpoint.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/test/resource_tracker.hpp>

// Placement new (defined in lib/new.cpp, no <new> header in freestanding)
inline void *operator new(unsigned long, void *placement) noexcept {
    return placement;
}

namespace kernel::cap {

Endpoint *Endpoint::create(uint32_t badge) {
    auto *endpoint = static_cast<Endpoint *>(MemPool::alloc(sizeof(Endpoint)));
    if (!endpoint)
        return nullptr;
    new (endpoint) Endpoint;
    endpoint->mark_pool_backed();
    endpoint->badge = badge;
    endpoint->q.init();
    endpoint->q.owner = nullptr;
    kernel::test::ResourceTracker::instance().track_cap_object_add();
    return endpoint;
}

void Endpoint::dispose() noexcept {
    bound_receiver = nullptr;
    kernel::test::ResourceTracker::instance().track_cap_object_remove();
    MemPool::free(this);
}

} // namespace kernel::cap
