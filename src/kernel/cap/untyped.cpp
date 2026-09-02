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

/// @file untyped.cpp
/// @brief Untyped memory object implementation (ROADMAP 0.4.1 item 3).

#include <kernel/cap/untyped.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/nexios_config.h>
#include <kernel/arch/page_table.hpp>

// Placement new (defined in lib/new.cpp, no <new> header in freestanding)
inline void *operator new(unsigned long, void *p) noexcept {
    return p;
}

namespace kernel::cap {

/// @brief Live Untyped object count, bounded by CONFIG_CAP_MAX_UNTYPED.
///        TU-local (post global-state-refactor convention).
static uint32_t g_live_untypeds = 0;

namespace {

/// @brief MemPool-backed construction shared by create/create_subrange.
///        Assumes @p phys/@p size already validated and, for create(), the
///        region already carved from PMM.  Enforces the shared
///        CONFIG_CAP_MAX_UNTYPED live bound and cap-object tracking.
UntypedMem *alloc_object(uint64_t phys, size_t size, bool is_user,
                         CapType target) noexcept {
    if (__atomic_load_n(&g_live_untypeds, __ATOMIC_RELAXED) >=
        static_cast<uint32_t>(CONFIG_CAP_MAX_UNTYPED))
        return nullptr;
    auto *ut = static_cast<UntypedMem *>(MemPool::alloc(sizeof(UntypedMem)));
    if (!ut)
        return nullptr;
    new (ut) UntypedMem;
    ut->mark_pool_backed();
    ut->phys = phys;
    ut->size = size;
    ut->is_user = is_user;
    ut->retype_target = target;
    __atomic_fetch_add(&g_live_untypeds, 1U, __ATOMIC_RELAXED);
    kernel::test::ResourceTracker::instance().track_cap_object_add();
    return ut;
}

} // namespace

UntypedMem *UntypedMem::create(size_t size, bool is_user, CapType target) {
    if (size == 0 || (size % arch::PAGE_SIZE) != 0)
        return nullptr;
    if (target != CapType::Frame)
        return nullptr; // Frame is the only supported target
    // Check the live bound BEFORE carving PMM (avoids allocate-then-free on
    // exhaustion).  alloc_object re-checks it as the shared enforcement for
    // create_subrange.
    if (__atomic_load_n(&g_live_untypeds, __ATOMIC_RELAXED) >=
        static_cast<uint32_t>(CONFIG_CAP_MAX_UNTYPED))
        return nullptr;

    size_t pages = size / arch::PAGE_SIZE;
    uint64_t phys = is_user ? PMM::alloc_user_contiguous(pages)
                            : PMM::alloc_contiguous(pages);
    if (!phys)
        return nullptr;

    UntypedMem *ut = alloc_object(phys, size, is_user, target);
    if (!ut) {
        for (size_t i = 0; i < pages; ++i)
            PMM::free_page(phys + i * arch::PAGE_SIZE);
        return nullptr;
    }
    return ut;
}

UntypedMem *UntypedMem::create_subrange(uint64_t phys, size_t size,
                                        bool is_user, CapType target) {
    if (phys == 0 || size == 0 || (size % arch::PAGE_SIZE) != 0)
        return nullptr;
    if (target != CapType::Frame)
        return nullptr;
    // No PMM allocation: the frames are already owned by the parent Untyped;
    // ownership transfers when the parent's retype guard is claimed.
    return alloc_object(phys, size, is_user, target);
}

bool UntypedMem::claim_once() noexcept {
    bool expected = false;
    return __atomic_compare_exchange_n(&retyped_, &expected, true, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

bool UntypedMem::owns_region() const noexcept {
    return !__atomic_load_n(&retyped_, __ATOMIC_ACQUIRE);
}

void UntypedMem::dispose() noexcept {
    // Free the region ONLY if this Untyped still owns it.  After a successful
    // retype the retyped capability (FrameCap) owns and frees the frames.
    if (owns_region() && phys != 0) {
        size_t pages = size / arch::PAGE_SIZE;
        for (size_t i = 0; i < pages; ++i)
            PMM::free_page(phys + i * arch::PAGE_SIZE);
    }
    if (__atomic_load_n(&g_live_untypeds, __ATOMIC_RELAXED) > 0)
        __atomic_fetch_sub(&g_live_untypeds, 1U, __ATOMIC_RELAXED);
    kernel::test::ResourceTracker::instance().track_cap_object_remove();
    MemPool::free(this);
}

int retype(CNode *cspace, uint64_t untyped_handle, CapType target_type,
           size_t size, uint32_t rights) noexcept {
    if (!cspace)
        return -1;
    // Pin the Untyped (WRITE = mutation right).  The pin keeps the object
    // alive across the transfer so dispose() cannot run mid-retype.
    KernelObject *obj = lookup(cspace, untyped_handle, CapType::Untyped,
                               CAP_RIGHT_WRITE);
    if (!obj)
        return -1;
    auto *ut = static_cast<UntypedMem *>(obj);

    // Validation must not consume the Untyped (retype can be retried).
    if (target_type != ut->retype_target || size == 0 ||
        (size % arch::PAGE_SIZE) != 0 || size > ut->size) {
        ut->release();
        return -1;
    }
    const bool exact = (size == ut->size);

    // Non-destructive slot-capacity pre-check: a sub-range carve installs two
    // slots (target + child Untyped).  Failing here leaves the Untyped intact.
    const size_t need_slots = exact ? 1U : 2U;
    if (occupied_count(cspace) + need_slots >
        static_cast<size_t>(CONFIG_CSLOT_COUNT)) {
        ut->release();
        return -1;
    }

    // Claim the single transfer.  Loser -> -1.
    if (!ut->claim_once()) {
        ut->release();
        return -1;
    }

    // Build the target FrameCap over [ut->phys, ut->phys + size).  On MemPool
    // exhaustion roll the claim back (no object over the region exists yet)
    // and keep the whole region with the Untyped.
    FrameCap *fc = FrameCap::create(ut->phys, size / arch::PAGE_SIZE,
                                    ut->is_user);
    if (!fc) {
        ut->reset_claim();
        ut->release();
        return -1;
    }

    // Sub-range carve: build the child Untyped for the remainder
    // [ut->phys + size, ut->phys + ut->size).  On failure (MemPool or the
    // CONFIG_CAP_MAX_UNTYPED live bound) fail closed: stretch the never-
    // installed FrameCap to the whole region and release it — every frame
    // returns to PMM exactly once and the parent stays spent (guard set).
    UntypedMem *child = nullptr;
    if (!exact) {
        child = UntypedMem::create_subrange(ut->phys + size, ut->size - size,
                                            ut->is_user, ut->retype_target);
        if (!child) {
            fc->count = ut->size / arch::PAGE_SIZE;
            fc->release();
            ut->release();
            return -1;
        }
    }

    int idx_target = cspace->install(fc, CapType::Frame, rights);
    if (idx_target < 0) {
        if (child)
            child->release(); // disposes [ut->phys + size, ut->phys + ut->size)
        fc->release();        // disposes [ut->phys, ut->phys + size)
        ut->release();
        return -1;
    }

    if (child) {
        // Child must remain retypable: retype() requires CAP_RIGHT_WRITE on
        // the Untyped slot; the caller already holds WRITE over the parent
        // region (the parent lookup demanded it), so this grants no new
        // authority.
        int idx_child = cspace->install(child, CapType::Untyped,
                                        rights | CAP_RIGHT_WRITE);
        if (idx_child < 0) {
            // Roll back the target install, then release the child: the two
            // disposes cover the whole region exactly once.
            cspace->remove(static_cast<uint32_t>(idx_target));
            fc->release(); // drop the creator ref -> dispose frees [ut->phys, ut->phys + size)
            child->release();
            ut->release();
            return -1;
        }
    }

    // Success: both slots hold a reference; drop the creator refs.
    fc->release();
    if (child)
        child->release();
    ut->release();
    return idx_target;
}

} // namespace kernel::cap
