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

/// @file cap.cpp
/// @brief CSpace core engine implementation: CNode lifecycle, slot install/
///        remove, handle lookup, cascade revoke.

#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/test/resource_tracker.hpp>

// Placement new (defined in lib/new.cpp, no <new> header in freestanding)
inline void *operator new(unsigned long, void *p) noexcept {
    return p;
}

namespace kernel::cap {

CNode *CNode::create(uint32_t cspace_id) {
    auto *node = static_cast<CNode *>(MemPool::alloc(sizeof(CNode)));
    if (!node)
        return nullptr;
    new (node) CNode;
    node->mark_pool_backed();
    node->cspace_id = cspace_id;
    kernel::test::ResourceTracker::instance().track_cap_object_add();
    return node;
}

void CNode::dispose() noexcept {
    // Drop every slot reference.  Runs outside any cap spinlock: it may
    // MemPool::free() targets whose last slot reference this is.
    for (uint32_t i = 0; i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT); ++i) {
        KernelObject *target = slots[i].obj;
        if (slots[i].occupied && target) {
            slots[i].occupied = false;
            slots[i].obj = nullptr;
            kernel::test::ResourceTracker::instance().track_cap_slot_remove();
            target->release();
        }
    }
    if (is_pool_backed()) {
        kernel::test::ResourceTracker::instance().track_cap_object_remove();
        MemPool::free(this);
    }
}

void CNode::revoke() noexcept {
    KernelObject::revoke();
    // Iterative cascade with an explicit work list (never unbounded
    // recursion).  Bound the walk by CONFIG_CAP_MAX_DEPTH and the slot
    // count.  Children are marked revoked; their refcounts are dropped by
    // the caller of revoke() (remove()) or by dispose() at teardown.
    CNode *todo[CONFIG_CAP_MAX_DEPTH];
    size_t todo_count = 0;
    todo[todo_count++] = this;
    while (todo_count > 0) {
        CNode *node = todo[--todo_count];
        for (uint32_t i = 0; i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT); ++i) {
            CSlot &slot = node->slots[i];
            if (!slot.occupied || !slot.obj)
                continue;
            slot.obj->revoke();
            if (slot.type == CapType::CNode &&
                node->depth_ < CONFIG_CAP_MAX_DEPTH &&
                todo_count < CONFIG_CAP_MAX_DEPTH) {
                auto *child = static_cast<CNode *>(slot.obj);
                child->depth_ = node->depth_ + 1;
                todo[todo_count++] = child;
            }
        }
    }
}

int CNode::install(KernelObject *obj, CapType type, uint32_t rights) noexcept {
    if (!obj || type == CapType::Null)
        return -1;
    if (!obj->acquire())
        return -1; // revoked target: refuse to install
    SpinLockGuard<sync::SpinLock> guard(lock_);
    for (uint32_t i = 0; i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT); ++i) {
        if (!slots[i].occupied) {
            slots[i].obj = obj;
            slots[i].type = type;
            slots[i].rights = rights;
            slots[i].occupied = true;
            kernel::test::ResourceTracker::instance().track_cap_slot_add();
            return static_cast<int>(i);
        }
    }
    // Table full: roll back the acquire taken above.
    obj->release();
    return -1;
}

void CNode::remove(uint32_t idx) noexcept {
    KernelObject *target = nullptr;
    {
        SpinLockGuard<sync::SpinLock> guard(lock_);
        if (idx >= static_cast<uint32_t>(CONFIG_CSLOT_COUNT) ||
            !slots[idx].occupied)
            return;
        target = slots[idx].obj;
        slots[idx].obj = nullptr;
        slots[idx].type = CapType::Null;
        slots[idx].rights = 0;
        ++slots[idx].gen; // invalidate stale handles for a recycled slot
        slots[idx].occupied = false;
        kernel::test::ResourceTracker::instance().track_cap_slot_remove();
    }
    // Release the slot's reference OUTSIDE the lock: the last release may
    // run dispose() -> MemPool::free, which must never run under a cap
    // spinlock (lock ordering).
    if (target)
        target->release();
}

KernelObject *CNode::peek(uint32_t idx, CapType want) const noexcept {
    if (idx >= static_cast<uint32_t>(CONFIG_CSLOT_COUNT))
        return nullptr;
    const CSlot &slot = slots[idx];
    if (!slot.occupied || slot.type != want)
        return nullptr;
    return slot.obj;
}

uint32_t CNode::slot_gen(uint32_t idx) const noexcept {
    if (idx >= static_cast<uint32_t>(CONFIG_CSLOT_COUNT))
        return 0;
    return slots[idx].gen;
}

void CNode::clear_grant(uint32_t idx) noexcept {
    SpinLockGuard<sync::SpinLock> guard(lock_);
    if (idx >= static_cast<uint32_t>(CONFIG_CSLOT_COUNT) || !slots[idx].occupied)
        return;
    slots[idx].rights &= ~CAP_RIGHT_GRANT;
}

