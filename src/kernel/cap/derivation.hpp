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

/// @file derivation.hpp
/// @brief Bounded derivation table (F2, issue #555): value-only edges
///        linking a derived cap slot to the source slot it was copied,
///        granted, minted or retyped from.  CAP_REVOKE (53) walks these
///        edges to destroy derivations; the named cap survives.
/// @details INV-DERIV-2: edges hold value data only (CNode serial + slot
///        + generation triples) — never pointers, never references — so
///        no table entry can dangle or pin a stack CNode past its frame.
///        INV-DERIV-1: the table's lock is Leaf-ranked (innermost) and is
///        never held together with any CNode slot lock; every derivation
///        mutation runs with no CNode slot lock held.  INV-DERIV-7: the
///        table is static bss (no MemPool/PMM use, no ResourceTracker
///        counters).  Availability (step 15): kCapMaxDerivations = 256 is
///        a new unmeasured number (Q7 lists no derivation bound); the
///        rationale is capacity >= 4x a full 64-slot destination fill,
///        re-homed to F9/#562 per Q3.  The table is shared across all
///        CNodes, so a busy granter can make previously working grants
///        fail closed with CAP_ERR_DERIVATION_FULL (never a silent
///        untracked derivation); used() plus the CONFIG_DEBUG high-water
///        mark are the observability hook and the F9 sizing input.  A
///        revoke itself always completes in bounded steps and cannot fail
///        for capacity.

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace kernel::cap {

class CNode;

/// @brief Maximum derivation edges tracked kernel-wide (step 15: shared
///        across all CNodes; >= 4x a full CONFIG_CSLOT_COUNT fill).
inline constexpr uint32_t kCapMaxDerivations = 256;

/// @brief Per-call edge budget for revoke_derivations (step 11): a revoke
///        that exceeds the budget returns CAP_ERR_REVOKE_INCOMPLETE and is
///        resumable by re-invocation (takes are destructive, so progress
///        is monotone).  Rationale (Q4): 8x the maximum depth chain.
inline constexpr uint32_t kRevokeEdgeBudget = 64;

/// @brief Hard bound on revoke_derivations loop iterations (2B+2 would
///        suffice; the guard carries slack so hitting it proves a
///        corrupt table, reported as CAP_ERR_DERIVATION_CYCLE).
inline constexpr uint32_t kRevokeMaxSteps = 2u * kRevokeEdgeBudget + 4u;

/// @brief Sentinel child-node slot: the derived slot lives in the same
///        CNode as its parent (the retype self-edge, untyped.cpp).
inline constexpr uint32_t kDerivSelfNode = 0xFFFFFFFFu;

static_assert(kCapMaxDerivations >= 4u * CONFIG_CSLOT_COUNT,
              "derivation table must cover 4x a full CNode fill");
static_assert(CONFIG_CAP_MAX_DEPTH >= 1, "derivation depth bound degenerate");
static_assert(kRevokeEdgeBudget >= CONFIG_CAP_MAX_DEPTH + 1u,
              "revoke budget must cover one full depth chain per call");

/// @brief One derivation edge: value data only (INV-DERIV-2).  No
///        pointers, no references; safe to hold across calls.
struct DerivationEdge {
    uint32_t parent_serial = 0; ///< serial of the parent (source) CNode
    uint32_t parent_slot = 0;   ///< source slot the derivation came from
    uint32_t parent_gen = 0;    ///< source slot generation at record time
    uint32_t child_node_slot = 0; ///< slot naming the child CNode (or
                                  ///< kDerivSelfNode for a self edge)
    uint32_t child_node_gen = 0;  ///< generation of that naming slot
    uint32_t child_slot = 0;      ///< derived slot inside the child CNode
    uint32_t child_gen = 0;       ///< derived slot generation at record time
    uint8_t depth = 0;            ///< derivation depth (1 = direct child)
    bool occupied = false;
};

/// @brief Bounded derivation-edge registry (Meyers singleton, shared by
///        every CNode).  All scans are linear and bounded; no allocation,
///        no recursion, no unbounded loops.  Locking (INV-DERIV-1): every
///        method takes only the Leaf-ranked table lock; callers must hold
///        no CNode slot lock.
class DerivationTable {
  public:
    /// @brief Returns the single table instance.
    static DerivationTable &instance() noexcept;

    /// @brief Records a derivation edge.  Rejects depth >
    ///        CONFIG_CAP_MAX_DEPTH before any mutation.  On a full table
    ///        runs one stale-GC pass and retries once, else returns false
    ///        (caller fails closed with CAP_ERR_DERIVATION_FULL).
    /// @return true when the edge is stored.
    bool record(CNode &parent, uint32_t parent_slot, uint32_t parent_gen,
                CNode &child_node, uint32_t child_node_slot,
                uint32_t child_node_gen, uint32_t child_slot,
                uint32_t child_gen, uint8_t depth) noexcept;

    /// @brief Removes and returns one edge parented at (parent, slot,
    ///        gen), lowest depth first.  The take is destructive, so a
    ///        revoke that aborts afterwards (INCOMPLETE) never loses an
    ///        edge it has not processed (INV-DERIV-11).
    /// @return true with @p out filled, false when no child edge remains.
    bool take_child(CNode &parent, uint32_t parent_slot, uint32_t parent_gen,
                    DerivationEdge &out) noexcept;

    /// @brief Read-only probe: true when at least one edge is parented at
    ///        (parent, slot, gen).  Mutates nothing in the table (lets a
    ///        revoke with an exhausted budget report INCOMPLETE without a
    ///        spurious take).
    bool has_child(CNode &parent, uint32_t parent_slot,
                   uint32_t parent_gen) noexcept;

    /// @brief Returns the depth of (node, slot, gen) as a derivation
    ///        (deepest matching edge), or 0 when the slot is not a
    ///        recorded derivation.  Creation paths add one to bound the
    ///        new edge (INV-DERIV-3).
    uint8_t child_depth(CNode &node, uint32_t slot, uint32_t gen) noexcept;

    /// @brief Prunes every entry provably stale after (node, slot) was
    ///        removed: edges parented at the slot (INV-DERIV-9 corollary —
    ///        under post-order only already-consumed edges can match) and
    ///        self-edges whose child endpoint was the removed slot (child
    ///        destroyed, so the edge can never destroy anything again).
    ///        Non-self child endpoints live in another CNode and are left
    ///        alone; their staleness fails closed via generations.
    void forget_slot(CNode &node, uint32_t slot) noexcept;

    /// @brief Prunes every entry parented in @p node (whole-CNode
    ///        teardown; the parent endpoints are all dead, so no live
    ///        walk can take them).
    void forget_node(CNode &node) noexcept;

    /// @brief Assigns (on first use, monotonically) and returns the
    ///        table identity of @p node.  Never 0 for a recorded node.
    uint32_t node_serial(CNode &node) noexcept;

    /// @brief Number of occupied entries (diagnostics, step 15).
    uint32_t used() noexcept;

    /// @brief Clears the table and the debug high-water mark.  Called
    ///        from test isolation before MemPool::restore_pool_data
    ///        (INV-DERIV-6); never on a production path.
    void snapshot_reset() noexcept;

  private:
    DerivationTable() noexcept = default;
};

/// @brief Production-TU wrapper for the QE isolation call site
///        (test_isolate.cpp, INV-DERIV-6).
void derivation_snapshot_reset() noexcept;

} // namespace kernel::cap
