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

/// @file msix.cpp
/// @brief MsixCap implementation (issue #10).  Bound by CONFIG_CAP_MAX_MSIX
/// via a TU-local live counter (mmio.cpp pattern); folds into the existing
/// cap_objects ResourceTracker counter.  The delivery-table slot is claimed
/// at create() time (single-owner per vector, cross-type with IrqCap) and
/// released at dispose/revoke.  The MSI-X table entry is programmed MASKED at
/// create; only IrqDelivery::arm unmasks it (issue #10 fail-closed contract).

#include <kernel/cap/msix.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/irq_delivery.hpp>
#include <kernel/arch/pci.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <constants.hpp>

// Placement new (defined in lib/new.cpp, no <new> header in freestanding)
inline void *operator new(unsigned long, void *placement) noexcept {
    return placement;
}

namespace kernel::cap {

/// @brief Live MsixCap count, bounded by CONFIG_CAP_MAX_MSIX.
static uint32_t g_live_msix = 0;

/// @brief Per-(BDF, entry) single-owner registry (issue #10).  One MSI-X
/// table entry programs exactly ONE vector: a second cap claiming the same
/// entry would reprogram the shared table slot with a different vector and
/// hijack the first owner's delivery.  Fail closed (never ENSURE — reachable
/// contention).
namespace {
struct MsixEntryClaim {
    bool occupied = false;
    arch::PciBdf bdf{};
    uint16_t entry_index = 0;
};
MsixEntryClaim g_entry_claims[CONFIG_CAP_MAX_MSIX];

/// @brief Claims (bdf, entry) for @p owner_count entries; @p index receives
///        the claim slot.  @return true on success.
bool entry_claim(const arch::PciBdf &bdf, uint16_t entry_index) {
    for (size_t i = 0; i < static_cast<size_t>(CONFIG_CAP_MAX_MSIX); ++i) {
        if (g_entry_claims[i].occupied && g_entry_claims[i].bdf == bdf &&
            g_entry_claims[i].entry_index == entry_index)
            return false; // already owned by another live cap
    }
    for (size_t i = 0; i < static_cast<size_t>(CONFIG_CAP_MAX_MSIX); ++i) {
        if (!g_entry_claims[i].occupied) {
            g_entry_claims[i].occupied = true;
            g_entry_claims[i].bdf = bdf;
            g_entry_claims[i].entry_index = entry_index;
            return true;
        }
    }
    return false; // registry full — fail closed
}

/// @brief Releases the (bdf, entry) claim.
void entry_release(const arch::PciBdf &bdf, uint16_t entry_index) {
    for (size_t i = 0; i < static_cast<size_t>(CONFIG_CAP_MAX_MSIX); ++i) {
        if (g_entry_claims[i].occupied && g_entry_claims[i].bdf == bdf &&
            g_entry_claims[i].entry_index == entry_index) {
            g_entry_claims[i].occupied = false;
            return;
        }
    }
}
} // namespace

MsixCap *MsixCap::create(const arch::PciBdf &bdf, uint16_t entry_index) {
    // BDF bounds (issue #4 gotcha: validate every indexing site).
    if (bdf.bus >= arch::PCI_MAX_BUSES || bdf.device >= arch::PCI_MAX_DEVICES ||
        bdf.function >= arch::PCI_MAX_FUNCTIONS)
        return nullptr;

    if (__atomic_load_n(&g_live_msix, __ATOMIC_RELAXED) >=
        static_cast<uint32_t>(CONFIG_CAP_MAX_MSIX))
        return nullptr;

    // Locate and map the MSI-X table (validates capability presence, memory
    // BAR, entry bounds vs BAR size).
    arch::PciMsixTableInfo tbl{};
    if (!arch::pci_msix_table_info(bdf, tbl))
        return nullptr;
    if (entry_index >= tbl.entry_count)
        return nullptr;

    // Single-owner per (BDF, entry): one table entry programs exactly one
    // vector — a duplicate claim would reprogram the shared table slot and
    // hijack the first owner's delivery.  Fail closed.
    if (!entry_claim(bdf, entry_index))
        return nullptr;

    // Single-owner per vector: allocate the vector and claim the delivery
    // slot as ONE IrqGuard-serialized pair so a concurrent create cannot
    // interleave the alloc->claim window (issue #10, cross-type with IrqCap).
    uint8_t vector = 0;
    int16_t reg_idx = -1;
    {
        arch::IrqGuard irq_guard{};
        vector = arch::pci_alloc_vector();
        if (vector == 0) {
            entry_release(bdf, entry_index);
            return nullptr;
        }
        reg_idx = IrqDelivery::claim_slot(vector);
        if (reg_idx < 0) {
            arch::pci_free_vector(vector);
            entry_release(bdf, entry_index);
            return nullptr;
        }
    }

    auto *msix = static_cast<MsixCap *>(MemPool::alloc(sizeof(MsixCap)));
    if (!msix) {
        // Roll back the claimed vector + slot + entry on a failed alloc.
        arch::pci_free_vector(vector);
        IrqDelivery::release_slot_idx(reg_idx, nullptr);
        entry_release(bdf, entry_index);
        return nullptr;
    }
    new (msix) MsixCap;
    msix->mark_pool_backed();
    msix->bdf = bdf;
    msix->entry_index = entry_index;
    msix->vector = vector;
    msix->reg_idx_ = reg_idx;
    msix->tbl_ = tbl;

    // Program the entry MASKED (fail-closed — no delivery until arm unmasks).
    if (!arch::pci_program_msix_entry(bdf, tbl, entry_index, vector,
                                      /*apic_id=*/0)) {
        arch::pci_free_vector(vector);
        IrqDelivery::release_slot_idx(reg_idx, nullptr);
        entry_release(bdf, entry_index);
        MemPool::free(msix);
        return nullptr;
    }

    // Enable the MSI-X function (Message Control ENABLE, clear FUNCMASK).
    uint8_t cap = arch::pci_find_capability(bdf, arch::PCI_CAP_ID_MSIX);
    if (cap == 0) {
        arch::pci_free_vector(vector);
        IrqDelivery::release_slot_idx(reg_idx, nullptr);
        entry_release(bdf, entry_index);
        MemPool::free(msix);
        return nullptr;
    }
    uint16_t ctrl = arch::pci_config_readw(arch::pci_make_addr(bdf, cap + 2));
    ctrl |= arch::PCI_MSIX_CTRL_ENABLE;
    ctrl &= ~arch::PCI_MSIX_CTRL_FUNCMASK;
    arch::pci_config_writel(arch::pci_make_addr(bdf, cap + 2), ctrl);

    // Bind ownership so a later sys_irq_register/sys_irq_wait can re-validate
    // that the slot at reg_idx_ still belongs to THIS cap (slot reuse safety,
    // issue #2).
    IrqDelivery::set_slot_owner(reg_idx, msix);
    __atomic_fetch_add(&g_live_msix, 1U, __ATOMIC_RELAXED);
    kernel::test::ResourceTracker::instance().track_cap_object_add();
    return msix;
}

void MsixCap::set_entry_masked(bool masked) {
    arch::pci_msix_entry_set_masked(tbl_, entry_index, masked);
}

bool MsixCap::entry_masked() const {
    return arch::pci_msix_entry_masked(tbl_, entry_index);
}

void MsixCap::dispose() noexcept {
    // Re-mask the entry FIRST (fail-closed: never leave an unmasked entry
    // delivering into a vector we are about to release), then unregister the
    // vector from the delivery table (disarm + wake any blocked waiter with
    // -1), then free the vector and the block.  release_slot_idx revalidates
    // ownership under the slot lock (issue #2): a slot drained at task death
    // and reused for another cap/vector is NOT released by a stale reg_idx_.
    if (reg_idx_ >= 0) {
        set_entry_masked(true);
        IrqDelivery::release_slot_idx(reg_idx_, this);
    }
    if (vector != 0)
        arch::pci_free_vector(vector);
    entry_release(bdf, entry_index);
    reg_idx_ = -1;
    if (__atomic_load_n(&g_live_msix, __ATOMIC_RELAXED) > 0)
        __atomic_fetch_sub(&g_live_msix, 1U, __ATOMIC_RELAXED);
    kernel::test::ResourceTracker::instance().track_cap_object_remove();
    MemPool::free(this);
}

void MsixCap::revoke() noexcept {
    if (reg_idx_ >= 0) {
        set_entry_masked(true);
        IrqDelivery::release_slot_idx(reg_idx_, this);
    }
    if (vector != 0)
        arch::pci_free_vector(vector);
    entry_release(bdf, entry_index);
    reg_idx_ = -1;
    // Mark revoked so acquire() refuses and lookup fails.
    KernelObject::revoke();
}

void MsixCap::snapshot_reset() {
    // Test-isolation rewind: clear the entry-claim registry so a recycled
    // (bdf, entry) is claimable again (the snapshot restores MsixCap blocks
    // and delivery slots, but this static registry is not part of it).
    for (size_t i = 0; i < static_cast<size_t>(CONFIG_CAP_MAX_MSIX); ++i) {
        g_entry_claims[i].occupied = false;
        g_entry_claims[i].bdf = arch::PciBdf{};
        g_entry_claims[i].entry_index = 0;
    }
}

} // namespace kernel::cap
