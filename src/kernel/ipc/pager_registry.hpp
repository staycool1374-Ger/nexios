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

/// @file pager_registry.hpp
/// @brief External pager protocol registry (issue #107): user-space #PF
/// delegation to a capability-designated Ring-3 pager.  Mirrors
/// DeathNotify/FrameUserMap: static bounded table, claim-under-lock /
/// poke-outside-lock, id+generation revalidation.  The bounded pager contract
/// (docs/specs/external-pager.md §4) designs the "pager blocked while faulting
/// task waits" deadlock out by construction.

#pragma once

#include <types.hpp>
#include <constants.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/sync/spinlock.hpp>

namespace kernel {
struct TaskControlBlock;
}

namespace kernel::ipc {

/// @brief Kernel-side record of one delegated page fault (paper §4.2).
/// Lives in a registry slot; exactly one outstanding fault per registration
/// (a client with a pending fault is BLOCKED and cannot fault again).
struct PagerFault {
    uint64_t fault_id = 0;   ///< monotonic sequence number
    uint64_t client_id = 0;  ///< faulting client id
    uint32_t client_gen = 0; ///< client TCB generation at record time
    uint64_t pager_id = 0;   ///< serving pager id
    uint32_t pager_gen = 0;  ///< pager TCB generation at record time
    uint64_t fault_va = 0;   ///< page-aligned faulting VA
    uint64_t fault_flags = 0; ///< raw #PF error-code bits (info)
    uint64_t deadline_tick = 0; ///< now + CONFIG_PAGER_FAULT_TIMEOUT_TICKS
    uint64_t client_pml4 = 0; ///< client PML4 phys captured at record time
    // Committed (resolved) mappings live on the Slot, not here (auditor S3-4:
    // these fields are vestigial).  A pending fault has no committed pages —
    // map() commits atomically under IrqGuard into the slot table.
    bool map_in_progress = false; ///< MAP_IN_PROGRESS pin (§7.2)
    bool poisoned_va = false;     ///< aborted-VA latch (§4.5)
};

/// @brief User-visible RECV payload (exactly {fault_id, client_id, fault_va,
/// fault_flags}).
struct PagerFaultMsg {
    uint64_t fault_id = 0;
    uint64_t client_id = 0;
    uint64_t fault_va = 0;
    uint64_t fault_flags = 0;
};

/// @brief Wakeup value pulsed into a pager's Notify when a client faults.
/// Wakeup-only; the authoritative data is drained via SYS_PAGER_RECV.
constexpr uint64_t PAGER_FAULT_PULSE = 0x50414752;

/// @brief External pager registry (issue #107).  A static bounded table keyed
/// by client task (id + generation): one registration per client pairing it
/// with its designated pager, plus the single outstanding fault record per
/// registration.  The bounded pager contract: the faulting client blocks only
/// on its passive registry record + the scheduler and is woken by exactly one
/// of {MAP-complete, ABORT, watchdog-timeout, death-drain}; the kernel never
/// waits on the pager.  No dynamic allocation on RT paths.  Lock order:
/// scheduler_lock_ -> pager registry lock (never held across a scheduler call,
/// a VMM map/unmap, a Notify poke, or a cap release).
class PagerRegistry {
  public:
    /// @brief Designates @p pager_pid as the pager for the calling client.
    /// Authority: the caller may register only for itself (pager live,
    /// pager != client).  Fails closed when the registry is full.
    static bool register_client(kernel::TaskControlBlock &client,
                                uint64_t pager_pid);

    /// @brief Removes the registration for @p client_pid.  Authority: the
    ///        caller must be the client itself or its designated pager (F3).
    ///        Aborts any pending fault and releases committed mappings.
    static bool unregister(kernel::TaskControlBlock &caller,
                           uint64_t client_pid);

    /// @brief Drains the next pending fault for @p pager (never blocks).
    /// @return 1 when a fault was copied, 0 when none is pending.
    static int recv(kernel::TaskControlBlock &pager, PagerFaultMsg &out);

    /// @brief Pager explicitly cannot satisfy @p fault_id: consume, rollback
    ///        ledger, wake the client, poison the VA latch (paper §4.5).
    static int abort(kernel::TaskControlBlock &pager, uint64_t fault_id);

    /// @brief Maps the pager's FrameCap into the client's PML4 at the fault VA
    ///        and completes the fault (paper §5, §7.2 TOCTOU pin).
    static int map(kernel::TaskControlBlock &pager, uint64_t fault_id,
                   cap::FrameCap *fc, uint64_t count, uint64_t flags);

    /// @brief Classifies a user-mode #PF (paper §3.2 F1-F10); if delegatable,
    ///        records the fault, pulses the pager, and blocks the client
    ///        inside the #PF ISR (paper §4.8).  Returns true when delegated.
    static bool delegate_fault(kernel::TaskControlBlock &client,
                               uint64_t error_code, uint64_t *regs,
                               uint64_t cr2);

    /// @brief Timer watchdog (called from on_tick): expire overdue faults
    ///        (per-fault fail-closed, registration KEPT, VA poison latch).
    static void watchdog_scan(uint64_t now);

    /// @brief Removes every registration/fault whose client OR pager is @p t
    ///        (cleanup/exec drain; rolls back any ledger before the client's
    ///        free_user_pages, and before the pager's Notify is destroyed).
    static void drain_task(kernel::TaskControlBlock &t);

    /// @brief Unmaps every ledger entry backed by @p fc across all clients
    ///        (FrameCap revoke/dispose closure).
    static void invalidate_cap(cap::FrameCap *fc);

    /// @brief Clears the whole registry (test isolation snapshot restore).
    static void snapshot_reset();

    /// @brief Number of live registrations (test accessor).
    static size_t live_count();

    /// @brief Whether @p client currently has a registration (test accessor).
    static bool is_registered(kernel::TaskControlBlock &client);

    /// @brief Whether @p pager has a pending fault record (test accessor).
    static bool pending_fault(kernel::TaskControlBlock &pager);

  private:
    static constexpr size_t kMaxClients = CONFIG_CAP_MAX_PAGER_CLIENTS;
    enum class SlotState : uint8_t { FREE = 0, ACTIVE };
    struct Slot {
        SlotState state = SlotState::FREE;
        uint64_t client_id = 0;  ///< client task id
        uint32_t client_gen = 0; ///< client TCB generation at register time
        uint64_t pager_id = 0;   ///< pager task id
        uint32_t pager_gen = 0;  ///< pager TCB generation at register time
        uint64_t poisoned_va = 0; ///< aborted-VA latch (F9); 0 = none
        bool poisoned_set = false;
        PagerFault fault = {};   ///< the single outstanding fault (if pending)
        bool pending = false;    ///< a fault record is live for this slot
        // Committed (resolved) pager mappings into the client's PML4.  These
        // PERSIST after the fault completes — the pins keep the pager's
        // FrameCap alive so free_user_pages can never free a pager-owned frame
        // that the client's PML4 still references.  Released only by
        // drain_task / invalidate_cap / unregister / snapshot_reset.
        uint64_t mapped_va[CONFIG_PAGER_MAX_COMMITTED_PAGES] = {};
        uint64_t mapped_pml4[CONFIG_PAGER_MAX_COMMITTED_PAGES] = {};
        cap::FrameCap *mapped_pin[CONFIG_PAGER_MAX_COMMITTED_PAGES] = {};
        uint32_t mapped_count = 0;
    };
    static Slot s_slots_[kMaxClients];
    static sync::SpinLock s_lock_;
    static uint64_t s_fault_seq_;
};

} // namespace kernel::ipc