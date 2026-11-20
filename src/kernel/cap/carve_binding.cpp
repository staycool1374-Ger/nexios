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

/// @file carve_binding.cpp
/// @brief Carve-binding registry implementation (issue #564, F11 Stage C):
///        claim-under-lock / act-outside-lock / revalidate discipline
///        (the FrameUserMap::map precedent).  Pins are KernelObject
///        acquire()s on shared objects only, so the ScopedRef
///        refcount==1 contract (kernel_object.hpp) is untouched.

#include <kernel/cap/carve_binding.hpp>
#include <kernel/cap/carved_object.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/cap/vspace_map.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/arch/page_table.hpp>
#include <constants.hpp>

namespace kernel::cap {

static_assert(CONFIG_CARVE_BINDINGS > 0,
              "carve binding registry must hold a binding");
static_assert(CONFIG_CARVE_MAX_REGIONS > 0,
              "carve binding must pin a frame");

CarveBinding::Slot CarveBinding::s_slots_[CONFIG_CARVE_BINDINGS];
sync::SpinLock CarveBinding::s_lock_{};
uint64_t CarveBinding::s_active_phys_[CONFIG_CARVE_BINDINGS];
uint32_t CarveBinding::s_active_gen_[CONFIG_CARVE_BINDINGS];
uint8_t CarveBinding::s_active_flag_[CONFIG_CARVE_BINDINGS];

size_t CarveBinding::probe_index(uint64_t root_phys) noexcept {
    return static_cast<size_t>((root_phys / arch::PAGE_SIZE) %
                               CONFIG_CARVE_BINDINGS);
}

bool CarveBinding::probe_publish_locked(uint64_t root_phys) noexcept {
    const size_t base = probe_index(root_phys);
    for (size_t step = 0; step < CONFIG_CARVE_BINDINGS; ++step) {
        const size_t idx =
            (base + step) % CONFIG_CARVE_BINDINGS;
        const uint8_t flag =
            __atomic_load_n(&s_active_flag_[idx], __ATOMIC_RELAXED);
        const uint64_t phys =
            __atomic_load_n(&s_active_phys_[idx], __ATOMIC_RELAXED);
        if (flag != 0) {
            if (phys == root_phys)
                return true; // idempotent re-publish (same root)
            continue;
        }
        __atomic_store_n(&s_active_phys_[idx], root_phys,
                         __ATOMIC_RELAXED);
        // Generation rides along untouched (bumped on every clear):
        // a freed-and-recycled root phys presents a different gen, so
        // a stale probe can never match (ABA closed by the bump).
        __atomic_store_n(&s_active_flag_[idx], 1u, __ATOMIC_RELEASE);
        return true;
    }
    return false; // probe table exhausted (== binding exhaustion)
}

void CarveBinding::probe_clear_locked(uint64_t root_phys) noexcept {
    const size_t base = probe_index(root_phys);
    for (size_t step = 0; step < CONFIG_CARVE_BINDINGS; ++step) {
        const size_t idx =
            (base + step) % CONFIG_CARVE_BINDINGS;
        const uint8_t flag =
            __atomic_load_n(&s_active_flag_[idx], __ATOMIC_RELAXED);
        if (flag == 0)
            continue;
        const uint64_t phys =
            __atomic_load_n(&s_active_phys_[idx], __ATOMIC_RELAXED);
        if (phys != root_phys)
            continue;
        __atomic_store_n(&s_active_flag_[idx], 0u, __ATOMIC_RELEASE);
        const uint32_t gen =
            __atomic_load_n(&s_active_gen_[idx], __ATOMIC_RELAXED);
        __atomic_store_n(&s_active_gen_[idx], gen + 1u,
                         __ATOMIC_RELAXED);
        return;
    }
}

int CarveBinding::begin(uint64_t pid, uint32_t gen) noexcept {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    for (size_t slot_idx = 0; slot_idx < CONFIG_CARVE_BINDINGS;
         ++slot_idx) {
        if (s_slots_[slot_idx].state != State::FREE &&
            s_slots_[slot_idx].pid == pid &&
            s_slots_[slot_idx].gen == gen)
            return -1; // already bound — fail closed (double-begin)
    }
    for (size_t slot_idx = 0; slot_idx < CONFIG_CARVE_BINDINGS;
         ++slot_idx) {
        Slot &slot = s_slots_[slot_idx];
        if (slot.state != State::FREE)
            continue;
        slot.pid = pid;
        slot.gen = gen;
        slot.tcb_cap = nullptr;
        slot.root_cap = nullptr;
        slot.frame_count = 0;
        slot.state = State::COMPOSING;
        return static_cast<int>(slot_idx);
    }
    return -1; // registry full — fail closed
}

bool CarveBinding::bind_tcb_cap(int slot, CarvedObjectCap *cap) noexcept {
    if (cap == nullptr)
        return false;
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    if (slot < 0 ||
        static_cast<size_t>(slot) >= CONFIG_CARVE_BINDINGS)
        return false;
    Slot &entry = s_slots_[static_cast<size_t>(slot)];
    if (entry.state != State::COMPOSING || entry.tcb_cap != nullptr)
        return false;
    if (!cap->acquire())
        return false; // revoked meanwhile — fail closed
    entry.tcb_cap = cap;
    return true;
}

bool CarveBinding::bind_root(int slot, CarvedObjectCap *cap) noexcept {
    if (cap == nullptr)
        return false;
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    if (slot < 0 ||
        static_cast<size_t>(slot) >= CONFIG_CARVE_BINDINGS)
        return false;
    Slot &entry = s_slots_[static_cast<size_t>(slot)];
    if (entry.state != State::COMPOSING || entry.root_cap != nullptr)
        return false;
    if (!cap->acquire())
        return false; // revoked meanwhile — fail closed
    entry.root_cap = cap;
    return true;
}

bool CarveBinding::bind_frame(int slot, FrameCap *cap) noexcept {
    if (cap == nullptr)
        return false;
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    if (slot < 0 ||
        static_cast<size_t>(slot) >= CONFIG_CARVE_BINDINGS)
        return false;
    Slot &entry = s_slots_[static_cast<size_t>(slot)];
    if (entry.state != State::COMPOSING)
        return false;
    if (entry.frame_count >= CONFIG_CARVE_MAX_REGIONS)
        return false; // per-child frame bound — fail closed
    if (!cap->acquire())
        return false; // revoked meanwhile — fail closed
    entry.frames[entry.frame_count] = cap;
    ++entry.frame_count;
    return true;
}

bool CarveBinding::bind_table(int slot, CarvedObjectCap *cap) noexcept {
    if (cap == nullptr)
        return false;
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    if (slot < 0 ||
        static_cast<size_t>(slot) >= CONFIG_CARVE_BINDINGS)
        return false;
    Slot &entry = s_slots_[static_cast<size_t>(slot)];
    if (entry.state != State::COMPOSING)
        return false;
    if (entry.table_count >= CONFIG_CARVE_MAX_TABLES)
        return false; // per-child table bound — fail closed
    if (!cap->acquire())
        return false; // revoked meanwhile — fail closed
    entry.tables[entry.table_count] = cap;
    ++entry.table_count;
    return true;
}

void CarveBinding::abandon(int slot) noexcept {
    CarvedObjectCap *tcb_pin = nullptr;
    CarvedObjectCap *root_pin = nullptr;
    FrameCap *frame_pins[CONFIG_CARVE_MAX_REGIONS] = {};
    CarvedObjectCap *table_pins[CONFIG_CARVE_MAX_TABLES] = {};
    uint8_t frame_total = 0;
    uint8_t table_total = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        if (slot < 0 ||
            static_cast<size_t>(slot) >= CONFIG_CARVE_BINDINGS)
            return;
        Slot &entry = s_slots_[static_cast<size_t>(slot)];
        if (entry.state != State::COMPOSING)
            return; // ACTIVE slots drain only (never abandoned)
        tcb_pin = entry.tcb_cap;
        root_pin = entry.root_cap;
        for (uint8_t frame_idx = 0; frame_idx < entry.frame_count;
             ++frame_idx)
            frame_pins[frame_idx] = entry.frames[frame_idx];
        frame_total = entry.frame_count;
        for (uint8_t table_idx = 0; table_idx < entry.table_count;
             ++table_idx)
            table_pins[table_idx] = entry.tables[table_idx];
        table_total = entry.table_count;
        entry.tcb_cap = nullptr;
        entry.root_cap = nullptr;
        entry.frame_count = 0;
        entry.table_count = 0;
        entry.pid = 0;
        entry.gen = 0;
        entry.state = State::FREE;
    }
    // Pins released OUTSIDE the lock: the last release may run dispose()
    // -> PMM::free / MemPool::free (and dispose calls invalidate_table,
    // which takes VSpaceMap s_lock_ — never hold the binding leaf across
    // it, INV-CARVE-7).
    if (tcb_pin != nullptr)
        tcb_pin->release();
    if (root_pin != nullptr)
        root_pin->release();
    for (uint8_t frame_idx = 0; frame_idx < frame_total; ++frame_idx) {
        if (frame_pins[frame_idx] != nullptr)
            frame_pins[frame_idx]->release();
    }
    for (uint8_t table_idx = 0; table_idx < table_total; ++table_idx) {
        if (table_pins[table_idx] != nullptr)
            table_pins[table_idx]->release();
    }
}

