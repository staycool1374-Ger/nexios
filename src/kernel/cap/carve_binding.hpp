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

/// @file carve_binding.hpp
/// @brief Carve-binding registry (issue #564, F11 Stage C): the ONE
///        registry owning both new surfaces' authority — the address-space
///        root a carved child runs on and every cap keeping that child
///        alive (TCB region, VSpace root, per-region frames, linked
///        tables).
///
///        INV-CARVE-4 (one releaser): every cap keeping a carved child
///        alive is pinned in exactly one slot keyed (pid, generation).
///        The slot drains exactly once, as the last statement of the
///        teardown tail — after the drain nothing may read or write the
///        TCB (its storage may be gone).  Double-drain is a
///        generation-mismatch no-op (INV-PROC-5 precedent).
///        INV-CARVE-7 (lock order): one TU-local leaf lock (s_lock_).
///        Order is CNode slot lock -> VSpaceMap s_lock_ -> binding leaf,
///        one-directional: vspace_map.cpp calls only the pure lock-free
///        carve_root_is_activated() probe, so there is no s_lock_ <->
///        binding cycle.  The binding leaf is never held across
///        VSpaceMap/VMM/PMM, never with scheduler_lock_, never from an
///        ISR.  Pins are released AFTER unlocking (a pin may dispose ->
///        MemPool::free, LEARNINGS #320).
///        INV-CARVE-5: the carved birth site takes its generation from
///        next_task_generation() before the child is reachable.
///        S3-2 (design-review-564-stageC-r2): the activation probe shares
///        the single CONFIG_CARVE_BINDINGS bound for index modulus, scan
///        bound and array size; wrap is mod size; readers use acquire
///        loads matching the writers' release stores.

#pragma once

#include <types.hpp>
#include <kernel/cap/vspace_errors.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/nexios_config.h>

namespace kernel {
struct TaskControlBlock;
} // namespace kernel

namespace kernel::cap {

class CarvedObjectCap;
class FrameCap;

/// @brief Bounded registry for carve-composed fork bindings (issue #564,
///        F11 Stage C).  All methods are noexcept, bounded (no unbounded
///        loop, no allocation, no blocking wait, no reschedule), and safe
///        from task context only (never ISR).
class CarveBinding {
  public:
    /// @brief Slot lifecycle: FREE -> COMPOSING -> ACTIVE; DRAINING is
    ///        the terminal transient between match and FREE.  A fresh
    ///        slot is FREE with null pins.
    enum class State : uint8_t {
        FREE = 0,
        COMPOSING = 1,
        ACTIVE = 2,
        DRAINING = 3,
    };

    /// @brief Claim a FREE slot for an unused (pid, gen), mark COMPOSING.
    /// @return Slot index, or -1 when full or (pid, gen) already bound
    ///         (fail closed — the caller rolls back).
    [[nodiscard]] static int begin(uint64_t pid, uint32_t gen) noexcept;
    /// @brief Pin the carved TCB region cap into @p slot (acquires).
    /// @return False when the slot is unusable or the acquire fails.
    static bool bind_tcb_cap(int slot, CarvedObjectCap *cap) noexcept;
    /// @brief Pin the carved VSpace root cap into @p slot (acquires).
    /// @return False when the slot is unusable or the acquire fails.
    static bool bind_root(int slot, CarvedObjectCap *cap) noexcept;
    /// @brief Pin one per-region FrameCap into @p slot (acquires).
    /// @return False when the slot is unusable, full, or acquire fails.
    static bool bind_frame(int slot, FrameCap *cap) noexcept;
    /// @brief Pin one linked PageTable cap into @p slot (acquires).
    ///        The VSpaceMap link registry stores physical identities
    ///        only (INV-VS-8) and holds no pin, so the binding is the
    ///        owner keeping linked tables alive until teardown drain
    ///        (INV-CARVE-4: every cap keeping the child alive is pinned
    ///        in exactly one slot).
    /// @return False when the slot is unusable, full, or acquire fails.
    static bool bind_table(int slot, CarvedObjectCap *cap) noexcept;
    /// @brief Abandon a COMPOSING slot: release whatever it pinned and
    ///        mark FREE.  Failure paths only; never touches ACTIVE slots.
    static void abandon(int slot) noexcept;
    /// @brief Top-down re-validation of the carved subtree under
    ///        @p root_phys through the per-arch codec: refuses a leaf in
    ///        a table position, a user-marked component at or above
    ///        PML4_USER_COUNT, any link the VSpaceMap registry does not
    ///        know, and a live (currently active) root.  Pure walk: takes
    ///        no binding lock (see INV-CARVE-7), allocates nothing.
    /// @return VSPACE_ERR_OK, else the single typed reason.
    [[nodiscard]] static errors::VspaceError
    validate_subtree(uint64_t root_phys) noexcept;
    /// @brief Publish @p slot: re-checks COMPOSING, re-validates the
    ///        subtree, marks ACTIVE and raises the activation flag.
    ///        Publishes nothing else (INV-CARVE-6: no register write).
    /// @return True on success; false maps through activate_err.
    static bool activate(int slot, uint64_t root_phys) noexcept;
    /// @brief Typed activation: OK, SUBTREE_INVALID, REGISTRY_FULL
    ///        (probe exhaustion), or TARGET_REJECTED (bad slot/state).
    [[nodiscard]] static errors::VspaceError
    activate_err(int slot, uint64_t root_phys) noexcept;
    /// @brief Drain the binding for (pid, gen): mark DRAINING, clear the
    ///        activation flag, release every pin AFTER unlocking, then
    ///        mark FREE with a monotonic generation bump.  Idempotent:
    ///        replay is a generation/state-mismatch no-op.  Last
    ///        statement of the carved teardown tail (INV-CARVE-4).
    static void drain_task(uint64_t pid, uint32_t gen) noexcept;
    /// @brief Clear the activation flag for (pid, gen) (exec path: the
    ///        root is replaced, pins stay until teardown drain), then a
    ///        full TLB shootdown after the unlock (no ASID narrowing — no
    ///        ASID is assigned to a carved root).  The slot stays ACTIVE.
    static void deactivate(uint64_t pid, uint32_t gen) noexcept;
    /// @brief Test isolation: clear every slot and every activation flag,
    ///        releasing pins outside the lock (FrameUserMap::snapshot_reset
    ///        shape, with the #320 detector comment in the .cpp).
    static void snapshot_reset() noexcept;
    /// @brief Number of live (non-FREE) bindings (test/QE accessor).
    static size_t live_count() noexcept;

