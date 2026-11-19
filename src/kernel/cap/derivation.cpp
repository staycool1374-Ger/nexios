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

/// @file derivation.cpp
/// @brief Bounded derivation-table storage and primitives (F2, #555).
///        Static bss, one Leaf-ranked SpinLock, monotonic CNode serial
///        allocator, GC-on-full, no allocation, no ResourceTracker use.

#include <kernel/cap/derivation.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>

namespace kernel::cap {

namespace {

/// @brief The edge store: static bss, never resized (INV-DERIV-7).
DerivationEdge g_table[kCapMaxDerivations];

/// @brief Table lock: Leaf (rank 3, innermost).  Never held together
///        with any CNode slot lock (INV-DERIV-1); the M2 check reports any
///        future nesting instead of passing silently.
sync::SpinLock g_derivation_lock{sync::LockRank::Leaf};

/// @brief Next CNode identity.  0 is never handed out (unassigned mark);
///        wrap is unreachable in a boot and is guarded defensively.
uint32_t g_next_node_serial = 1;

#if defined(CONFIG_DEBUG)
/// @brief Debug-only high-water mark for the F9 sizing input (step 15).
///        Read-only observability; cleared by snapshot_reset().
uint32_t g_high_water = 0;
#endif

/// @brief Conservative value-only liveness predicate (INV-DERIV-2: no
///        dereferences).  Only depth-corrupt entries are provably stale
///        from values alone; true staleness is closed by the
///        forget_slot/forget_node hooks and snapshot_reset, and every
///        take-time use is gen-checked (remove_if), so a kept-stale edge
///        can only fail closed.
bool edge_still_live(const DerivationEdge &edge) noexcept {
    return edge.depth >= 1 &&
           edge.depth <= static_cast<uint8_t>(CONFIG_CAP_MAX_DEPTH);
}

} // namespace

DerivationTable &DerivationTable::instance() noexcept {
    static DerivationTable table{};
    return table;
}

uint32_t DerivationTable::node_serial(CNode &node) noexcept {
    SpinLockGuard<sync::SpinLock> guard(g_derivation_lock);
    if (node.serial_ == 0) {
        node.serial_ = g_next_node_serial++;
        if (g_next_node_serial == 0)
            g_next_node_serial = 1; // never hand out the unassigned mark
    }
    return node.serial_;
}

bool DerivationTable::record(CNode &parent, uint32_t parent_slot,
                             uint32_t parent_gen, CNode &child_node,
                             uint32_t child_node_slot,
                             uint32_t child_node_gen, uint32_t child_slot,
                             uint32_t child_gen, uint8_t depth) noexcept {
    if (depth > static_cast<uint8_t>(CONFIG_CAP_MAX_DEPTH))
        return false; // creation-enforced bound (INV-DERIV-3), no mutation
    const uint32_t parent_serial = node_serial(parent);
    const uint32_t child_serial = node_serial(child_node);
    (void)child_serial; // identity established; the edge keys on the
                        // naming-slot triple (value-only, INV-DERIV-2)
    SpinLockGuard<sync::SpinLock> guard(g_derivation_lock);
    for (uint32_t pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < kCapMaxDerivations; ++i) {
            if (!g_table[i].occupied) {
                g_table[i].parent_serial = parent_serial;
                g_table[i].parent_slot = parent_slot;
                g_table[i].parent_gen = parent_gen;
                g_table[i].child_node_slot = child_node_slot;
                g_table[i].child_node_gen = child_node_gen;
                g_table[i].child_slot = child_slot;
                g_table[i].child_gen = child_gen;
                g_table[i].depth = depth;
                g_table[i].occupied = true;
#if defined(CONFIG_DEBUG)
                uint32_t used_now = 0;
                for (uint32_t k = 0; k < kCapMaxDerivations; ++k)
                    used_now += g_table[k].occupied ? 1u : 0u;
                if (used_now > g_high_water)
                    g_high_water = used_now;
#endif
                return true;
            }
        }
        if (pass == 0) {
            // One stale-GC pass over provably dead entries, then retry.
            for (uint32_t i = 0; i < kCapMaxDerivations; ++i) {
                if (g_table[i].occupied && !edge_still_live(g_table[i]))
                    g_table[i].occupied = false;
            }
        }
    }
    return false;
}

bool DerivationTable::take_child(CNode &parent, uint32_t parent_slot,
                                 uint32_t parent_gen,
                                 DerivationEdge &out) noexcept {
    const uint32_t parent_serial = node_serial(parent);
    SpinLockGuard<sync::SpinLock> guard(g_derivation_lock);
    uint32_t best = kCapMaxDerivations;
    for (uint32_t i = 0; i < kCapMaxDerivations; ++i) {
        if (!g_table[i].occupied ||
            g_table[i].parent_serial != parent_serial ||
            g_table[i].parent_slot != parent_slot ||
            g_table[i].parent_gen != parent_gen)
            continue;
        if (best == kCapMaxDerivations || g_table[i].depth < g_table[best].depth)
            best = i;
    }
    if (best == kCapMaxDerivations)
        return false;
    out = g_table[best];
    g_table[best].occupied = false;
    return true;
}