namespace {

// Top-down subtree check (pure walk, no binding lock): every table child
// must be registry-known (is_linked), leaves/blocks are legal only at the
// leaf level, and the scan covers the user half only — the merged kernel
// half is shared (never linked) and skipped by construction.  A
// user-marked kernel-half entry is unconstructible through link() (which
// refuses KERNEL_HALF) and frame_map (which refuses kernel frames), so no
// portable USER-bit test is needed here.
errors::VspaceError check_table(uint64_t phys, unsigned depth,
                                unsigned levels) noexcept {
    if (depth >= levels)
        return errors::VSPACE_ERR_OK; // unreachable guard
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const auto *table = reinterpret_cast<const uint64_t *>(
        arch::HHDM_OFFSET + phys);
    const size_t span = (depth == 0)
                            ? arch::PML4_USER_COUNT
                            : arch::ArchPageTable::ENTRIES;
    const bool last_level = (depth == levels - 1u);
    for (size_t entry_idx = 0; entry_idx < span; ++entry_idx) {
        const uint64_t entry = table[entry_idx];
        if (entry == 0)
            continue;
        // Level-aware table test (aarch64 page descriptors read as
        // tables under the agnostic codec — the level gates them).
        if (VMM::table_entry_is_table_at(entry, depth)) {
            if (last_level)
                return errors::VSPACE_ERR_SUBTREE_INVALID;
            const uint64_t child = VMM::table_entry_child(entry);
            if (!VSpaceMap::is_linked(child))
                return errors::VSPACE_ERR_SUBTREE_INVALID;
            const errors::VspaceError sub =
                check_table(child, depth + 1u, levels);
            if (sub != errors::VSPACE_ERR_OK)
                return sub;
        } else if (!last_level) {
            return errors::VSPACE_ERR_SUBTREE_INVALID;
        }
    }
    if (depth == 0) {
        // Kernel-half linkage audit: no carved table may hang under the
        // merged half (link() refuses KERNEL_HALF at construction; this
        // is the activation-time backstop).
        for (size_t entry_idx = arch::PML4_USER_COUNT;
             entry_idx < arch::ArchPageTable::ENTRIES; ++entry_idx) {
            const uint64_t entry = table[entry_idx];
            if (entry == 0)
                continue;
            if (VMM::table_entry_is_table_at(entry, depth) &&
                VSpaceMap::is_linked(VMM::table_entry_child(entry)))
                return errors::VSPACE_ERR_SUBTREE_INVALID;
        }
    }
    return errors::VSPACE_ERR_OK;
}

} // namespace

