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

/// @file irq.cpp
/// @brief IrqCap implementation (issue #2).  Bound by CONFIG_CAP_MAX_IRQ via
/// a TU-local live counter (mmio.cpp pattern); folds into the existing
/// cap_objects ResourceTracker counter.  The delivery-table slot is claimed
/// at create() time (single-owner per vector) and released at dispose/revoke.

#include <kernel/cap/irq.hpp>
#include <kernel/irq_delivery.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <constants.hpp>

// Placement new (defined in lib/new.cpp, no <new> header in freestanding)
inline void *operator new(unsigned long, void *placement) noexcept {
    return placement;
}

namespace kernel::cap {

/// @brief Live IrqCap count, bounded by CONFIG_CAP_MAX_IRQ.
static uint32_t g_live_irqs = 0;

IrqCap *IrqCap::create(uint8_t vector) {
    if (__atomic_load_n(&g_live_irqs, __ATOMIC_RELAXED) >=
        static_cast<uint32_t>(CONFIG_CAP_MAX_IRQ))
        return nullptr;

    // Single-owner per vector: claim the delivery-table slot up front (also
    // validates the vector window and rejects threaded-IRQ overlap).
    int16_t idx = IrqDelivery::claim_slot(vector);
    if (idx < 0)
        return nullptr;

    auto *irq = static_cast<IrqCap *>(MemPool::alloc(sizeof(IrqCap)));
    if (!irq) {
        // Release the claimed slot — a failed alloc must not hold a vector.
        // The slot has no owner yet (ownerless claim), so release with nullptr.
        IrqDelivery::release_slot_idx(idx, nullptr);
        return nullptr;
    }
    new (irq) IrqCap;
    irq->mark_pool_backed();
    irq->vector = vector;
    irq->reg_idx_ = idx;
    // Bind ownership so a later sys_irq_register/sys_irq_wait can re-validate
    // that the slot at reg_idx_ still belongs to THIS cap (slot reuse safety,
    // issue #2).  Without this a stale reg_idx_ surviving a drain+reuse could
    // arm/consume a different cap's vector.
    IrqDelivery::set_slot_owner(idx, irq);
    __atomic_fetch_add(&g_live_irqs, 1U, __ATOMIC_RELAXED);
    kernel::test::ResourceTracker::instance().track_cap_object_add();
    return irq;
}

void IrqCap::dispose() noexcept {
    // Unregister the vector from the delivery table (disarm + wake any
    // blocked waiter with -1) before freeing the block.  release_slot_idx
    // revalidates ownership under the slot lock (issue #2): a slot drained at
    // task death and reused for another cap/vector is NOT released by a stale
    // reg_idx_ (atomic check+release, no TOCTOU).
    if (reg_idx_ >= 0)
        IrqDelivery::release_slot_idx(reg_idx_, this);
    reg_idx_ = -1;
    if (__atomic_load_n(&g_live_irqs, __ATOMIC_RELAXED) > 0)
        __atomic_fetch_sub(&g_live_irqs, 1U, __ATOMIC_RELAXED);
    kernel::test::ResourceTracker::instance().track_cap_object_remove();
    MemPool::free(this);
}

void IrqCap::revoke() noexcept {
    if (reg_idx_ >= 0)
        IrqDelivery::release_slot_idx(reg_idx_, this);
    reg_idx_ = -1;
    // Mark revoked so acquire() refuses and lookup fails.
    KernelObject::revoke();
}

} // namespace kernel::cap
