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

/// @file frame_foreign_map.cpp
/// @brief Foreign frame mapping registry implementation (issue #564,
///        F11 Stage C).  Mirrors FrameUserMap exactly (claim-under-lock /
///        map-outside-lock, revalidate-before-release): the registry holds
///        a reference on the backing cap for the lifetime of a live slot
///        so the VMM unmap path can never dereference a freed cap.

#include <kernel/cap/frame_foreign_map.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <constants.hpp>

#if defined(CONFIG_DEBUG)
extern "C" void debug_write(const char *s);
extern "C" void debug_write_hex(uint64_t value);
#endif

namespace kernel::cap {

static_assert(CONFIG_CAP_MAX_FRAME_MAPS > 0,
              "CONFIG_CAP_MAX_FRAME_MAPS must be positive");

ForeignFrameMap::Slot ForeignFrameMap::s_slots_[kMaxMaps];
sync::SpinLock ForeignFrameMap::s_lock_{};

errors::VmmError ForeignFrameMap::map(cap::FrameCap &fc,
                                         uint64_t vspace_phys,
                                         uint64_t va, bool executable,
                                         bool writable) {
    // Fail closed on a revoked cap, a kernel-backed cap (never
    // user-mappable — FrameUserMap precedent), a zero frame, or
    // misaligned addresses (VMM parity).  Registry exhaustion surfaces
    // as PAGE_ALLOC (slot-allocation failure, fail-closed).
    if (fc.revoked() || !fc.is_user || fc.phys == 0 || fc.count == 0)
        return errors::VMM_ERR_INVALID_ADDR;
    if (vspace_phys == 0 || va == 0)
        return errors::VMM_ERR_INVALID_ADDR;
    if ((va & (arch::PAGE_SIZE - 1u)) != 0 ||
        (vspace_phys & (arch::PAGE_SIZE - 1u)) != 0)
        return errors::VMM_ERR_INVALID_ADDR;
    // Claim a slot under the lock (exhaustion fails closed).  A live
    // slot overlapping the same root's window refuses the claim: the
    // pre-probe in sys_vspace_frame_map covers committed mappings, this
    // covers claimed-but-unmapped in-flight slots from a concurrent map
    // on the same root (fail-closed either way).  The registry holds a
    // reference on the backing cap for the lifetime of a live slot (the
    // caller also holds a ref, so this acquire cannot trigger dispose
    // during map()).
    if (fc.count > (0xFFFFFFFFFFFFFFFFULL / arch::PAGE_SIZE))
        return errors::VMM_ERR_INVALID_ADDR;
    const uint64_t bytes = fc.count * arch::PAGE_SIZE;
    if (va + bytes < va)
        return errors::VMM_ERR_INVALID_ADDR; // wrap — fail closed
    int16_t idx = -1;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t scan_idx = 0; scan_idx < kMaxMaps; ++scan_idx) {
            const Slot &live = s_slots_[scan_idx];
            if (live.occupied && live.vspace_phys == vspace_phys) {
                const uint64_t live_bytes =
                    live.pages * arch::PAGE_SIZE;
                const bool disjoint =
                    (va + bytes <= live.va) ||
                    (live.va + live_bytes <= va);
                if (!disjoint)
                    return errors::VMM_ERR_INVALID_ADDR; // overlap
            }
        }
        for (size_t scan_idx = 0; scan_idx < kMaxMaps; ++scan_idx) {
            if (!s_slots_[scan_idx].occupied) {
                if (!fc.acquire())
                    return errors::VMM_ERR_INVALID_ADDR; // revoked
                idx = static_cast<int16_t>(scan_idx);
                s_slots_[scan_idx].vspace_phys = vspace_phys;
                s_slots_[scan_idx].va = va;
                s_slots_[scan_idx].pages = fc.count;
                s_slots_[scan_idx].fc = &fc;
                s_slots_[scan_idx].occupied = true;
                break;
            }
        }
    }
    if (idx < 0)
        return errors::VMM_ERR_PAGE_ALLOC; // registry full — fail closed

    // Map OUTSIDE the registry lock (VMM walks may allocate table pages;
    // holding the map lock across them risks lock inversion with PMM).
    const errors::VmmError map_err = VMM::map_frame_from_cap_err(
        &fc, va, /*user=*/true, vspace_phys, executable);
    if (map_err != errors::VMM_ERR_OK) {
        // Roll back the claimed slot on a failed map and drop the slot's
        // pin.  Re-validate the slot still holds THIS mapping: a
        // concurrent invalidate_cap (cross-task revoke) may have cleared
        // the slot (and released its pin) while we mapped outside the
        // lock.  Only the path that observes the occupied true->false
        // transition releases the pin — releasing unconditionally is a
        // double-release (refcount corruption -> premature dispose ->
        // use-after-free).
        bool cleared = false;
        {
            SpinLockGuard<sync::SpinLock> guard(s_lock_);
            auto &slot = s_slots_[static_cast<size_t>(idx)];
            if (slot.occupied && slot.fc == &fc && slot.va == va &&
                slot.vspace_phys == vspace_phys) {
                slot.occupied = false;
                cleared = true;
            }
        }
        if (cleared)
            fc.release(); // slot pin only; caller holds its own ref
        return map_err;
    }
    if (!writable) {
        // Read-only pass over the freshly mapped range (the map_page_ro
        // precedent): remap each page RO preserving executability.
        for (size_t page_idx = 0; page_idx < fc.count; ++page_idx) {
            VMM::map_page_ro_in_pml4(va + page_idx * arch::PAGE_SIZE,
                                     fc.phys + page_idx * arch::PAGE_SIZE,
                                     executable, vspace_phys);
        }
    }
    return errors::VMM_ERR_OK;
}

