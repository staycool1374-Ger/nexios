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

/// @file wdog.cpp
/// @brief WdogCap implementation (issue #277). Bound by CONFIG_CAP_MAX_WDOG
/// via a TU-local live counter (mmio.cpp pattern); folds into the existing
/// cap_objects ResourceTracker counter. Single live cap per target pid is
/// enforced by a static claim registry (MsixCap entry-claim precedent).

#include <kernel/cap/wdog.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <constants.hpp>

// Placement new (defined in lib/new.cpp, no <new> header in freestanding)
inline void *operator new(unsigned long, void *placement) noexcept {
    return placement;
}

namespace kernel::cap {

/// @brief Live WdogCap count, bounded by CONFIG_CAP_MAX_WDOG.
static uint32_t g_live_wdogs = 0;

/// @brief Single-owner claim registry: at most one live cap per pid.
struct WdogClaim {
    bool occupied = false;
    uint64_t pid = 0;
};
static constexpr size_t kMaxWdogClaims = CONFIG_CAP_MAX_WDOG;
static WdogClaim g_wdog_claims[kMaxWdogClaims];

WdogCap *WdogCap::create(uint64_t target_pid) {
    if (target_pid == 0)
        return nullptr;
    auto *target = Scheduler::find_task(target_pid);
    if (!target || target->magic != TaskControlBlock::TCB_MAGIC)
        return nullptr;

    if (__atomic_load_n(&g_live_wdogs, __ATOMIC_RELAXED) >=
        static_cast<uint32_t>(CONFIG_CAP_MAX_WDOG))
        return nullptr;

    // Single-owner per pid: claim a registry slot up front.
    for (size_t i = 0;
         i < kMaxWdogClaims;
         ++i) {
        if (g_wdog_claims[i].occupied && g_wdog_claims[i].pid == target_pid)
            return nullptr;
    }
    size_t slot = kMaxWdogClaims;
    for (size_t i = 0;
         i < kMaxWdogClaims;
         ++i) {
        if (!g_wdog_claims[i].occupied) {
            slot = i;
            break;
        }
    }
    if (slot >= kMaxWdogClaims)
        return nullptr;
    g_wdog_claims[slot].occupied = true;
    g_wdog_claims[slot].pid = target_pid;

    auto *wdog = static_cast<WdogCap *>(MemPool::alloc(sizeof(WdogCap)));
    if (!wdog) {
        g_wdog_claims[slot].occupied = false;
        return nullptr;
    }
    new (wdog) WdogCap;
    wdog->mark_pool_backed();
    wdog->target_pid = target_pid;
    wdog->target_gen = target->generation;
    __atomic_fetch_add(&g_live_wdogs, 1U, __ATOMIC_RELAXED);
    kernel::test::ResourceTracker::instance().track_cap_object_add();
    return wdog;
}

/// @brief Release the claim-registry slot for @p pid (best-effort).
static void release_claim(uint64_t pid) {
    for (size_t i = 0;
         i < kMaxWdogClaims;
         ++i) {
        if (g_wdog_claims[i].occupied && g_wdog_claims[i].pid == pid) {
            g_wdog_claims[i].occupied = false;
            g_wdog_claims[i].pid = 0;
            return;
        }
    }
}

/// @brief Disarm the target without killing it (dispose/revoke shared).
static void disarm_target(WdogCap *wdog) {
    if (!wdog)
        return;
    auto *target = Scheduler::find_task(wdog->target_pid);
    if (target && target->magic == TaskControlBlock::TCB_MAGIC &&
        target->generation == wdog->target_gen) {
        target->wdog_armed = false;
        target->wdog_expiry_tick = 0;
        ++target->wdog_gen;
        if (target->wdog_gen == 0)
            target->wdog_gen = 1;
    }
    release_claim(wdog->target_pid);
}

void WdogCap::dispose() noexcept {
    disarm_target(this);
    if (__atomic_load_n(&g_live_wdogs, __ATOMIC_RELAXED) > 0)
        __atomic_fetch_sub(&g_live_wdogs, 1U, __ATOMIC_RELAXED);
    kernel::test::ResourceTracker::instance().track_cap_object_remove();
    MemPool::free(this);
}

void WdogCap::revoke() noexcept {
    disarm_target(this);
    // Mark revoked so acquire() refuses and lookup fails.
    KernelObject::revoke();
}

void WdogCap::snapshot_reset() noexcept {
    for (size_t i = 0;
         i < kMaxWdogClaims;
         ++i) {
        g_wdog_claims[i].occupied = false;
        g_wdog_claims[i].pid = 0;
    }
}

} // namespace kernel::cap