KernelObject *lookup(CNode *cspace, uint64_t handle, CapType want,
                     uint32_t need_rights) noexcept {
    if (!cspace)
        return nullptr;
    const uint32_t idx = handle_slot(handle);
    if (!slot_index_valid(idx))
        return nullptr;
    KernelObject *target = nullptr;
    {
        SpinLockGuard<sync::SpinLock> guard(cspace->lock_);
        if (handle_cspace(handle) != cspace->cspace_id)
            return nullptr;
        const CSlot &slot = cspace->slots[idx];
        if (!slot.occupied)
            return nullptr;
        if (handle_gen(handle) != slot.gen)
            return nullptr; // stale handle
        if (want != CapType::Null && slot.type != want)
            return nullptr;
        if ((slot.rights & need_rights) != need_rights)
            return nullptr;
        if (!slot.obj->acquire())
            return nullptr; // revoked meanwhile
        target = slot.obj;
    }
    return target;
}

bool revoke(CNode *cspace, uint64_t handle) noexcept {
    if (!cspace)
        return false;
    const uint32_t idx = handle_slot(handle);
    if (!slot_index_valid(idx))
        return false;
    if (handle_cspace(handle) != cspace->cspace_id)
        return false;
    // Occupy-claim the slot under the lock; collect the target for deferred
    // release (the last release may MemPool::free the target).
    KernelObject *target = nullptr;
    bool found = false;
    {
        SpinLockGuard<sync::SpinLock> guard(cspace->lock_);
        CSlot &slot = cspace->slots[idx];
        if (!slot.occupied)
            return false; // idempotent: already revoked/free
        if (handle_gen(handle) != slot.gen)
            return false;
        target = slot.obj;
        slot.obj = nullptr;
        slot.type = CapType::Null;
        slot.rights = 0;
        ++slot.gen;
        slot.occupied = false;
        kernel::test::ResourceTracker::instance().track_cap_slot_remove();
        found = true;
    }
    if (!found)
        return false;
    // Outside the lock: mark the target revoked (future acquire() refused),
    // cascading if it is a CNode, then drop the slot reference.
    if (target)
        target->revoke();
    if (target)
        target->release();
    return true;
}

size_t occupied_count(const CNode *cspace) noexcept {
    if (!cspace)
        return 0;
    size_t n = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT); ++i)
        if (cspace->slots[i].occupied)
            ++n;
    return n;
}

/// @brief Core of copy/grant: pins the source slot target, then installs it
///        into @p dst with @p rights.  The install takes its own acquire();
///        the pin taken here is released on both success and failure.
int do_copy_pinned(CNode *src, uint64_t src_handle, CNode *dst,
                   uint32_t rights) noexcept {
    if (!src || !dst || src == dst)
        return -1;
    // lookup() returns the target with acquire() already taken.
    KernelObject *target =
        lookup(src, src_handle, CapType::Null, 0);
    if (!target)
        return -1;
    CapType type = CapType::Null;
    uint32_t src_rights = 0;
    uint32_t src_gen = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(src->lock_);
        const uint32_t idx = handle_slot(src_handle);
        if (idx >= static_cast<uint32_t>(CONFIG_CSLOT_COUNT) ||
            !src->slots[idx].occupied) {
            target->release();
            return -1;
        }
        type = src->slots[idx].type;
        src_rights = src->slots[idx].rights;
        src_gen = src->slots[idx].gen;
    }
    if (src_gen != handle_gen(src_handle)) {
        target->release();
        return -1;
    }
    // Reduce the granted rights by the requested mask AND the source rights
    // (a copy can never widen rights).
    const uint32_t effective = rights & src_rights;
    int installed = dst->install(target, type, effective);
    target->release();
    return installed;
}

int copy(CNode *src, uint64_t src_handle, CNode *dst) noexcept {
    return do_copy_pinned(src, src_handle, dst, CAP_RIGHT_READ |
                                                      CAP_RIGHT_WRITE |
                                                      CAP_RIGHT_COPY |
                                                      CAP_RIGHT_GRANT);
}

int grant(CNode *src, uint64_t src_handle, CNode *dst) noexcept {
    // Requires CAP_RIGHT_GRANT on the source slot.
    KernelObject *target = lookup(src, src_handle, CapType::Null,
                                  CAP_RIGHT_GRANT);
    if (!target)
        return -1;
    // Capture the type/rights under the lock, then clear GRANT (mint-once).
    CapType type = CapType::Null;
    uint32_t rights = 0;
    uint32_t src_gen = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(src->lock_);
        const uint32_t idx = handle_slot(src_handle);
        if (idx >= static_cast<uint32_t>(CONFIG_CSLOT_COUNT) ||
            !src->slots[idx].occupied) {
            target->release();
            return -1;
        }
        type = src->slots[idx].type;
        rights = src->slots[idx].rights;
        src_gen = src->slots[idx].gen;
    }
    if (src_gen != handle_gen(src_handle)) {
        target->release();
        return -1;
    }
    int installed = dst->install(target, type, rights);
    if (installed >= 0)
        src->clear_grant(handle_slot(src_handle));
    target->release();
    return installed;
}

int mint(CNode *src, uint64_t src_handle, CNode *dst, uint32_t rights_mask,
         uint32_t badge) noexcept {
    (void)badge; // badge re-branding lands with endpoint integration (Phase 4)
    return do_copy_pinned(src, src_handle, dst, rights_mask);
}

} // namespace kernel::cap
