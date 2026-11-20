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
#include <kernel/cap/derivation.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/test/resource_tracker.hpp>

// Placement new (defined in lib/new.cpp, no <new> header in freestanding)
inline void *operator new(unsigned long, void *placement) noexcept {
    return placement;
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
    DerivationTable::instance().forget_node(*this);
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
        for (uint32_t i = 0; i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT);
             ++i) {
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

int CNode::install(KernelObject *obj, CapType type, CapRights rights,
                    uint32_t *gen_out) noexcept {
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
            if (gen_out != nullptr)
                *gen_out = slots[i].gen;
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
        slots[idx].rights = CapRights::NONE;
        ++slots[idx].gen; // invalidate stale handles for a recycled slot
        slots[idx].occupied = false;
        kernel::test::ResourceTracker::instance().track_cap_slot_remove();
    }
    // INV-DERIV-9 corollary: only already-consumed edges can match this
    // prune during a post-order revoke walk; outside a walk it closes the
    // table entry for a deleted slot (leak-closer for delete/dispose).
    DerivationTable::instance().forget_slot(*this, idx);
    // Release the slot's reference OUTSIDE the lock: the last release may
    // run dispose() -> MemPool::free, which must never run under a cap
    // spinlock (lock ordering).
    if (target)
        target->release();
}

bool CNode::remove_if(uint32_t idx, uint32_t gen) noexcept {
    KernelObject *target = nullptr;
    {
        SpinLockGuard<sync::SpinLock> guard(lock_);
        if (idx >= static_cast<uint32_t>(CONFIG_CSLOT_COUNT) ||
            !slots[idx].occupied || slots[idx].gen != gen)
            return false;
        target = slots[idx].obj;
        slots[idx].obj = nullptr;
        slots[idx].type = CapType::Null;
        slots[idx].rights = CapRights::NONE;
        ++slots[idx].gen; // invalidate stale handles for a recycled slot
        slots[idx].occupied = false;
        kernel::test::ResourceTracker::instance().track_cap_slot_remove();
    }
    // Same INV-DERIV-9 prune placement as remove().
    DerivationTable::instance().forget_slot(*this, idx);
    // Release OUTSIDE the lock (dispose() lock ordering, as in remove()).
    if (target)
        target->release();
    return true;
}

uint32_t CNode::serial() noexcept {
    return DerivationTable::instance().node_serial(*this);
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

uint32_t CNode::free_slot_count() const noexcept {
    if (revoked()) {
        return 0;
    }
    SpinLockGuard<sync::SpinLock> guard(lock_);
    uint32_t free = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT);
         ++i) {
        if (!slots[i].occupied) {
            ++free;
        }
    }
    return free;
}

void CNode::clear_grant(uint32_t idx) noexcept {
    SpinLockGuard<sync::SpinLock> guard(lock_);
    if (idx >= static_cast<uint32_t>(CONFIG_CSLOT_COUNT) ||
        !slots[idx].occupied)
        return;
    slots[idx].rights &= ~CapRights::GRANT;
}

void CNode::set_policy_word(uint32_t word) noexcept {
    SpinLockGuard<sync::SpinLock> guard(lock_);
    policy_word_ = word;
    ++policy_gen_; // atomic with the store under the same lock (S3 note)
}

bool cnode_holds_type(CNode *cs, CapType want) noexcept {
    if (cs == nullptr) {
        return false;
    }
    SpinLockGuard<sync::SpinLock> guard(cs->lock_);
    for (uint32_t idx = 0;
         idx < static_cast<uint32_t>(CONFIG_CSLOT_COUNT); ++idx) {
        if (cs->peek(idx, want) != nullptr) {
            return true;
        }
    }
    return false;
}