errors::VspaceError
CarveBinding::validate_subtree(uint64_t root_phys) noexcept {
    if (root_phys == 0 ||
        (root_phys & (arch::PAGE_SIZE - 1u)) != 0)
        return errors::VSPACE_ERR_SUBTREE_INVALID;
    if (root_phys == VMM::current_root_phys())
        return errors::VSPACE_ERR_SUBTREE_INVALID;
    const unsigned levels = VMM::page_table_levels();
    if (levels == 0)
        return errors::VSPACE_ERR_SUBTREE_INVALID;
    return check_table(root_phys, 0, levels);
}

errors::VspaceError CarveBinding::activate_err(int slot,
                                               uint64_t root_phys) noexcept {
    if (root_phys == 0 ||
        (root_phys & (arch::PAGE_SIZE - 1u)) != 0)
        return errors::VSPACE_ERR_TARGET_REJECTED;
    // Issue #564 audit (Stage C code r1, S2): refuse an already-activated
    // root here as well (defence in depth behind the syscall pre-check):
    // probe_publish_locked is idempotent for a live root, so without this
    // bar one root could activate onto two unpublished targets and alias
    // them onto a single address space.  Fresh composer roots are never
    // active, so no legitimate flow changes face.
    if (carve_root_is_activated(root_phys))
        return errors::VSPACE_ERR_ACTIVATED;
    // Re-validate the whole subtree top-down before install: post-
    // activation the tree is immutable (link() refuses ACTIVATED roots),
    // so this snapshot is the activation-time guarantee (M14 handoff).
    const errors::VspaceError valid = validate_subtree(root_phys);
    if (valid != errors::VSPACE_ERR_OK)
        return valid;
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    if (slot < 0 ||
        static_cast<size_t>(slot) >= CONFIG_CARVE_BINDINGS)
        return errors::VSPACE_ERR_TARGET_REJECTED;
    Slot &entry = s_slots_[static_cast<size_t>(slot)];
    if (entry.state != State::COMPOSING)
        return errors::VSPACE_ERR_TARGET_REJECTED;
    if (entry.root_cap == nullptr ||
        entry.root_cap->phys != root_phys)
        return errors::VSPACE_ERR_TARGET_REJECTED;
    if (!probe_publish_locked(root_phys))
        return errors::VSPACE_ERR_REGISTRY_FULL;
    entry.state = State::ACTIVE;
    return errors::VSPACE_ERR_OK;
}

