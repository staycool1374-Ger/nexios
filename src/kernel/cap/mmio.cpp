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
/// @brief MmioCap implementation (issue #3/#8).  Bound by CONFIG_CAP_MAX_MMIO
/// via a TU-local live counter (untyped.cpp pattern); folds into the existing
/// cap_objects ResourceTracker counter.  dispose never touches PMM — device
/// memory is not PMM-owned.  Issue #8: dispose/revoke also retroactively
/// remove this cap's IOPB grants (arch::iopb_ledger_clear_cap) and user MMIO
/// mappings (MmioUserMap::invalidate_cap), closing the #3 revocation gap.

#include <kernel/cap/mmio.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/hal/iopb.hpp>
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

/// @brief Live MmioCap count, bounded by CONFIG_CAP_MAX_MMIO.
static uint32_t g_live_mmios = 0;

// Issue #8: the fixed user MMIO window must sit below the user stack and
// clear of the heap (ELF segments must end below HEAP_VADDR; the heap grows
// to HEAP_VADDR + HEAP_SIZE).  The window base + all regions must fit.
static_assert(CONFIG_USER_MMIO_VA_BASE >=
                  mem::HEAP_VADDR + mem::HEAP_SIZE,
              "user MMIO window must be above the user heap");
static_assert(CONFIG_USER_MMIO_VA_BASE +
                      static_cast<uint64_t>(CONFIG_CAP_MAX_MMIO_MAPS) *
                          CONFIG_USER_MMIO_REGION_SIZE <=
                  mem::STACK_VADDR,
              "user MMIO window must fit below the user stack");
static_assert(CONFIG_USER_MMIO_REGION_SIZE >= arch::PAGE_SIZE,
              "user MMIO region must hold at least one page");
// The map registry is a static bounded array — no dynamic allocation on RT
// paths (MISRA/ISO 26262 mandatory rule).
static_assert(CONFIG_CAP_MAX_MMIO_MAPS > 0,
              "CONFIG_CAP_MAX_MMIO_MAPS must be positive");

MmioUserMap::Slot MmioUserMap::s_slots_[MmioUserMap::kMaxMaps];
sync::SpinLock MmioUserMap::s_lock_{};

MmioUserMap::Slot *MmioUserMap::slot(size_t i) {
    if (i >= kMaxMaps)
        return nullptr;
    return &s_slots_[i];
}

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
    // Issue #8 revocation closure: before the cap block is freed, retroactively
    // remove this cap's IOPB grants and user MMIO mappings so a live task's
    // granted port bits / mapped device pages cannot outlive the capability.
    // The cap pointer is non-owning (equality-match only) in the ledger and
    // map registry — never dereferenced after this point.
    arch::iopb_ledger_clear_cap(this);
    MmioUserMap::invalidate_cap(this);
    if (__atomic_load_n(&g_live_mmios, __ATOMIC_RELAXED) > 0)
        __atomic_fetch_sub(&g_live_mmios, 1U, __ATOMIC_RELAXED);
    kernel::test::ResourceTracker::instance().track_cap_object_remove();
    MemPool::free(this);
}

void MmioCap::revoke() noexcept {
    // Issue #8: same closure as dispose — a revoked capability must not leave
    // stale grants/mappings behind (fail-closed, IOPB parity).
    arch::iopb_ledger_clear_cap(this);
    MmioUserMap::invalidate_cap(this);
    KernelObject::revoke();
}

uint64_t MmioUserMap::map(kernel::TaskControlBlock &task, cap::MmioCap &mmio) {
    // Fail closed on a revoked cap, an IO-type range (delegated via
    // sys_ioport_grant, not MMIO) and a zero range (VMM parity).
    if (mmio.revoked() || mmio.bar_type == arch::PciBarType::IO ||
        mmio.phys == 0 || mmio.size == 0)
        return static_cast<uint64_t>(-1);
    const size_t pages =
        (mmio.size + arch::PAGE_SIZE - 1) / arch::PAGE_SIZE;
    if (pages * arch::PAGE_SIZE > CONFIG_USER_MMIO_REGION_SIZE)
        return static_cast<uint64_t>(-1); // range exceeds one fixed region

    uint64_t pml4 = task.page_table_;
    if (pml4 == 0)
        return static_cast<uint64_t>(-1); // kernel task — no user PML4

    // Claim a slot under the lock (exhaustion fails closed).  The user VA is
    // slot-derived: BASE + slot*REGION — fixed, collision-free by construction.
    // The registry holds a reference on the backing cap for the lifetime of a
    // live slot so the VMM unmap path (which reads mmio->size) can never
    // dereference a freed cap (issue #8, UAF fix).  The caller (sys_mmio_map)
    // also holds a ref, so this acquire cannot trigger dispose during map().
    int16_t idx = -1;
    uint64_t va = static_cast<uint64_t>(-1);
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t i = 0; i < kMaxMaps; ++i) {
            if (!s_slots_[i].occupied) {
                if (!mmio.acquire())
                    return static_cast<uint64_t>(-1); // revoked — fail closed
                idx = static_cast<int16_t>(i);
                va = CONFIG_USER_MMIO_VA_BASE +
                     static_cast<uint64_t>(i) * CONFIG_USER_MMIO_REGION_SIZE;
                s_slots_[i].owner_task_id = task.id;
                s_slots_[i].owner_gen = task.generation;
                s_slots_[i].mmio = &mmio;
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
    if (!VMM::map_mmio_from_cap(&mmio, va, /*user=*/true, pml4)) {
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
            if (sl.occupied && sl.mmio == &mmio && sl.va == va) {
                sl.occupied = false;
                cleared = true;
            }
        }
        if (cleared)
            mmio.release(); // slot pin only; the caller still holds its own ref
        return static_cast<uint64_t>(-1);
    }
    return va;
}