KernelObject *lookup(CNode *cspace, uint64_t handle, CapType want,
                     CapRights need_rights) noexcept {
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
        slot.rights = CapRights::NONE;
        ++slot.gen;
        slot.occupied = false;
        kernel::test::ResourceTracker::instance().track_cap_slot_remove();
        found = true;
    }
    if (!found)
        return false;
    // INV-DERIV-9 corollary (see CNode::remove): prune placement is
    // behaviour-neutral during a post-order walk and closes the entry for
    // teardown-driven revokes.  Behaviour below is otherwise byte-identical
    // (object-wide cascade) for the ramdiskd + test call sites.
    DerivationTable::instance().forget_slot(*cspace, idx);
    // Outside the lock: mark the target revoked (future acquire() refused),
    // cascading if it is a CNode, then drop the slot reference.
    if (target)
        target->revoke();
    if (target)
        target->release();
    return true;
}

bool remove(CNode *cspace, uint64_t handle) noexcept {
    if (!cspace)
        return false;
    const uint32_t idx = handle_slot(handle);
    if (!slot_index_valid(idx))
        return false;
    if (handle_cspace(handle) != cspace->cspace_id)
        return false;
    // Occupy-claim the slot under the lock (same three fail-closed checks
    // as revoke()); collect the target for deferred release.
    KernelObject *target = nullptr;
    {
        SpinLockGuard<sync::SpinLock> guard(cspace->lock_);
        CSlot &slot = cspace->slots[idx];
        if (!slot.occupied)
            return false; // idempotent: already deleted
        if (handle_gen(handle) != slot.gen)
            return false;
        target = slot.obj;
        slot.obj = nullptr;
        slot.type = CapType::Null;
        slot.rights = CapRights::NONE;
        ++slot.gen; // invalidate stale handles for a recycled slot
        slot.occupied = false;
        kernel::test::ResourceTracker::instance().track_cap_slot_remove();
    }
    // INV-DERIV-9 corollary (see CNode::remove): prune placement.
    DerivationTable::instance().forget_slot(*cspace, idx);
    // N6 deletes only the caller's slot: no target->revoke() (INV-DERIV-5)
    // — other holders' lookups keep succeeding.  The last release destroys
    // the target via dispose().
    if (target)
        target->release();
    return true;
}