bool CarveBinding::activate(int slot, uint64_t root_phys) noexcept {
    return activate_err(slot, root_phys) == errors::VSPACE_ERR_OK;
}

void CarveBinding::drain_task(uint64_t pid, uint32_t gen) noexcept {
    CarvedObjectCap *tcb_pin = nullptr;
    CarvedObjectCap *root_pin = nullptr;
    FrameCap *frame_pins[CONFIG_CARVE_MAX_REGIONS] = {};
    CarvedObjectCap *table_pins[CONFIG_CARVE_MAX_TABLES] = {};
    uint8_t frame_total = 0;
    uint8_t table_total = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        Slot *entry = nullptr;
        for (size_t slot_idx = 0; slot_idx < CONFIG_CARVE_BINDINGS;
             ++slot_idx) {
            Slot &candidate = s_slots_[slot_idx];
            if (candidate.state != State::FREE &&
                candidate.pid == pid && candidate.gen == gen) {
                entry = &candidate;
                break;
            }
        }
        if (entry == nullptr)
            return; // idempotent replay (generation/state mismatch)
        entry->state = State::DRAINING;
        if (entry->root_cap != nullptr)
            probe_clear_locked(entry->root_cap->phys);
        tcb_pin = entry->tcb_cap;
        root_pin = entry->root_cap;
        for (uint8_t frame_idx = 0; frame_idx < entry->frame_count;
             ++frame_idx)
            frame_pins[frame_idx] = entry->frames[frame_idx];
        frame_total = entry->frame_count;
        for (uint8_t table_idx = 0; table_idx < entry->table_count;
             ++table_idx)
            table_pins[table_idx] = entry->tables[table_idx];
        table_total = entry->table_count;
        entry->tcb_cap = nullptr;
        entry->root_cap = nullptr;
        entry->frame_count = 0;
        entry->table_count = 0;
    }
    // Pins released OUTSIDE the lock (INV-CARVE-7: a pin may dispose ->
    // PMM/MemPool, and dispose takes VSpaceMap s_lock_).
    if (tcb_pin != nullptr)
        tcb_pin->release();
    if (root_pin != nullptr)
        root_pin->release();
    for (uint8_t frame_idx = 0; frame_idx < frame_total; ++frame_idx) {
        if (frame_pins[frame_idx] != nullptr)
            frame_pins[frame_idx]->release();
    }
    for (uint8_t table_idx = 0; table_idx < table_total; ++table_idx) {
        if (table_pins[table_idx] != nullptr)
            table_pins[table_idx]->release();
    }
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t slot_idx = 0; slot_idx < CONFIG_CARVE_BINDINGS;
             ++slot_idx) {
            Slot &candidate = s_slots_[slot_idx];
            if (candidate.state == State::DRAINING &&
                candidate.pid == pid && candidate.gen == gen) {
                candidate.pid = 0;
                candidate.gen = 0;
                candidate.state = State::FREE;
                break;
            }
        }
    }
}

