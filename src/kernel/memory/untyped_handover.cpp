/// @file kernel/memory/untyped_handover.cpp
/// @brief Mem-mgr bootstrap handover registry (M4, #563/F10).
#include <kernel/memory/untyped_handover.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/untyped.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/log/dmesg.hpp>
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
    g_source = &root;
    for (uint32_t i = 0; i < runs; ++i) {
        if (g_run_count >= CONFIG_MEM_HANDOVER_MAX_RUNS) {
            g_result = errors::MEM_HANDOVER_ERR_REGISTRY_FULL;
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
            g_result = errors::MEM_HANDOVER_ERR_UNTYPED_CAP_FULL;
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
            g_result = errors::MEM_HANDOVER_ERR_CSPACE_FULL;
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