bool ForeignFrameMap::unmap(uint64_t va,
                            uint64_t vspace_phys) noexcept {
    int16_t idx = -1;
    cap::FrameCap *fc = nullptr;
    size_t pages = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t scan_idx = 0; scan_idx < kMaxMaps; ++scan_idx) {
            if (s_slots_[scan_idx].occupied &&
                s_slots_[scan_idx].va == va &&
                s_slots_[scan_idx].vspace_phys == vspace_phys) {
                idx = static_cast<int16_t>(scan_idx);
                fc = s_slots_[scan_idx].fc;
                // Pin is held here — the count read is safe.
                pages = s_slots_[scan_idx].pages;
                break;
            }
        }
    }
    if (idx < 0 || fc == nullptr)
        return false;

    // Unmap every PTE outside the lock, then free the slot.  The slot's
    // own reference keeps @p fc alive across the unmap.  Re-validate at
    // the clear step (concurrent invalidate_cap may have cleared this
    // slot while we unmapped outside the lock).
    for (size_t page_idx = 0; page_idx < pages; ++page_idx) {
        // Issue #366 Q2b: fold-and-ignore — a hole (already unmapped by
        // a concurrent invalidate) is benign; the slot protocol, not the
        // PTE, is authoritative.
        (void)VMM::unmap_frame_from_cap_err(
            fc, va + page_idx * arch::PAGE_SIZE, vspace_phys);
    }
    bool cleared = false;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        auto &slot = s_slots_[static_cast<size_t>(idx)];
        if (slot.occupied && slot.fc == fc && slot.va == va &&
            slot.vspace_phys == vspace_phys) {
            slot.occupied = false;
            cleared = true;
        }
    }
    if (cleared)
        fc->release(); // drop the slot's pin outside the lock
    return true;
}

void ForeignFrameMap::invalidate_cap(cap::FrameCap *fc) {
    // Collect matching slots under the lock, unmap their PTEs outside it,
    // then free the slots and drop each slot's pin.  Pointer-equality
    // only.  At dispose() time no live slot references the cap (each slot
    // holds its own ref, so dispose cannot fire while mapped); at revoke()
    // time it clears live slots and releases their pins.
    for (size_t scan_idx = 0; scan_idx < kMaxMaps; ++scan_idx) {
        uint64_t va = 0;
        uint64_t vspace_phys = 0;
        cap::FrameCap *match = nullptr;
        size_t pages = 0;
        bool hit = false;
        {
            SpinLockGuard<sync::SpinLock> guard(s_lock_);
            if (!s_slots_[scan_idx].occupied || s_slots_[scan_idx].fc != fc)
                continue;
            va = s_slots_[scan_idx].va;
            vspace_phys = s_slots_[scan_idx].vspace_phys;
            match = s_slots_[scan_idx].fc;
            // Pin is held here — the count read is safe.
            pages = s_slots_[scan_idx].pages;
            s_slots_[scan_idx].occupied = false;
            hit = true;
        }
        if (hit) {
            for (size_t page_idx = 0; page_idx < pages; ++page_idx)
                (void)VMM::unmap_frame_from_cap_err(
                    match, va + page_idx * arch::PAGE_SIZE, vspace_phys);
            match->release(); // drop the slot's pin (outside the lock)
        }
    }
}

