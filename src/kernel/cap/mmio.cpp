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

/// @file mmio.cpp
/// @brief MmioCap implementation (issue #3).  Bound by CONFIG_CAP_MAX_MMIO
/// via a TU-local live counter (untyped.cpp pattern); folds into the existing
/// cap_objects ResourceTracker counter.  dispose never touches PMM — device
/// memory is not PMM-owned.

#include <kernel/cap/mmio.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <constants.hpp>

// Placement new (defined in lib/new.cpp, no <new> header in freestanding)
inline void *operator new(unsigned long, void *placement) noexcept {
    return placement;
}

namespace kernel::cap {

/// @brief Live MmioCap count, bounded by CONFIG_CAP_MAX_MMIO.
static uint32_t g_live_mmios = 0;

MmioCap *MmioCap::create(uint64_t phys, uint64_t size,
                         arch::PciBarType bar_type) {
    // Range validation (fail closed, never panic — reachable exhaustion).
    if (size == 0)
        return nullptr;
    if (bar_type == arch::PciBarType::IO) {
        // I/O port space is 16-bit: the whole range must fit in [0, 65536).
        if (phys >= 65536ULL || size > 65536ULL - phys)
            return nullptr;
    } else {
        // MMIO mapping is page-granular.
        if ((phys & (arch::PAGE_SIZE - 1)) != 0)
            return nullptr;
    }

    if (__atomic_load_n(&g_live_mmios, __ATOMIC_RELAXED) >=
        static_cast<uint32_t>(CONFIG_CAP_MAX_MMIO))
        return nullptr;

    auto *mmio = static_cast<MmioCap *>(MemPool::alloc(sizeof(MmioCap)));
    if (!mmio)
        return nullptr;
    new (mmio) MmioCap;
    mmio->mark_pool_backed();
    mmio->phys = phys;
    mmio->size = size;
    mmio->bar_type = bar_type;
    __atomic_fetch_add(&g_live_mmios, 1U, __ATOMIC_RELAXED);
    kernel::test::ResourceTracker::instance().track_cap_object_add();
    return mmio;
}

MmioCap *MmioCap::create_from_bar(const arch::PciBar &bar) {
    return create(bar.address, bar.size, bar.type);
}

void MmioCap::dispose() noexcept {
    if (__atomic_load_n(&g_live_mmios, __ATOMIC_RELAXED) > 0)
        __atomic_fetch_sub(&g_live_mmios, 1U, __ATOMIC_RELAXED);
    kernel::test::ResourceTracker::instance().track_cap_object_remove();
    MemPool::free(this);
}

} // namespace kernel::cap