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

/// @file frame_map.cpp
/// @brief FrameUserMap implementation (issue #106 Part B).  Mirrors MmioUserMap
/// (issue #8) exactly: static bounded slot array, claim-under-lock /
/// map-outside-lock, revalidate-before-release discipline.  A FrameCap owns
/// its PMM frames (dispose frees them on the last reference); every slot and
/// every user-map pin must be released before dispose.

#include <kernel/cap/frame_map.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <constants.hpp>

// Placement new (defined in lib/new.cpp, no <new> header in freestanding)
inline void *operator new(unsigned long, void *placement) noexcept {
    return placement;
}

namespace kernel::cap {

// Issue #106 Part B: the fixed user SHM window must sit below the user stack
// and clear of the heap AND the MMIO window.  The window base + all regions
// must fit.
static_assert(CONFIG_USER_SHM_VA_BASE >= mem::HEAP_VADDR + mem::HEAP_SIZE,
              "user SHM window must be above the user heap");
static_assert(CONFIG_USER_SHM_VA_BASE >=
                  CONFIG_USER_MMIO_VA_BASE +
                      static_cast<uint64_t>(CONFIG_CAP_MAX_MMIO_MAPS) *
                          CONFIG_USER_MMIO_REGION_SIZE,
              "user SHM window must sit above the MMIO window");
static_assert(CONFIG_USER_SHM_VA_BASE +
                      static_cast<uint64_t>(CONFIG_CAP_MAX_FRAME_MAPS) *
                          CONFIG_USER_SHM_REGION_SIZE <=
                  mem::STACK_VADDR,
              "user SHM window must fit below the user stack");
static_assert(CONFIG_USER_SHM_REGION_SIZE >= arch::PAGE_SIZE,
              "user SHM region must hold at least one page");
static_assert(CONFIG_CAP_MAX_FRAME_MAPS > 0,
              "CONFIG_CAP_MAX_FRAME_MAPS must be positive");

FrameUserMap::Slot FrameUserMap::s_slots_[FrameUserMap::kMaxMaps];
sync::SpinLock FrameUserMap::s_lock_{};

FrameUserMap::Slot *FrameUserMap::slot(size_t i) {
    if (i >= kMaxMaps)
        return nullptr;
    return &s_slots_[i];
}

uint64_t FrameUserMap::map(kernel::TaskControlBlock &task, cap::FrameCap &fc) {
    // Fail closed on a revoked cap or a zero frame (VMM parity).  A
    // kernel-backed (is_user == false) cap is NEVER user-mappable: mapping
    // kernel physical memory into the user VA window would be a privilege
    // escalation even if such a cap ever reached a user CNode (defense-in-depth
    // — SYS_FRAME_CREATE always installs is_user == true, and user retype of a
    // kernel Untyped is unreachable today, but the map path must not rely on
    // creation-side guarantees alone).
    if (fc.revoked() || !fc.is_user || fc.phys == 0 || fc.count == 0)
        return static_cast<uint64_t>(-1);
    const uint64_t bytes = fc.count * arch::PAGE_SIZE;
    if (bytes > CONFIG_USER_SHM_REGION_SIZE)
        return static_cast<uint64_t>(-1); // range exceeds one fixed region

    uint64_t pml4 = task.page_table_;
    if (pml4 == 0)
        return static_cast<uint64_t>(-1); // kernel task — no user PML4

    // Claim a slot under the lock (exhaustion fails closed).  The user VA is
    // slot-derived: BASE + slot*REGION — fixed, collision-free by construction.
    // The registry holds a reference on the backing cap for the lifetime of a
    // live slot so the VMM unmap path can never dereference a freed cap (UAF,
    // mmio.cpp precedent).  The caller (sys_frame_map) also holds a ref, so
    // this acquire cannot trigger dispose during map().
    int16_t idx = -1;
    uint64_t va = static_cast<uint64_t>(-1);
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t i = 0; i < kMaxMaps; ++i) {
            if (!s_slots_[i].occupied) {
                if (!fc.acquire())
                    return static_cast<uint64_t>(-1); // revoked — fail closed
                idx = static_cast<int16_t>(i);
                va = CONFIG_USER_SHM_VA_BASE +
                     static_cast<uint64_t>(i) * CONFIG_USER_SHM_REGION_SIZE;
                s_slots_[i].owner_task_id = task.id;
                s_slots_[i].owner_gen = task.generation;
                s_slots_[i].fc = &fc;
                s_slots_[i].va = va;
                s_slots_[i].pml4 = pml4;
                s_slots_[i].occupied = true;
                break;
            }
        }
    }
    if (idx < 0)
        return static_cast<uint64_t>(-1); // registry full — fail closed

    // Map OUTSIDE the registry lock (VMM walks allocate PMM pages; holding the
    // map lock across them would risk lock inversion with PMM/mempool).
    if (!VMM::map_frame_from_cap(&fc, va, /*user=*/true, pml4)) {
        // Roll back the claimed slot on a failed map and drop the slot's pin.
        // Re-validate the slot still holds THIS mapping: a concurrent
        // invalidate_cap (cross-task revoke) may have cleared the slot (and
        // released its pin) while we mapped outside the lock.  Only the path
        // that observes the occupied true->false transition releases the pin —
        // releasing unconditionally is a double-release (refcount corruption ->
        // premature dispose -> use-after-free).
        bool cleared = false;
        {
            SpinLockGuard<sync::SpinLock> guard(s_lock_);
            auto &sl = s_slots_[static_cast<size_t>(idx)];
            if (sl.occupied && sl.fc == &fc && sl.va == va) {
                sl.occupied = false;
                cleared = true;
            }
        }
        if (cleared)
            fc.release(); // slot pin only; the caller still holds its own ref
        return static_cast<uint64_t>(-1);
    }
    return va;
}

