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
#include <kernel/cap/cap_errors.hpp>
#include <kernel/cap/carved_object.hpp>
#include <kernel/cap/derivation.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/cap/object_sizing.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/sync/spinlock_guard.hpp>
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
UntypedMem *alloc_object(uint64_t phys, size_t size,
                         bool is_user) noexcept {
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
    __atomic_fetch_add(&g_live_untypeds, 1U, __ATOMIC_RELAXED);
    kernel::test::ResourceTracker::instance().track_cap_object_add();
    return ut;
}

/// @brief Builds the retype target object over the carved prefix
///        [ut->phys, ut->phys + carve_bytes).  Frame takes an explicit
///        size; the five fixed kinds are constructed at their
///        compile-time size (set by construction — no widening setter).
KernelObject *create_target(const UntypedMem *ut, CapType target_type,
                            size_t carve_bytes) noexcept {
    if (target_type == CapType::Frame) {
        return FrameCap::create(ut->phys, carve_bytes / arch::PAGE_SIZE,
                                ut->is_user);
    }
    return CarvedObjectCap::create(target_type, ut->phys, carve_bytes,
                                   ut->is_user);
}

} // namespace

UntypedMem *UntypedMem::create(size_t size, bool is_user) {
    if (size == 0 || (size % arch::PAGE_SIZE) != 0)
        return nullptr;
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

    UntypedMem *ut = alloc_object(phys, size, is_user);
    if (!ut) {
        for (size_t i = 0; i < pages; ++i)
            PMM::free_page(phys + i * arch::PAGE_SIZE);
        return nullptr;
    }
    return ut;
}

UntypedMem *UntypedMem::create_subrange(uint64_t phys, size_t size,
                                        bool is_user) {
    if (phys == 0 || size == 0 || (size % arch::PAGE_SIZE) != 0)
        return nullptr;
    // No PMM allocation: the frames are already owned by the parent Untyped;
    // ownership transfers when the parent's retype guard is claimed.
    return alloc_object(phys, size, is_user);
}