int revoke_derivations(CNode *cspace, uint64_t handle,
                       errors::CapError *err_out) noexcept {
    if (cspace == nullptr) {
        if (err_out != nullptr)
            *err_out = errors::CAP_ERR_BAD_HANDLE;
        return -1;
    }
    const uint32_t named_slot = handle_slot(handle);
    if (!slot_index_valid(named_slot) ||
        handle_cspace(handle) != cspace->cspace_id) {
        if (err_out != nullptr)
            *err_out = errors::CAP_ERR_BAD_HANDLE;
        return -1;
    }
    // Validate + pin under cspace->lock_ (mirrors lookup(); INV-DERIV-10):
    // the walk then uses only the captured (serial, slot, gen) triple.
    KernelObject *named_target = nullptr;
    uint32_t named_gen = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(cspace->lock_);
        CSlot &named = cspace->slots[named_slot];
        if (!named.occupied || handle_gen(handle) != named.gen ||
            !named.obj->acquire()) {
            if (err_out != nullptr)
                *err_out = errors::CAP_ERR_BAD_HANDLE;
            return -1;
        }
        named_target = named.obj;
        named_gen = named.gen;
    }
    // Pin the cspace itself so the walk is independent of the caller's
    // argument (INV-DERIV-2: no raw CNode* is stored, pins are released on
    // every exit path through the single epilogue below).
    if (!cspace->acquire()) {
        named_target->release();
        if (err_out != nullptr)
            *err_out = errors::CAP_ERR_BAD_HANDLE;
        return -1;
    }
    uint32_t budget = kRevokeEdgeBudget;
    uint32_t destroyed = 0;
    struct WorkItem {
        CNode *node = nullptr;
        uint32_t slot = 0;
        uint32_t gen = 0;
        bool owns_pin = false;
        bool is_named_root = false;
    };
    WorkItem item_stack[CONFIG_CAP_MAX_DEPTH + 1];
    static_assert(CONFIG_CAP_MAX_DEPTH + 1 <= kCapMaxDerivations,
                  "revoke stack must fit the derivation budget");
    uint32_t stack_size = 0;
    item_stack[stack_size++] =
        WorkItem{cspace, named_slot, named_gen, false, true};
    bool aborted = false;
    errors::CapError abort_err = errors::CAP_ERR_OK;
    for (uint32_t steps = 0; steps < kRevokeMaxSteps; ++steps) {
        if (stack_size == 0)
            break;
        WorkItem &top = item_stack[stack_size - 1];
        // Budget check runs before the take: every not-yet-taken edge
        // survives the abort, so re-invocation converges (INV-DERIV-11).
        if (budget == 0 && DerivationTable::instance().has_child(
                               *top.node, top.slot, top.gen)) {
            aborted = true;
            abort_err = errors::CAP_ERR_REVOKE_INCOMPLETE;
            break;
        }
        DerivationEdge edge{};
        if (!DerivationTable::instance().take_child(*top.node, top.slot,
                                                    top.gen, edge)) {
            // Fully drained: post-order destroy (INV-DERIV-9), never the
            // named root (root exemption, spec 4.3).  remove_if is
            // gen-checked, so a slot recycled between take and destroy is
            // never deleted (INV-DERIV-10).
            if (!top.is_named_root) {
                if (top.node->remove_if(top.slot, top.gen))
                    ++destroyed;
            }
            if (top.owns_pin)
                top.node->release();
            --stack_size;
            continue;
        }
        // S3-O1: a concurrent record may have landed between has_child
        // and take_child; the take already made monotone progress, so
        // saturate instead of wrapping and let the budget check above
        // report INCOMPLETE on the next iteration.
        if (budget > 0)
            --budget;
        CNode *child_node = nullptr;
        bool owns_pin = false;
        if (edge.child_node_slot == kDerivSelfNode) {
            // Retype self edge: the item/root pins cover this CNode.
            child_node = top.node;
        } else {
            KernelObject *obj = lookup(
                top.node,
                encode_handle(top.node->cspace_id, edge.child_node_slot,
                              edge.child_node_gen),
                CapType::CNode, CapRights::NONE);
            if (obj == nullptr)
                continue; // stale naming slot: edge consumed = progress
            child_node = static_cast<CNode *>(obj);
            owns_pin = true;
        }
        if (stack_size >= CONFIG_CAP_MAX_DEPTH + 1) {
            // Unreachable by construction (INV-DERIV-3 caps creation at
            // depth 8 and the stack holds 9): fail closed.  Restore the
            // taken edge so a retry can still converge (INV-DERIV-11).
            DerivationTable::instance().record(
                *top.node, top.slot, top.gen, *child_node,
                edge.child_node_slot, edge.child_node_gen, edge.child_slot,
                edge.child_gen, edge.depth);
            if (owns_pin)
                child_node->release();
            aborted = true;
            abort_err = errors::CAP_ERR_DEPTH_EXCEEDED;
            break;
        }
        item_stack[stack_size++] = WorkItem{child_node, edge.child_slot,
                                            edge.child_gen, owns_pin, false};
    }
    const bool finished = !aborted && (stack_size == 0);
    errors::CapError result_err = errors::CAP_ERR_OK;
    int result_count = 0;
    bool failed = false;
    if (aborted) {
        result_err = abort_err;
        failed = true;
    } else if (!finished) {
        // Step bound hit without draining: corrupt table (unreachable —
        // iterations are bounded by takes + pops, 2B+2 < kRevokeMaxSteps).
        result_err = errors::CAP_ERR_DERIVATION_CYCLE;
        failed = true;
    } else {
        // Re-verify the named slot under the lock (INV-DERIV-10): a
        // mid-walk delete/recycle denies (BAD_HANDLE) instead of
        // wrong-success.
        bool still = false;
        {
            SpinLockGuard<sync::SpinLock> guard(cspace->lock_);
            const CSlot &named = cspace->slots[named_slot];
            still = named.occupied && named.gen == named_gen &&
                    named.obj == named_target;
        }
        if (!still) {
            result_err = errors::CAP_ERR_BAD_HANDLE;
            failed = true;
        } else {
            result_count = static_cast<int>(destroyed);
        }
    }
    // Single epilogue: release every pin on every exit path.
    while (stack_size > 0) {
        WorkItem &item = item_stack[--stack_size];
        if (item.owns_pin)
            item.node->release();
    }
    cspace->release();
    named_target->release();
    if (failed) {
        if (err_out != nullptr)
            *err_out = result_err;
        return -1;
    }
    return result_count;
}

