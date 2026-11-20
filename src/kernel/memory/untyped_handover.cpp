/// @file kernel/memory/untyped_handover.cpp
/// @brief Mem-mgr bootstrap handover registry (M4, #563/F10).
#include <kernel/memory/untyped_handover.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/untyped.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/memory/mem_pressure.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/nexios_config.h>
#include <kernel/task/task.hpp>
#include <lib/logger.hpp>

namespace kernel {
namespace untyped_handover {

namespace {

HandoverRun g_runs[CONFIG_MEM_HANDOVER_MAX_RUNS] = {};
uint32_t g_run_count = 0;
bool g_done = false;
bool g_transferred = false;
TaskControlBlock *g_source = nullptr;
errors::MemHandoverError g_result = errors::MEM_HANDOVER_ERR_OK;

/// @brief Undo installs 0..g_run_count-1 in reverse slot order, then reset
///        the registry. Zeroes each entry whose remove() dropped the last
///        reference (a dangling pointer after release()->dispose()) so a
///        retry overwriting entries 0..runs-1 can never observe a stale
///        object. g_done stays false: the retry restarts clean.
void rollback_minted(cap::CNode &cs) noexcept {
    for (uint32_t j = g_run_count; j > 0; --j) {
        const uint32_t k = j - 1;
        cs.remove(g_runs[k].slot);
        g_runs[k] = HandoverRun{};
    }
    g_run_count = 0;
    g_source = nullptr;
}

/// @brief Latch + log a failed handover attempt (mid-loop or pre-check).
///        The latch emits the PMM+4 row once; this row carries the typed
///        outcome with the demanded run count as context.
void note_handover_failure(errors::MemHandoverError err,
                            uint32_t runs) noexcept {
    mem_pressure::latch(mem_pressure::MemPressureReason::HANDOVER_CAPPED);
    log::dmesg_push_sev(log::ErrorSubsystem::PMM,
                        log::kDmesgBase_PMM + 4, log::LogSeverity::ERROR,
                        "mem: handover failed", runs);
    g_result = err;
}

} // namespace

errors::MemHandoverError boot_handover(TaskControlBlock &root) {
    if (g_done) {
        return errors::MEM_HANDOVER_ERR_ALREADY_FROZEN;
    }
    const uint32_t runs = PMM::handover_run_count();
    if (runs == 0) {
        g_result = errors::MEM_HANDOVER_ERR_NO_FREE_RUNS;
        return g_result;
    }
    root.ensure_cspace();
    cap::CNode *cs = root.get_cspace();
    if (cs == nullptr) {
        g_result = errors::MEM_HANDOVER_ERR_NO_ROOT;
        return g_result;
    }
    // All-or-nothing pre-check (INV-F10-1): demand is exactly one Untyped
    // + one CSlot per run. Refuse with BOUNDS before anything mints when
    // the CONFIG_CAP_MAX_UNTYPED headroom or the root-CSpace free slots
    // cannot hold all runs — no partial state, so a retry can never
    // double-mint a region. Advisory inputs (live_count, free_slot_count);
    // the live bound and install() stay authoritative at mint time.
    const uint32_t live = cap::UntypedMem::live_count();
    const uint32_t cap_max =
        static_cast<uint32_t>(CONFIG_CAP_MAX_UNTYPED);
    const uint32_t headroom = (live >= cap_max) ? 0U : (cap_max - live);
    const uint32_t free_slots = cs->free_slot_count();
    if (runs > headroom || runs > free_slots) {
        note_handover_failure(errors::MEM_HANDOVER_ERR_BOUNDS, runs);
        return g_result;
    }
    g_source = &root;
    for (uint32_t i = 0; i < runs; ++i) {
        if (g_run_count >= CONFIG_MEM_HANDOVER_MAX_RUNS) {
            // Unreachable: PMM caps handover_run_count at MAX_RUNS, and
            // the pre-check above bounded runs by headroom. Roll back
            // anyway — a partial registry must never survive.
            rollback_minted(*cs);
            note_handover_failure(errors::MEM_HANDOVER_ERR_REGISTRY_FULL,
                                  runs);
            return g_result;
        }
        const uint64_t phys = PMM::handover_run_phys(i);
        const uint64_t pages = PMM::handover_run_pages(i);
        if (phys == 0 || pages == 0) {
            continue;
        }
        cap::UntypedMem *obj = cap::UntypedMem::create_subrange(
            phys, static_cast<size_t>(pages * arch::PAGE_SIZE),
            /*is_user=*/true);
        if (obj == nullptr) {
            // MemPool exhaust or live-bound race: unwind every install so
            // no handed-over frame is marked-but-unminted.
            rollback_minted(*cs);
            note_handover_failure(
                errors::MEM_HANDOVER_ERR_UNTYPED_CAP_FULL, runs);
            return g_result;
        }
        uint32_t gen = 0;
        const int idx =
            cs->install(obj, cap::CapType::Untyped,
                        cap::CapRights::READ | cap::CapRights::WRITE |
                            cap::CapRights::GRANT,
                        &gen);
        if (idx < 0) {
            obj->release(); // never installed — drop the creator reference
            rollback_minted(*cs);
            note_handover_failure(errors::MEM_HANDOVER_ERR_CSPACE_FULL,
                                  runs);
            return g_result;
        }
        obj->release(); // slot holds the reference; drop the creator's
        g_runs[g_run_count].phys = phys;
        g_runs[g_run_count].pages = pages;
        g_runs[g_run_count].obj = obj;
        g_runs[g_run_count].slot = static_cast<uint32_t>(idx);
        g_runs[g_run_count].gen = gen;
        ++g_run_count;
    }
    g_done = true;
    g_result = errors::MEM_HANDOVER_ERR_OK;
    log::dmesg_push_sev(log::ErrorSubsystem::PMM, log::kDmesgBase_PMM + 3,
                        log::LogSeverity::INFO,
                        "mem: handover complete", g_run_count);
    Logger::info("mem: handover %u runs %lu pages installed in root cspace",
                 g_run_count, PMM::handover_pages());
    return errors::MEM_HANDOVER_ERR_OK;
}

errors::MemHandoverError transfer_all_to(TaskControlBlock &dst) {
    if (g_source != nullptr && &dst == g_source) {
        return errors::MEM_HANDOVER_ERR_BAD_TARGET;
    }
    if (g_transferred) {
        return errors::MEM_HANDOVER_ERR_ALREADY_TRANSFERRED;
    }
    if (!g_done || g_run_count == 0) {
        return errors::MEM_HANDOVER_ERR_NO_FREE_RUNS;
    }
    cap::CNode *src = g_source->get_cspace();
    if (src == nullptr) {
        return errors::MEM_HANDOVER_ERR_NO_ROOT;
    }
    dst.ensure_cspace();
    cap::CNode *dcs = dst.get_cspace();
    if (dcs == nullptr) {
        return errors::MEM_HANDOVER_ERR_NO_ROOT;
    }
    // Phase 1 — verify every slot still names its cap with the install
    // generation. Any stale slot aborts before anything moves (no partial
    // transfer, no orphaned references).
    for (uint32_t i = 0; i < g_run_count; ++i) {
        if (src->peek(g_runs[i].slot, cap::CapType::Untyped) !=
                g_runs[i].obj ||
            src->slot_gen(g_runs[i].slot) != g_runs[i].gen) {
            return errors::MEM_HANDOVER_ERR_STALE_SLOT;
        }
    }
    // Destination-capacity pre-check (F10): phase 2 installs exactly
    // g_run_count caps, so refuse up front when the destination CSpace
    // cannot hold them — a partial phase-2 move would strand caps split
    // across two CSpaces with g_transferred == false. M5 follow-up
    // (M5-transfer-atomicity): under live-AP callers a concurrent install
    // between this check and the phase-2 moves can still strand; close it
    // there with a phase-2 rollback or a generation re-verify.
    if (dcs->free_slot_count() < g_run_count) {
        return errors::MEM_HANDOVER_ERR_CSPACE_FULL;
    }
    // Phase 2 — move: install-then-remove keeps the object referenced at
    // every instant (no use-after-free window for a concurrent lookup).
    for (uint32_t i = 0; i < g_run_count; ++i) {
        uint32_t gen = 0;
        const int idx =
            dcs->install(g_runs[i].obj, cap::CapType::Untyped,
                         cap::CapRights::READ | cap::CapRights::WRITE |
                             cap::CapRights::GRANT,
                         &gen);
        if (idx < 0) {
            return errors::MEM_HANDOVER_ERR_CSPACE_FULL;
        }
        src->remove(g_runs[i].slot);
        g_runs[i].slot = static_cast<uint32_t>(idx);
        g_runs[i].gen = gen;
    }
    g_transferred = true;
    return errors::MEM_HANDOVER_ERR_OK;
}

HandoverStatus status() {
    HandoverStatus st{};
    st.frozen = PMM::is_frozen();
    st.runs_total = g_run_count;
    st.runs_capped = PMM::handover_dropped_runs();
    st.pages_handed = PMM::handover_pages();
    st.pages_retained = PMM::handover_retained_pages();
    st.caps_minted = g_run_count;
    st.result = g_result;
    return st;
}

const HandoverRun *region(uint32_t idx) {
    if (idx >= g_run_count) {
        return nullptr;
    }
    return &g_runs[idx];
}

} // namespace untyped_handover
} // namespace kernel