bool FrameUserMap::unmap(kernel::TaskControlBlock &task, uint64_t va) {
    // Find the slot by VA under the lock; ownership + generation revalidated
    // (a recycled task id or stale VA must not unmap another task's mapping).
    int16_t idx = -1;
    cap::FrameCap *fc = nullptr;
    uint64_t pml4 = 0;
    size_t pages = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t i = 0; i < kMaxMaps; ++i) {
            if (s_slots_[i].occupied && s_slots_[i].va == va &&
                s_slots_[i].owner_task_id == task.id &&
                s_slots_[i].owner_gen == task.generation) {
                idx = static_cast<int16_t>(i);
                fc = s_slots_[i].fc;
                pml4 = s_slots_[i].pml4;
                // Pin is held here — the count read is safe.  Capture it for
                // the unmap loop below (VMM::unmap_frame_from_cap clears ONE
                // PTE; map_frame_from_cap mapped fc->count pages).
                pages = s_slots_[i].fc->count;
                break;
            }
        }
    }
    if (idx < 0)
        return false;

    // Unmap every PTE outside the lock, then free the slot.  The slot's own
    // reference keeps @p fc alive across the unmap (the caller, sys_frame_unmap,
    // holds none), so the VMM unmap deref is safe.  Re-validate the slot at the
    // clear step: a concurrent invalidate_cap (cross-task revoke) may clear
    // this slot (and release its pin) while we unmap outside the lock.  Only
    // the path that observes the occupied true->false transition releases the
    // slot's pin — releasing unconditionally is a double-release.
    for (size_t i = 0; i < pages; ++i)
        VMM::unmap_frame_from_cap(va + i * arch::PAGE_SIZE, pml4);
    bool cleared = false;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        auto &sl = s_slots_[static_cast<size_t>(idx)];
        if (sl.occupied && sl.fc == fc && sl.va == va &&
            sl.owner_task_id == task.id && sl.owner_gen == task.generation) {
            sl.occupied = false;
            cleared = true;
        }
    }
    if (cleared)
        fc->release(); // drop the slot's pin outside the lock (may dispose)
    return true;
}

