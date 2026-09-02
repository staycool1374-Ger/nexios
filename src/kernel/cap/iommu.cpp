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

/// @file iommu.cpp
/// @brief IoMmuDmaCap implementation (issue #4).  Bound by CONFIG_CAP_MAX_IOMMU
/// via a TU-local live counter (irq.cpp pattern); folds into the existing
/// cap_objects ResourceTracker counter.  The domain slot is claimed at
/// create() time and released at dispose/revoke — authority loss always
/// removes the domain FIRST (fail-closed DMA teardown).

#include <kernel/cap/iommu.hpp>
#include <kernel/iommu/iommu.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/nexios_config.h>

// Placement new (defined in lib/new.cpp, no <new> header in freestanding)
inline void *operator new(unsigned long, void *placement) noexcept {
    return placement;
}

namespace kernel::cap {

/// @brief Live IoMmuDmaCap count, bounded by CONFIG_CAP_MAX_IOMMU.
static uint32_t g_live_iommus = 0;

IoMmuDmaCap *IoMmuDmaCap::create() {
    if (__atomic_load_n(&g_live_iommus, __ATOMIC_RELAXED) >=
        static_cast<uint32_t>(CONFIG_CAP_MAX_IOMMU))
        return nullptr;

    // Presence gate first: no IOMMU -> no authority (graceful degradation).
    if (!iommu::IoMmuManager::probe())
        return nullptr;

    auto *t = Scheduler::current_task();
    if (!t)
        return nullptr;

    int16_t idx =
        iommu::IoMmuManager::domain_create(static_cast<uint32_t>(t->id));
    if (idx < 0)
        return nullptr; // domain table exhausted — fail closed

    auto *dma = static_cast<IoMmuDmaCap *>(MemPool::alloc(sizeof(IoMmuDmaCap)));
    if (!dma) {
        // Roll back the claimed domain — a failed alloc must not hold it.
        iommu::IoMmuManager::domain_destroy(idx);
        return nullptr;
    }
    new (dma) IoMmuDmaCap;
    dma->mark_pool_backed();
    dma->domain_idx_ = idx;
    dma->owner_task_id_ = static_cast<uint32_t>(t->id);
    __atomic_fetch_add(&g_live_iommus, 1U, __ATOMIC_RELAXED);
    kernel::test::ResourceTracker::instance().track_cap_object_add();
    return dma;
}

void IoMmuDmaCap::dispose() noexcept {
    if (domain_idx_ >= 0) {
        iommu::IoMmuManager::domain_destroy(domain_idx_);
        domain_idx_ = -1;
    }
    if (__atomic_load_n(&g_live_iommus, __ATOMIC_RELAXED) > 0)
        __atomic_fetch_sub(&g_live_iommus, 1U, __ATOMIC_RELAXED);
    kernel::test::ResourceTracker::instance().track_cap_object_remove();
    MemPool::free(this);
}

void IoMmuDmaCap::revoke() noexcept {
    // Fail-closed: authority loss removes ALL DMA access before the revoke
    // is published (after KernelObject::revoke() acquire() refuses, but the
    // domain must already be gone — never the other way around).
    if (domain_idx_ >= 0) {
        iommu::IoMmuManager::domain_destroy(domain_idx_);
        domain_idx_ = -1;
    }
    KernelObject::revoke();
}

} // namespace kernel::cap