bool DerivationTable::has_child(CNode &parent, uint32_t parent_slot,
                                uint32_t parent_gen) noexcept {
    const uint32_t parent_serial = node_serial(parent);
    SpinLockGuard<sync::SpinLock> guard(g_derivation_lock);
    for (uint32_t i = 0; i < kCapMaxDerivations; ++i) {
        if (g_table[i].occupied &&
            g_table[i].parent_serial == parent_serial &&
            g_table[i].parent_slot == parent_slot &&
            g_table[i].parent_gen == parent_gen)
            return true;
    }
    return false;
}

uint8_t DerivationTable::child_depth(CNode &node, uint32_t slot,
                                     uint32_t gen) noexcept {
    const uint32_t serial = node_serial(node);
    SpinLockGuard<sync::SpinLock> guard(g_derivation_lock);
    uint8_t depth = 0;
    for (uint32_t i = 0; i < kCapMaxDerivations; ++i) {
        if (!g_table[i].occupied)
            continue;
        // Self edge: the child endpoint is fully identified (same node).
        // Non-self edge: the child CNode is named only by a slot triple
        // in the parent, so the node itself is unprovable from values
        // alone — match on the (slot, gen) pair.  This can only
        // overestimate depth across unrelated CNodes (coincidental
        // slot/gen reuse), which fails closed at creation
        // (CAP_ERR_DEPTH_EXCEEDED, deny-never-degrade) and can never
        // undercut the INV-DERIV-3 bound.
        const bool as_child =
            (g_table[i].child_node_slot == kDerivSelfNode &&
             g_table[i].parent_serial == serial &&
             g_table[i].child_slot == slot && g_table[i].child_gen == gen) ||
            (g_table[i].child_node_slot != kDerivSelfNode &&
             g_table[i].child_slot == slot && g_table[i].child_gen == gen);
        if (as_child && g_table[i].depth > depth)
            depth = g_table[i].depth;
    }
    return depth;
}

void DerivationTable::forget_slot(CNode &node, uint32_t slot) noexcept {
    // Serial taken before the table lock (INV-DERIV-1); a never-recorded
    // node has serial 0 and owns no edge, so there is nothing to prune.
    const uint32_t serial = node_serial(node);
    if (serial == 0)
        return;
    SpinLockGuard<sync::SpinLock> guard(g_derivation_lock);
    for (uint32_t i = 0; i < kCapMaxDerivations; ++i) {
        if (!g_table[i].occupied)
            continue;
        // Parent side: the source slot is gone, so under post-order every
        // matching edge is already consumed (INV-DERIV-9 corollary).
        const bool dead_parent = (g_table[i].parent_serial == serial &&
                                  g_table[i].parent_slot == slot);
        // Self-edge child side: the derived slot itself was destroyed, so
        // the edge is provably stale (remove_if on it could never fire).
        // Non-self child endpoints live in another CNode and cannot be
        // proven stale from values alone — they are left for the
        // gen-checked destroy, which fails closed.
        const bool dead_self_child =
            (g_table[i].child_node_slot == kDerivSelfNode &&
             g_table[i].parent_serial == serial &&
             g_table[i].child_slot == slot);
        if (dead_parent || dead_self_child)
            g_table[i].occupied = false;
    }
}

void DerivationTable::forget_node(CNode &node) noexcept {
    const uint32_t serial = node_serial(node);
    if (serial == 0)
        return;
    SpinLockGuard<sync::SpinLock> guard(g_derivation_lock);
    for (uint32_t i = 0; i < kCapMaxDerivations; ++i) {
        if (g_table[i].occupied && g_table[i].parent_serial == serial)
            g_table[i].occupied = false;
    }
}

uint32_t DerivationTable::used() noexcept {
    SpinLockGuard<sync::SpinLock> guard(g_derivation_lock);
    uint32_t count = 0;
    for (uint32_t i = 0; i < kCapMaxDerivations; ++i)
        count += g_table[i].occupied ? 1u : 0u;
    return count;
}

void DerivationTable::snapshot_reset() noexcept {
    SpinLockGuard<sync::SpinLock> guard(g_derivation_lock);
    for (uint32_t i = 0; i < kCapMaxDerivations; ++i)
        g_table[i].occupied = false;
#if defined(CONFIG_DEBUG)
    g_high_water = 0;
#endif
}

void derivation_snapshot_reset() noexcept {
    DerivationTable::instance().snapshot_reset();
}

} // namespace kernel::cap