bool MmioUserMap::unmap(kernel::TaskControlBlock &task, uint64_t va) {
    // Find the slot by VA under the lock; ownership + generation revalidated
    // (a recycled task id or stale VA must not unmap another task's mapping).
    int16_t idx = -1;
    cap::MmioCap *mmio = nullptr;
    uint64_t pml4 = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t i = 0; i < kMaxMaps; ++i) {
            if (s_slots_[i].occupied && s_slots_[i].va == va &&
                s_slots_[i].owner_task_id == task.id &&
                s_slots_[i].owner_gen == task.generation) {
                idx = static_cast<int16_t>(i);
                mmio = s_slots_[i].mmio;
                pml4 = s_slots_[i].pml4;
                break;
            }
        }
    }
    if (idx < 0)
        return false;

    // Unmap the PTE outside the lock, then free the slot.  The slot's own
    // reference keeps @p mmio alive across the unmap (the caller, sys_mmio_unmap,
    // holds none), so the mmio->size deref in unmap_mmio_from_cap is safe.
    // Re-validate the slot at the clear step: a concurrent invalidate_cap
    // (cross-task revoke) may clear this slot (and release its pin) while we
    // unmap outside the lock.  Only the path that observes the occupied
    // true->false transition releases the slot's pin — releasing
    // unconditionally is a double-release (refcount corruption -> premature
    // dispose -> use-after-free).
    VMM::unmap_mmio_from_cap(mmio, va, pml4);
    bool cleared = false;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        auto &sl = s_slots_[static_cast<size_t>(idx)];
        if (sl.occupied && sl.mmio == mmio && sl.va == va &&
            sl.owner_task_id == task.id && sl.owner_gen == task.generation) {
            sl.occupied = false;
            cleared = true;
        }
    }
    if (cleared)
        mmio->release(); // drop the slot's pin outside the lock (may dispose)
    return true;
}

void MmioUserMap::invalidate_cap(cap::MmioCap *mmio) {
    // Collect matching slots under the lock, unmap their PTEs outside it,
    // then free the slots and drop each slot's pin.  Pointer-equality only.
    // At dispose() time no live slot references the cap (each slot holds its
    // own ref, so dispose cannot fire while mapped), so this is normally a
    // no-op; at revoke() time it clears live slots and releases their pins.
    for (size_t i = 0; i < kMaxMaps; ++i) {
        uint64_t va = 0;
        uint64_t pml4 = 0;
        cap::MmioCap *match = nullptr;
        bool hit = false;
        {
            SpinLockGuard<sync::SpinLock> guard(s_lock_);
            if (!s_slots_[i].occupied || s_slots_[i].mmio != mmio)
                continue;
            va = s_slots_[i].va;
            match = s_slots_[i].mmio;
            // The slot stores the PML4 phys captured at map time — the owning
            // task may not be scheduler-resolvable (e.g. a stand-alone test
            // fixture), so never re-resolve by id here.
            pml4 = s_slots_[i].pml4;
            s_slots_[i].occupied = false;
            hit = true;
        }
        if (hit) {
            VMM::unmap_mmio_from_cap(match, va, pml4);
            match->release(); // drop the slot's pin (outside the lock)
        }
    }
}

void MmioUserMap::drain_task(kernel::TaskControlBlock &task) {
    for (size_t i = 0; i < kMaxMaps; ++i) {
        uint64_t va = 0;
        uint64_t pml4 = 0;
        cap::MmioCap *mmio = nullptr;
        bool hit = false;
        {
            SpinLockGuard<sync::SpinLock> guard(s_lock_);
            if (!s_slots_[i].occupied ||
                s_slots_[i].owner_task_id != task.id)
                continue;
            va = s_slots_[i].va;
            mmio = s_slots_[i].mmio;
            pml4 = s_slots_[i].pml4;
            s_slots_[i].occupied = false;
            hit = true;
        }
        if (hit) {
            VMM::unmap_mmio_from_cap(mmio, va, pml4);
            mmio->release(); // drop the slot's pin (outside the lock)
        }
    }
}

void MmioUserMap::snapshot_reset() {
    // Clear the whole registry and drop each slot's pin.  Pins are released
    // OUTSIDE the lock: the last release may run dispose() -> MemPool::free
    // (and dispose calls invalidate_cap, which takes s_lock_).
    cap::MmioCap *to_release[kMaxMaps];
    size_t n = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t i = 0; i < kMaxMaps; ++i) {
            if (s_slots_[i].occupied)
                to_release[n++] = s_slots_[i].mmio;
            s_slots_[i].owner_task_id = 0;
            s_slots_[i].owner_gen = 0;
            s_slots_[i].mmio = nullptr;
            s_slots_[i].va = 0;
            s_slots_[i].pml4 = 0;
            s_slots_[i].occupied = false;
        }
    }
    for (size_t i = 0; i < n; ++i)
        to_release[i]->release();
}


size_t MmioUserMap::live_count() {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    size_t n = 0;
    for (size_t i = 0; i < kMaxMaps; ++i)
        if (s_slots_[i].occupied)
            ++n;
    return n;
}

bool MmioUserMap::is_owner(kernel::TaskControlBlock &task, uint64_t va) {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    for (size_t i = 0; i < kMaxMaps; ++i)
        if (s_slots_[i].occupied && s_slots_[i].va == va &&
            s_slots_[i].owner_task_id == task.id &&
            s_slots_[i].owner_gen == task.generation)
            return true;
    return false;
}

} // namespace kernel::cap