  private:
    /// @brief One carved-child binding: physical identities plus pins.
    ///        Pins keep the region alive exactly until drain_task.  Table
    ///        pins keep linked tables alive: VSpaceMap stores phys only
    ///        (INV-VS-8, no pins), so without these the first release
    ///        would dispose a live tree page.
    struct Slot {
        uint64_t pid = 0;       ///< bound task id (0 = free)
        uint32_t gen = 0;       ///< bound task generation (ABA guard)
        CarvedObjectCap *tcb_cap = nullptr;  ///< pinned TCB region
        CarvedObjectCap *root_cap = nullptr; ///< pinned VSpace root
        FrameCap *frames[CONFIG_CARVE_MAX_REGIONS] = {}; ///< pins
        CarvedObjectCap *tables[CONFIG_CARVE_MAX_TABLES] = {}; ///< pins
        uint8_t frame_count = 0; ///< pinned frame count
        uint8_t table_count = 0; ///< pinned table count
        State state = State::FREE;
    };
    static Slot s_slots_[CONFIG_CARVE_BINDINGS];
    static sync::SpinLock s_lock_;

    /// @brief Lock-free activation probe triplets (S3-2 single bound):
    ///        flag sampled before and after phys+gen; mismatch = retry.
    ///        flag == 1 with matching phys means live activation.
    static uint64_t s_active_phys_[CONFIG_CARVE_BINDINGS];
    static uint32_t s_active_gen_[CONFIG_CARVE_BINDINGS];
    static uint8_t s_active_flag_[CONFIG_CARVE_BINDINGS];

    /// @brief Probe index for @p root_phys (S3-2: one index function,
    ///        shared by readers and writers).
    static size_t probe_index(uint64_t root_phys) noexcept;
    /// @brief Publish the activation triplet for @p root_phys.
    ///        Caller holds s_lock_; sets the flag last (release).
    static bool probe_publish_locked(uint64_t root_phys) noexcept;
    /// @brief Clear the activation triplet for @p root_phys, bump the
    ///        slot generation (ABA: a freed-and-recycled root phys
    ///        presents a different gen).  Caller holds s_lock_.
    static void probe_clear_locked(uint64_t root_phys) noexcept;

    // Free-probe access (same TU only): the phys-keyed readers/writers
    // below touch the probe triplets and the slot table without taking
    // a header cycle through the class.
    friend bool carve_root_is_activated(uint64_t root_phys) noexcept;
    friend void carve_note_activated(uint64_t root_phys) noexcept;
    friend void carve_note_deactivated(uint64_t root_phys) noexcept;
};

/// @brief Lock-free probe: true when @p root_phys names a live activated
///        root.  Pure (no lock — the INV-VS-9 precedent): vspace_map.cpp
///        consults activation without a header cycle.  Reader samples
///        flag-phys-gen-flag with acquire ordering (S3-2); any mismatch
///        retries the slot; the scan is bounded (CONFIG_CARVE_BINDINGS).
bool carve_root_is_activated(uint64_t root_phys) noexcept;

/// @brief Phys-keyed activation publish for paths holding no slot (test/QE
///        seam; production writers are activate/deactivate).  Finds the
///        slot pinning @p root_phys under the lock and publishes.
void carve_note_activated(uint64_t root_phys) noexcept;

/// @brief Phys-keyed activation clear for paths holding no slot (test/QE
///        seam).  No shootdown (that is deactivate's job).
void carve_note_deactivated(uint64_t root_phys) noexcept;

} // namespace kernel::cap