void FrameUserMap::invalidate_cap(cap::FrameCap *fc) {
    // Collect matching slots under the lock, unmap their PTEs outside it,
    // then free the slots and drop each slot's pin.  Pointer-equality only.
    // At dispose() time no live slot references the cap (each slot holds its
    // own ref, so dispose cannot fire while mapped), so this is normally a
    // no-op; at revoke() time it clears live slots and releases their pins.
    for (size_t i = 0; i < kMaxMaps; ++i) {
        uint64_t va = 0;
        uint64_t pml4 = 0;
        cap::FrameCap *match = nullptr;
        size_t pages = 0;
        bool hit = false;
        {
            SpinLockGuard<sync::SpinLock> guard(s_lock_);
            if (!s_slots_[i].occupied || s_slots_[i].fc != fc)
                continue;
            va = s_slots_[i].va;
            match = s_slots_[i].fc;
            // The slot stores the PML4 phys captured at map time — the owning
            // task may not be scheduler-resolvable (e.g. a stand-alone test
            // fixture), so never re-resolve by id here.
            pml4 = s_slots_[i].pml4;
            // Pin is held here — the count read is safe.
            pages = s_slots_[i].fc->count;
            s_slots_[i].occupied = false;
            hit = true;
        }
        if (hit) {
            // Clear EVERY page of a multi-page mapping (map_frame_from_cap
            // mapped fc->count pages; unmap_frame_from_cap clears one PTE).
            for (size_t j = 0; j < pages; ++j)
                VMM::unmap_frame_from_cap(va + j * arch::PAGE_SIZE, pml4);
            match->release(); // drop the slot's pin (outside the lock)
        }
    }
}

void FrameUserMap::drain_task(kernel::TaskControlBlock &task) {
    for (size_t i = 0; i < kMaxMaps; ++i) {
        uint64_t va = 0;
        uint64_t pml4 = 0;
        cap::FrameCap *fc = nullptr;
        size_t pages = 0;
        bool hit = false;
        {
            SpinLockGuard<sync::SpinLock> guard(s_lock_);
            if (!s_slots_[i].occupied ||
                s_slots_[i].owner_task_id != task.id)
                continue;
            va = s_slots_[i].va;
            fc = s_slots_[i].fc;
            pml4 = s_slots_[i].pml4;
            // Pin is held here — the count read is safe.
            pages = s_slots_[i].fc->count;
            s_slots_[i].occupied = false;
            hit = true;
        }
        if (hit) {
            // Clear EVERY page of a multi-page mapping before the owning
            // task's PML4 is freed (map_frame_from_cap mapped fc->count
            // pages; unmap_frame_from_cap clears one PTE).
            for (size_t j = 0; j < pages; ++j)
                VMM::unmap_frame_from_cap(va + j * arch::PAGE_SIZE, pml4);
            fc->release(); // drop the slot's pin (outside the lock)
        }
    }
}

void FrameUserMap::snapshot_reset() {
    // Clear the whole registry and drop each slot's pin.  Pins are released
    // OUTSIDE the lock: the last release may run dispose() -> MemPool::free
    // (and dispose calls invalidate_cap, which takes s_lock_).
    cap::FrameCap *to_release[kMaxMaps];
    size_t n = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t i = 0; i < kMaxMaps; ++i) {
            if (s_slots_[i].occupied)
                to_release[n++] = s_slots_[i].fc;
            s_slots_[i].owner_task_id = 0;
            s_slots_[i].owner_gen = 0;
            s_slots_[i].fc = nullptr;
            s_slots_[i].va = 0;
            s_slots_[i].pml4 = 0;
            s_slots_[i].occupied = false;
        }
    }
    for (size_t i = 0; i < n; ++i)
        to_release[i]->release();
}

size_t FrameUserMap::live_count() {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    size_t n = 0;
    for (size_t i = 0; i < kMaxMaps; ++i)
        if (s_slots_[i].occupied)
            ++n;
    return n;
}

bool FrameUserMap::is_owner(kernel::TaskControlBlock &task, uint64_t va) {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    for (size_t i = 0; i < kMaxMaps; ++i)
        if (s_slots_[i].occupied && s_slots_[i].va == va &&
            s_slots_[i].owner_task_id == task.id &&
            s_slots_[i].owner_gen == task.generation)
            return true;
    return false;
}

} // namespace kernel::cap