static_assert(sizeof(CNode) <= kMemPoolMaxBlockSize,
              "CNode growth (serial_) must fit a MemPool block class");

size_t occupied_count(const CNode *cspace) noexcept {
    if (!cspace)
        return 0;
    size_t n = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT); ++i)
        if (cspace->slots[i].occupied)
            ++n;
    return n;
}

/// @brief Records the derivation edge for a successful copy-family install
///        (F2, issue #555).  Resolves the destination's CNode-cap slot in
///        @p src with a bounded scan inside one slot-table critical section
///        (no new lock, no nesting), then records with no CNode lock held
///        (INV-DERIV-1).  A raw in-kernel @p dst (no CNode cap of @p src)
///        records nothing — the derivation graph is the capability graph.
/// @return true when no edge was needed or the edge was recorded; false
///         with @p err_out set (DEPTH_EXCEEDED / DERIVATION_FULL) when the
///         caller must roll its install back (fail closed, never a silent
///         untracked derivation).
bool record_copy_edge(CNode &src, uint32_t src_slot, uint32_t src_gen,
                      CNode &dst, uint32_t installed_idx,
                      uint32_t installed_gen,
                      errors::CapError *err_out) noexcept {
    uint32_t dst_node_slot = static_cast<uint32_t>(CONFIG_CSLOT_COUNT);
    uint32_t dst_node_gen = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(src.lock_);
        for (uint32_t i = 0; i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT);
             ++i) {
            if (src.slots[i].occupied &&
                src.slots[i].type == CapType::CNode &&
                src.slots[i].obj == &dst) {
                dst_node_slot = i;
                dst_node_gen = src.slots[i].gen;
                break;
            }
        }
    }
    if (dst_node_slot >= static_cast<uint32_t>(CONFIG_CSLOT_COUNT))
        return true;
    const uint32_t parent_depth =
        static_cast<uint32_t>(DerivationTable::instance().child_depth(
            src, src_slot, src_gen));
    if (parent_depth + 1 > static_cast<uint32_t>(CONFIG_CAP_MAX_DEPTH)) {
        if (err_out != nullptr)
            *err_out = errors::CAP_ERR_DEPTH_EXCEEDED;
        return false;
    }
    const uint8_t depth = static_cast<uint8_t>(parent_depth + 1);
    if (!DerivationTable::instance().record(src, src_slot, src_gen, dst,
                                            dst_node_slot, dst_node_gen,
                                            installed_idx, installed_gen,
                                            depth)) {
        if (err_out != nullptr)
            *err_out = errors::CAP_ERR_DERIVATION_FULL;
        return false;
    }
    return true;
}