uint32_t UntypedMem::live_count() noexcept {
    return __atomic_load_n(&g_live_untypeds, __ATOMIC_RELAXED);
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
           size_t size, CapRights rights, uint32_t *gen_out,
           errors::CapError *err_out) noexcept {
    if (!cspace) {
        if (err_out != nullptr)
            *err_out = errors::CAP_ERR_INVALID_ARGS;
        return -1;
    }
    // Issue #240 step-a: cached policy-word check (one short locked load,
    // zero new lock order); all-allow at step-a so always passes.
    {
        SpinLockGuard<sync::SpinLock> guard(cspace->lock_);
        if (!CNode::policy_allows(cspace->policy_word_,
                                   kCapBrokerAllowRetype)) {
            if (err_out != nullptr)
                *err_out = errors::CAP_ERR_POLICY_DENIED;
            return -1;
        }
    }
    // Pin the Untyped (WRITE = mutation right).  The pin keeps the object
    // alive across the transfer so dispose() cannot run mid-retype.
    KernelObject *obj = lookup(cspace, untyped_handle, CapType::Untyped,
                               CapRights::WRITE);
    if (!obj) {
        if (err_out != nullptr)
            *err_out = errors::CAP_ERR_BAD_HANDLE;
        return -1;
    }
    auto *ut = static_cast<UntypedMem *>(obj);

    // Per-invocation target (F1 #554): only the six carvable kinds pass.
    // Endpoint/CNode/Mmio/Irq/IoMmuDma/Msix/Wdog/Untyped/Task/Null are
    // MemPool-resident or authority-only and stay rejected here.
    const ObjectSizing sizing = object_sizing(target_type);
    if (!sizing.carvable) {
        if (err_out != nullptr)
            *err_out = errors::CAP_ERR_TARGET_UNSUPPORTED;
        ut->release();
        return -1;
    }
    // Effective carve size: Frame takes the explicit size; a fixed kind
    // with size 0 means "the type's size" (F1 UX note in cap.hpp).
    size_t carve_bytes = size;
    if (!sizing.variable_size && size == 0)
        carve_bytes = sizing.bytes;
    // Validation must not consume the Untyped (retype can be retried).
    if (sizing.variable_size && size == 0) {
        if (err_out != nullptr)
            *err_out = errors::CAP_ERR_SIZE_ZERO;
        ut->release();
        return -1;
    }
    if ((carve_bytes % arch::PAGE_SIZE) != 0) {
        if (err_out != nullptr)
            *err_out = errors::CAP_ERR_SIZE_UNALIGNED;
        ut->release();
        return -1;
    }
    if (carve_bytes > ut->size) {
        if (err_out != nullptr) {
            *err_out = sizing.variable_size ? errors::CAP_ERR_RANGE_EXCEEDED
                                            : errors::CAP_ERR_SHORT_RANGE;
        }
        ut->release();
        return -1;
    }
    if (!sizing.variable_size && size != 0 && size != sizing.bytes) {
        if (err_out != nullptr)
            *err_out = errors::CAP_ERR_SIZE_MISMATCH;
        ut->release();
        return -1;
    }
    const bool exact = (carve_bytes == ut->size);

    // Non-destructive slot-capacity pre-check: a sub-range carve installs two
    // slots (target + child Untyped).  Failing here leaves the Untyped intact.
    const size_t need_slots = exact ? 1U : 2U;
    if (occupied_count(cspace) + need_slots >
        static_cast<size_t>(CONFIG_CSLOT_COUNT)) {
        if (err_out != nullptr)
            *err_out = errors::CAP_ERR_NO_CSLOT;
        ut->release();
        return -1;
    }

    // Claim the single transfer.  Loser -> -1.
    if (!ut->claim_once()) {
        if (err_out != nullptr)
            *err_out = errors::CAP_ERR_ALREADY_RETYPED;
        ut->release();
        return -1;
    }

    // Build the target over [ut->phys, ut->phys + carve_bytes).  On MemPool
    // exhaustion roll the claim back (no object over the region exists yet)
    // and keep the whole region with the Untyped.
    KernelObject *target = create_target(ut, target_type, carve_bytes);
    if (!target) {
        if (err_out != nullptr)
            *err_out = errors::CAP_ERR_OBJECT_EXHAUSTED;
        ut->reset_claim();
        ut->release();
        return -1;
    }

    // Sub-range carve: build the child Untyped for the remainder
    // [ut->phys + carve_bytes, ut->phys + ut->size).  On failure (MemPool or
    // the CONFIG_CAP_MAX_UNTYPED live bound) fail closed: the whole region
    // returns to PMM exactly once and the parent stays spent (guard set).
    UntypedMem *child = nullptr;
    if (!exact) {
        child = UntypedMem::create_subrange(ut->phys + carve_bytes,
                                            ut->size - carve_bytes,
                                            ut->is_user);
        if (!child) {
            if (err_out != nullptr)
                *err_out = errors::CAP_ERR_CHILD_UNAVAILABLE;
            if (target_type == CapType::Frame) {
                auto *frame = static_cast<FrameCap *>(target);
                frame->count = ut->size / arch::PAGE_SIZE;
                frame->release();
            } else {
                // Carved kinds have no widening setter (S3-4 construction
                // preference): release the prefix target (frees the carved
                // prefix) then free the remainder directly.  Bounded loop
                // (INV-RT-4); every page is freed exactly once.
                target->release();
                const size_t first_page = carve_bytes / arch::PAGE_SIZE;
                const size_t total_pages = ut->size / arch::PAGE_SIZE;
                for (size_t page_idx = first_page; page_idx < total_pages;
                     ++page_idx)
                    PMM::free_page(ut->phys + page_idx * arch::PAGE_SIZE);
            }
            ut->release();
            return -1;
        }
    }

    int idx_target = cspace->install(target, target_type, rights, gen_out);
    if (idx_target < 0) {
        if (err_out != nullptr)
            *err_out = errors::CAP_ERR_INSTALL_FAILED;
        if (child)
            child->release(); // disposes [ut->phys + carve, remainder)
        target->release();    // disposes [ut->phys, ut->phys + carve)
        ut->release();
        return -1;
    }

    // F2 (#555): hoisted to function scope so the derivation-record
    // rollback below can remove the child slot (the verbatim reuse left a
    // live child Untyped on that path).
    int idx_child = -1;
    if (child) {
        // Child must remain retypable: retype() requires CapRights::WRITE on
        // the Untyped slot; the caller already holds WRITE over the parent
        // region (the parent lookup demanded it), so this grants no new
        // authority.
        idx_child = cspace->install(child, CapType::Untyped,
                                    rights | CapRights::WRITE);
        if (idx_child < 0) {
            if (err_out != nullptr)
                *err_out = errors::CAP_ERR_INSTALL_FAILED;
            // Roll back the target install, then release the child: the two
            // disposes cover the whole region exactly once.
            cspace->remove(static_cast<uint32_t>(idx_target));
            target->release(); // drop creator ref -> dispose frees prefix
            child->release();
            ut->release();
            return -1;
        }
    }

    // F2 (#555): record both derivations (carved target + child remainder)
    // as self edges.  The depth is generic (child_depth(parent) + 1), so
    // carve-chains nest correctly (INV-DERIV-3).  Either record failure
    // removes BOTH slots before the creator releases: each region is then
    // disposed exactly once (prefix via target, remainder via child) and no
    // live child Untyped survives — matching the no-partial-grant
    // precedent above.  cspace->remove's forget_slot prunes an
    // already-recorded edge, so the table keeps no dangling entry.
    {
        const uint32_t parent_slot = handle_slot(untyped_handle);
        const uint32_t parent_gen = handle_gen(untyped_handle);
        const uint32_t parent_depth =
            static_cast<uint32_t>(DerivationTable::instance().child_depth(
                *cspace, parent_slot, parent_gen));
        if (parent_depth + 1 > static_cast<uint32_t>(CONFIG_CAP_MAX_DEPTH)) {
            cspace->remove(static_cast<uint32_t>(idx_target));
            if (child)
                cspace->remove(static_cast<uint32_t>(idx_child));
            target->release();
            if (child)
                child->release();
            ut->release();
            if (err_out != nullptr)
                *err_out = errors::CAP_ERR_DEPTH_EXCEEDED;
            return -1;
        }
        const uint8_t depth = static_cast<uint8_t>(parent_depth + 1);
        const uint32_t target_gen =
            cspace->slot_gen(static_cast<uint32_t>(idx_target));
        bool recorded = DerivationTable::instance().record(
            *cspace, parent_slot, parent_gen, *cspace, kDerivSelfNode, 0,
            static_cast<uint32_t>(idx_target), target_gen, depth);
        if (recorded && child) {
            const uint32_t child_slot_gen =
                cspace->slot_gen(static_cast<uint32_t>(idx_child));
            recorded = DerivationTable::instance().record(
                *cspace, parent_slot, parent_gen, *cspace, kDerivSelfNode, 0,
                static_cast<uint32_t>(idx_child), child_slot_gen, depth);
        }
        if (!recorded) {
            cspace->remove(static_cast<uint32_t>(idx_target));
            if (child)
                cspace->remove(static_cast<uint32_t>(idx_child));
            target->release();
            if (child)
                child->release();
            ut->release();
            if (err_out != nullptr)
                *err_out = errors::CAP_ERR_DERIVATION_FULL;
            return -1;
        }
    }

    // Success: both slots hold a reference; drop the creator refs.
    // err_out is untouched on success (INV-RT-5).
    target->release();
    if (child)
        child->release();
    ut->release();
    return idx_target;
}

} // namespace kernel::cap
