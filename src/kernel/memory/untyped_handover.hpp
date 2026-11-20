/// @file kernel/memory/untyped_handover.hpp
/// @brief Mem-mgr bootstrap handover registry (M4, #563/F10).
///
/// After PMM::freeze_pools() runs at end of composition, boot_handover()
/// mints one UntypedMem per handed-over run into the root task's CSpace.
/// The mem-mgr PD later takes ownership via transfer_all_to() (M5 seam).
/// Fixed-capacity registry: no allocation, ever.
#pragma once

#include <kernel/memory/mem_handover_errors.hpp>
#include <types.hpp>

namespace kernel {

struct TaskControlBlock;

namespace cap {
class CNode;
class UntypedMem;
} // namespace cap

namespace untyped_handover {

/// @brief One handed-over run with its minted cap and slot identity.
struct HandoverRun {
    uint64_t phys = 0;            ///< run base (PAGE_SIZE multiple)
    uint64_t pages = 0;           ///< run length in pages
    cap::UntypedMem *obj = nullptr; ///< minted cap (slot-owned after install)
    uint32_t slot = 0;            ///< root-CNode slot holding the cap
    uint32_t gen = 0;             ///< slot generation at install time
};

/// @brief Handover result snapshot (M4 gate asserts + meminfo rows).
struct HandoverStatus {
    bool frozen = false;
    uint32_t runs_total = 0;   ///< handed-over runs recorded
    uint32_t runs_capped = 0;  ///< runs dropped by the MAX_RUNS cap
    uint64_t pages_handed = 0; ///< sum of handed run sizes
    uint64_t pages_retained = 0; ///< free-but-kernel-retained pages
    uint32_t caps_minted = 0;  ///< UntypedMem caps installed
    errors::MemHandoverError result = errors::MEM_HANDOVER_ERR_OK;
};

/// @brief Mint one UntypedMem per PMM handover run into @p root's CSpace.
///        All-or-nothing (INV-F10-1): a capacity pre-check (Untyped
///        headroom + root-CSpace free slots) refuses with BOUNDS before
///        anything mints, and any mid-loop failure unwinds every install
///        in reverse, so the registry is either fully installed or empty —
///        a retry can never double-mint a region.
///        Idempotent guard: a second call returns ALREADY_FROZEN without
///        touching caps. Runs with zero PMM handover runs return
///        NO_FREE_RUNS. Every failure path is typed; no asserts, no panic.
errors::MemHandoverError boot_handover(TaskControlBlock &root);

/// @brief M5 seam: move every handed-over cap into @p dst's CSpace.
///        Two-phase (verify all slots+generations first, then move), so a
///        stale slot aborts with STALE_SLOT before anything moves.
///        A destination-capacity pre-check refuses with CSPACE_FULL before
///        phase 2 when @p dst cannot hold all runs.
///        Refuses dst == source (BAD_TARGET) and a second transfer
///        (ALREADY_TRANSFERRED). Unused in M4 by design.
errors::MemHandoverError transfer_all_to(TaskControlBlock &dst);

/// @brief Current handover status (gate asserts, meminfo rows).
HandoverStatus status();

/// @brief Handover run @p idx, or nullptr when out of range.
const HandoverRun *region(uint32_t idx);

} // namespace untyped_handover
} // namespace kernel