void ForeignFrameMap::drain_vspace(uint64_t vspace_phys) {
    for (size_t scan_idx = 0; scan_idx < kMaxMaps; ++scan_idx) {
        uint64_t va = 0;
        cap::FrameCap *fc = nullptr;
        size_t pages = 0;
        bool hit = false;
        {
            SpinLockGuard<sync::SpinLock> guard(s_lock_);
            if (!s_slots_[scan_idx].occupied ||
                s_slots_[scan_idx].vspace_phys != vspace_phys)
                continue;
            va = s_slots_[scan_idx].va;
            fc = s_slots_[scan_idx].fc;
            // Pin is held here — the count read is safe.
            pages = s_slots_[scan_idx].pages;
            s_slots_[scan_idx].occupied = false;
            hit = true;
        }
        if (hit) {
            // Issue #366 Q2b: fold-and-ignore — teardown must never fail
            // on holes (task death mid-map); the slot was already marked
            // unoccupied under the lock.
            for (size_t page_idx = 0; page_idx < pages; ++page_idx)
                (void)VMM::unmap_frame_from_cap_err(
                    fc, va + page_idx * arch::PAGE_SIZE, vspace_phys);
            fc->release(); // drop the slot's pin (outside the lock)
        }
    }
}

void ForeignFrameMap::snapshot_reset() {
    // Clear the whole registry and drop each slot's pin.  Pins are
    // released OUTSIDE the lock: the last release may run dispose() ->
    // MemPool::free (and dispose calls invalidate_cap, which takes
    // s_lock_).
    cap::FrameCap *to_release[kMaxMaps];
    size_t total = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t scan_idx = 0; scan_idx < kMaxMaps; ++scan_idx) {
            if (s_slots_[scan_idx].occupied)
                to_release[total++] = s_slots_[scan_idx].fc;
            s_slots_[scan_idx].vspace_phys = 0;
            s_slots_[scan_idx].va = 0;
            s_slots_[scan_idx].pages = 0;
            s_slots_[scan_idx].fc = nullptr;
            s_slots_[scan_idx].occupied = false;
        }
    }
    for (size_t pin_idx = 0; pin_idx < total; ++pin_idx) {
#if defined(CONFIG_DEBUG)
        // Issue #320 detector: a live slot holds exactly one pin, so a
        // well-formed release observes refcount >= 2 (this pin + the
        // CSpace slot's reference).  refcount <= 1 means the pin was
        // already dropped by the MemPool baseline rewind — releasing here
        // would dispose a cap the restored CSpace still references.
        // Log-only, and s_lock_ is NOT held on this path.
        const uint32_t rc_before = to_release[pin_idx]->refcount();
        if (rc_before <= 1U) {
            debug_write("[#320] ForeignFrameMap pin over-release "
                        "(refcount=");
            debug_write_hex(rc_before);
            debug_write(") fc=0x");
            debug_write_hex(
                reinterpret_cast<uint64_t>(to_release[pin_idx]));
            debug_write("\n");
        }
#endif
        to_release[pin_idx]->release();
    }
}

size_t ForeignFrameMap::live_count() {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    size_t total = 0;
    for (size_t scan_idx = 0; scan_idx < kMaxMaps; ++scan_idx) {
        if (s_slots_[scan_idx].occupied)
            ++total;
    }
    return total;
}

bool ForeignFrameMap::is_mapped(uint64_t va,
                                uint64_t vspace_phys) noexcept {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    for (size_t scan_idx = 0; scan_idx < kMaxMaps; ++scan_idx) {
        if (s_slots_[scan_idx].occupied &&
            s_slots_[scan_idx].va == va &&
            s_slots_[scan_idx].vspace_phys == vspace_phys)
            return true;
    }
    return false;
}

} // namespace kernel::cap
