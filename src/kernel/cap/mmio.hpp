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

/// @file mmio.hpp
/// @brief Capability-wrapped MMIO (BAR) range (v0.4.2, issues #3/#8).
/// Wraps a PCI BAR range: memory BARs enable capability-gated MMIO page
/// mapping; IO BARs drive sys_ioport_grant (fine-grained I/O delegation).
/// Issue #8 adds the user-space MMIO map registry (MmioUserMap) and the IOPB
/// grant ledger (arch::iopb_ledger_*) that closes the #3 revocation gap.

#pragma once

#include <types.hpp>
#include <kernel/arch/pci.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/sync/spinlock.hpp>

namespace kernel {
struct TaskControlBlock;
}

namespace kernel::cap {

/// @brief A capability-gated MMIO/BAR range.  Does NOT own physical memory
/// (device memory is never PMM-owned) — dispose only frees the MemPool
/// block.  Shared-heap class (FrameCap pattern).
class MmioCap : public KernelObject {
  public:
    /// @brief Physical base of the BAR range.
    uint64_t phys = 0;
    /// @brief Size of the BAR range in bytes.
    uint64_t size = 0;
    /// @brief BAR type (MEMORY_32 / MEMORY_64 / IO).
    arch::PciBarType bar_type = arch::PciBarType::MEMORY_32;

    /// @brief Allocates an MmioCap from the MemPool and pool-marks it.
    ///        Validates the range (IO ranges must lie in the 64 KiB port
    ///        space; memory ranges must be page-aligned).  Returns nullptr on
    ///        failure or when CONFIG_CAP_MAX_MMIO live objects are reached.
    static MmioCap *create(uint64_t phys, uint64_t size,
                           arch::PciBarType bar_type);

    /// @brief Convenience factory wrapping an already-parsed PCI BAR.
    static MmioCap *create_from_bar(const arch::PciBar &bar);

    /// @brief Final teardown: releases the MemPool block (device memory is
    ///        never freed).  Also retroactively removes this cap's grants and
    ///        user MMIO mappings (issue #8 revocation closure).
    void dispose() noexcept override;

    /// @brief Marks the cap revoked; retroactively removes this cap's grants
    ///        and user MMIO mappings (issue #8).  Called by cap::revoke.
    void revoke() noexcept override;

    /// @brief Genuinely shared (referenced by capability slots).
    bool is_shared() const noexcept override {
        return true;
    }
};

/// @brief User-space MMIO page-frame mapping registry (issue #8).  A static
/// bounded table keyed by task id (IrqDelivery pattern) that maps an MmioCap's
/// memory BAR into a fixed user VA window for Ring 3 drivers via the
/// SYS_MMIO_MAP/SYS_MMIO_UNMAP syscall surface.  Keyed by owner_task_id + a
/// per-slot generation (recycled-slot stale-VA defense).  No dynamic
/// allocation on RT paths.  Lock order: scheduler_lock_ -> g_iopb_lock ->
/// mmio map lock (never held across VMM map/unmap or reschedule).
class MmioUserMap {
  public:
    /// @brief Maps @p mmio's memory BAR into @p task's user VA window.
    /// @return The user VA base (CONFIG_USER_MMIO_VA_BASE + slot*REGION), or
    ///         -1 on a revoked/IO cap, a zero range, an oversized range, a
    ///         full registry, a missing user page table, or a failed map.
    static uint64_t map(kernel::TaskControlBlock &task, cap::MmioCap &mmio);

    /// @brief Unmaps the mapping at @p va if it belongs to @p task.
    /// @return true when a mapping was removed.
    static bool unmap(kernel::TaskControlBlock &task, uint64_t va);

    /// @brief Immediately removes every live mapping backed by @p mmio
    ///        (revocation closure, issue #8).  Called from MmioCap::dispose/
    ///        revoke before the cap block is freed.  Pointer-equality only —
    ///        never dereferenced.
    static void invalidate_cap(cap::MmioCap *mmio);

    /// @brief Removes every live mapping owned by @p task.  Called from
    ///        TaskControlBlock::cleanup() (never leaves a dangling PTE in a
    ///        recycled PML4).
    static void drain_task(kernel::TaskControlBlock &task);

    /// @brief Clears the whole registry (test isolation snapshot restore).
    static void snapshot_reset();

    /// @brief Number of live mappings (test accessor).
    static size_t live_count();

    /// @brief Whether @p task currently owns the mapping at @p va (test
    ///        accessor).
    static bool is_owner(kernel::TaskControlBlock &task, uint64_t va);

  private:
    static constexpr size_t kMaxMaps = CONFIG_CAP_MAX_MMIO_MAPS;
    struct Slot {
        uint64_t owner_task_id = 0; ///< owning task (0 = free)
        uint64_t owner_gen = 0;     ///< TCB generation at map time
        cap::MmioCap *mmio = nullptr; ///< backing cap (equality only)
        uint64_t va = 0;            ///< user VA base of the mapping
        uint64_t pml4 = 0;          ///< owning task's PML4 phys at map time
        bool occupied = false;      ///< slot in use
    };
    static Slot s_slots_[kMaxMaps];
    static sync::SpinLock s_lock_;
    static Slot *slot(size_t i);
};

} // namespace kernel::cap