/// @brief Core of copy/grant: pins the source slot target, then installs it
///        into @p dst with @p rights.  The install takes its own acquire();
///        the pin taken here is released on both success and failure.
///        Forwards an optional generation out-slot to the dst install
///        (issue #266: lets syscall returns encode with no TOCTOU).
///        Issue #240 step-a: reads the cached policy word under the existing
///        src lock snapshot (one load, zero new locks); all-allow at step-a
///        so the check always passes (zero delta). Step-b adds default-deny
///        + gen-compare fail-closed.
///        F2 (issue #555): records the derivation edge after a successful
///        install; on record failure the install is rolled back and -1
///        returned with @p err_out set (fail closed).
int do_copy_pinned(CNode *src, uint64_t src_handle, CNode *dst,
                   CapRights rights, uint32_t *gen_out,
                   uint32_t op_mask, errors::CapError *err_out) noexcept {
    if (!src || !dst || src == dst)
        return -1;
    // lookup() returns the target with acquire() already taken.
    KernelObject *target = lookup(src, src_handle, CapType::Null,
                                    CapRights::NONE);
    if (!target)
        return -1;
    CapType type = CapType::Null;
    CapRights src_rights = CapRights::NONE;
    uint32_t src_gen = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(src->lock_);
        const uint32_t idx = handle_slot(src_handle);
        if (idx >= static_cast<uint32_t>(CONFIG_CSLOT_COUNT) ||
            !src->slots[idx].occupied) {
            target->release();
            return -1;
        }
        if (!CNode::policy_allows(src->policy_word_, op_mask)) {
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
    const CapRights effective = rights & src_rights;
    uint32_t installed_gen = 0;
    int installed = dst->install(target, type, effective, &installed_gen);
    if (installed < 0) {
        target->release();
        return -1;
    }
    if (gen_out != nullptr)
        *gen_out = installed_gen;
    // F2 (#555): record the derivation edge; on failure roll the install
    // back (fail closed — never a silent untracked derivation).
    if (!record_copy_edge(*src, handle_slot(src_handle),
                          handle_gen(src_handle), *dst,
                          static_cast<uint32_t>(installed), installed_gen,
                          err_out)) {
        dst->remove(static_cast<uint32_t>(installed));
        target->release();
        return -1;
    }
    target->release();
    return installed;
}

int copy(CNode *src, uint64_t src_handle, CNode *dst, uint32_t *gen_out,
         errors::CapError *err_out) noexcept {
    return do_copy_pinned(src, src_handle, dst,
                          CapRights::READ | CapRights::WRITE | CapRights::COPY |
                              CapRights::GRANT,
                          gen_out, kCapBrokerAllowCopy, err_out);
}

int grant(CNode *src, uint64_t src_handle, CNode *dst, uint32_t *gen_out,
          errors::CapError *err_out) noexcept {
    // Requires CapRights::GRANT on the source slot.
    KernelObject *target =
        lookup(src, src_handle, CapType::Null, CapRights::GRANT);
    if (!target)
        return -1;
    // Capture the type/rights under the lock, then clear GRANT (mint-once).
    // Issue #240 step-a: policy-word read under the existing lock (one
    // load, zero new locks); all-allow so always passes (zero delta).
    CapType type = CapType::Null;
    CapRights rights = CapRights::NONE;
    uint32_t src_gen = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(src->lock_);
        const uint32_t idx = handle_slot(src_handle);
        if (idx >= static_cast<uint32_t>(CONFIG_CSLOT_COUNT) ||
            !src->slots[idx].occupied) {
            target->release();
            return -1;
        }
        if (!CNode::policy_allows(src->policy_word_,
                                   kCapBrokerAllowGrant)) {
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
    uint32_t installed_gen = 0;
    int installed = dst->install(target, type, rights, &installed_gen);
    if (installed < 0) {
        target->release();
        return -1;
    }
    if (gen_out != nullptr)
        *gen_out = installed_gen;
    // F2 (#555): record the edge BEFORE consuming the mint-once GRANT
    // right, so a record failure leaves the source authority intact.
    // S3-O3: the pin release stays visible on the failure path below.
    if (!record_copy_edge(*src, handle_slot(src_handle),
                          handle_gen(src_handle), *dst,
                          static_cast<uint32_t>(installed), installed_gen,
                          err_out)) {
        dst->remove(static_cast<uint32_t>(installed));
        target->release();
        return -1;
    }
    src->clear_grant(handle_slot(src_handle));
    target->release();
    return installed;
}

int mint(CNode *src, uint64_t src_handle, CNode *dst, CapRights rights_mask,
         uint32_t badge, uint32_t *gen_out,
         errors::CapError *err_out) noexcept {
    (void)badge; // badge re-branding lands with endpoint integration (Phase 4)
    return do_copy_pinned(src, src_handle, dst, rights_mask, gen_out,
                          kCapBrokerAllowMint, err_out);
}

} // namespace kernel::cap