void CarveBinding::deactivate(uint64_t pid, uint32_t gen) noexcept {
    uint64_t root_phys = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t slot_idx = 0; slot_idx < CONFIG_CARVE_BINDINGS;
             ++slot_idx) {
            const Slot &candidate = s_slots_[slot_idx];
            if (candidate.state != State::FREE &&
                candidate.pid == pid && candidate.gen == gen &&
                candidate.root_cap != nullptr) {
                root_phys = candidate.root_cap->phys;
                probe_clear_locked(root_phys);
                break;
            }
        }
    }
    if (root_phys == 0)
        return; // nothing activated (bare slot or TCB-only binding)
    // Full shootdown after the unlock (no ASID narrowing — no ASID is
    // assigned to a carved root).  Never held across the flush.
    VMM::tlb_shootdown(0, 0, true);
}

void CarveBinding::snapshot_reset() noexcept {
    CarvedObjectCap *tcb_pins[CONFIG_CARVE_BINDINGS] = {};
    CarvedObjectCap *root_pins[CONFIG_CARVE_BINDINGS] = {};
    FrameCap *frame_pins[CONFIG_CARVE_BINDINGS * CONFIG_CARVE_MAX_REGIONS] =
        {};
    CarvedObjectCap *table_pins[CONFIG_CARVE_BINDINGS *
                                CONFIG_CARVE_MAX_TABLES] = {};
    size_t tcb_total = 0;
    size_t root_total = 0;
    size_t frame_total = 0;
    size_t table_total = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t slot_idx = 0; slot_idx < CONFIG_CARVE_BINDINGS;
             ++slot_idx) {
            Slot &entry = s_slots_[slot_idx];
            if (entry.state == State::FREE)
                continue;
            if (entry.tcb_cap != nullptr)
                tcb_pins[tcb_total++] = entry.tcb_cap;
            if (entry.root_cap != nullptr)
                root_pins[root_total++] = entry.root_cap;
            for (uint8_t frame_idx = 0; frame_idx < entry.frame_count;
                 ++frame_idx)
                frame_pins[frame_total++] = entry.frames[frame_idx];
            for (uint8_t table_idx = 0; table_idx < entry.table_count;
                 ++table_idx)
                table_pins[table_total++] = entry.tables[table_idx];
            entry.tcb_cap = nullptr;
            entry.root_cap = nullptr;
            entry.frame_count = 0;
            entry.table_count = 0;
            entry.pid = 0;
            entry.gen = 0;
            entry.state = State::FREE;
        }
        for (size_t probe_idx = 0; probe_idx < CONFIG_CARVE_BINDINGS;
             ++probe_idx) {
            __atomic_store_n(&s_active_flag_[probe_idx], 0u,
                             __ATOMIC_RELAXED);
            __atomic_store_n(&s_active_phys_[probe_idx], 0u,
                             __ATOMIC_RELAXED);
        }
    }
    // Pins released OUTSIDE the lock (same dispose-takes-s_lock_ reason
    // as drain_task; the #320 detector note: a well-formed release
    // observes refcount >= 2 — the MemPool baseline rewind paired with
    // this reset must not over-release a restored CSpace reference).
    for (size_t pin_idx = 0; pin_idx < tcb_total; ++pin_idx) {
        if (tcb_pins[pin_idx] != nullptr)
            tcb_pins[pin_idx]->release();
    }
    for (size_t pin_idx = 0; pin_idx < root_total; ++pin_idx) {
        if (root_pins[pin_idx] != nullptr)
            root_pins[pin_idx]->release();
    }
    for (size_t pin_idx = 0; pin_idx < frame_total; ++pin_idx) {
        if (frame_pins[pin_idx] != nullptr)
            frame_pins[pin_idx]->release();
    }
    for (size_t pin_idx = 0; pin_idx < table_total; ++pin_idx) {
        if (table_pins[pin_idx] != nullptr)
            table_pins[pin_idx]->release();
    }
}

size_t CarveBinding::live_count() noexcept {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    size_t live = 0;
    for (size_t slot_idx = 0; slot_idx < CONFIG_CARVE_BINDINGS;
         ++slot_idx) {
        if (s_slots_[slot_idx].state != State::FREE)
            ++live;
    }
    return live;
}

bool carve_root_is_activated(uint64_t root_phys) noexcept {
    if (root_phys == 0)
        return false;
    // S3-2: one index function shared with the writers; flag-phys-gen
    // with acquire ordering, mismatch retries the slot; bounded scan.
    const size_t base = CarveBinding::probe_index(root_phys);
    for (size_t step = 0; step < CONFIG_CARVE_BINDINGS; ++step) {
        const size_t idx =
            (base + step) % CONFIG_CARVE_BINDINGS;
        const uint8_t first =
            __atomic_load_n(&CarveBinding::s_active_flag_[idx],
                            __ATOMIC_ACQUIRE);
        if (first == 0)
            continue;
        const uint64_t phys =
            __atomic_load_n(&CarveBinding::s_active_phys_[idx],
                            __ATOMIC_ACQUIRE);
        const uint32_t gen =
            __atomic_load_n(&CarveBinding::s_active_gen_[idx],
                            __ATOMIC_ACQUIRE);
        (void)gen; // torn-read witness (mismatch detected via flags)
        const uint8_t second =
            __atomic_load_n(&CarveBinding::s_active_flag_[idx],
                            __ATOMIC_ACQUIRE);
        if (first != second || second == 0)
            continue; // concurrent publish/clear — retry the slot
        if (phys == root_phys)
            return true;
    }
    return false;
}

void carve_note_activated(uint64_t root_phys) noexcept {
    if (root_phys == 0)
        return;
    SpinLockGuard<sync::SpinLock> guard(CarveBinding::s_lock_);
    for (size_t slot_idx = 0; slot_idx < CONFIG_CARVE_BINDINGS;
         ++slot_idx) {
        const CarveBinding::Slot &entry =
            CarveBinding::s_slots_[slot_idx];
        if (entry.state != CarveBinding::State::FREE &&
            entry.root_cap != nullptr &&
            entry.root_cap->phys == root_phys) {
            CarveBinding::probe_publish_locked(root_phys);
            return;
        }
    }
}

void carve_note_deactivated(uint64_t root_phys) noexcept {
    if (root_phys == 0)
        return;
    SpinLockGuard<sync::SpinLock> guard(CarveBinding::s_lock_);
    CarveBinding::probe_clear_locked(root_phys);
}

} // namespace kernel::cap
