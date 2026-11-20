/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
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

/// @file proc_tcb_caps.cpp
/// @brief Proc-mgr TCB-cap hold table (F11 Stage A, #564; §8.8 protocol).
///        Holds are lifecycle tracking only — zero stop power (INV-PROC-6).
///        Locking: one table leaf lock (Leaf rank, innermost); the CNode
///        leaf internals of install/remove_if and the derivation-table leaf
///        are each taken separately and sequentially — a CNode slot lock is
///        never held across a derivation-table call (INV-DERIV-1), and the
///        table lock is never held across a CNode call (collect-then-act in
///        release_on_death). The funnel leg takes no lock itself: the
///        caller (publish_task_death) holds IrqGuard with scheduler_lock_
///        NOT held, and this leg adds only leaf takes (INV-PROC-5).

#include <kernel/proc/proc_tcb_caps.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/tcb_cap.hpp>
#include <kernel/cap/derivation.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/log/dmesg.hpp>

namespace kernel::proc::proc_tcb_caps {

namespace {

/// @brief One tracked hold: (holder, target) identity plus the installed
///        CNode slot naming the wrapper.
struct HoldSlot {
    bool occupied = false;
    uint64_t holder_id = 0;
    uint32_t holder_gen = 0;
    uint64_t target_id = 0;
    uint32_t target_gen = 0;
    int cnode_slot = -1;
    uint32_t cnode_gen = 0;
};

/// @brief Fixed hold index (INV-PROC-4): no allocation on any path.
HoldSlot g_hold_slots[CONFIG_PROC_MGR_MAX_HELD_TCB_CAPS] = {};

/// @brief Table leaf lock (Leaf rank, innermost; never held across a
///        CNode or derivation-table call).
sync::SpinLock g_hold_lock{sync::LockRank::Leaf};

/// @brief Live-hold fast-path counter (atomics; the funnel skips the scan
///        when no hold exists — zero behavior delta at Stage A).
uint64_t g_live_holds = 0;

/// @brief Funnel removal job (collect-then-act: gathered under the table
///        lock, executed after it is released).
struct FunnelJob {
    cap::CNode *holder_space = nullptr;
    uint32_t cnode_slot = 0;
    uint32_t cnode_gen = 0;
};

/// @brief Live-target gate shared by hold/lookup (lock-free probe).
/// @return The live target, or nullptr when unresolvable (never issued,
///         magic mismatch, generation mismatch, TERMINATED/REAPED).
TaskControlBlock *resolve_live_target(uint64_t target_pid,
                                      uint32_t target_gen) noexcept {
    if (!Scheduler::validate_id(target_pid)) {
        return nullptr;
    }
    auto *target = Scheduler::find_task(target_pid);
    if (target == nullptr ||
        target->magic != TaskControlBlock::TCB_MAGIC) {
        return nullptr;
    }
    if (target->generation != target_gen) {
        return nullptr;
    }
    if (target->state == TaskState::TERMINATED ||
        target->state == TaskState::REAPED) {
        return nullptr;
    }
    return target;
}

} // namespace

int hold(TaskControlBlock &holder, uint64_t target_pid, uint32_t target_gen,
         errors::ProcError *err_out) noexcept {
    if (resolve_live_target(target_pid, target_gen) == nullptr) {
        if (err_out != nullptr) {
            *err_out = errors::PROC_ERR_STALE_PID;
        }
        return -1;
    }
    int free_idx = -1;
    {
        SpinLockGuard<sync::SpinLock> table_guard(g_hold_lock);
        for (uint32_t idx = 0; idx < kMaxHeldTcbCaps; ++idx) {
            if (!g_hold_slots[idx].occupied) {
                free_idx = static_cast<int>(idx);
                break;
            }
        }
    }
    if (free_idx < 0) {
        // INV-PROC-4: overflow fails closed — the child is refused before
        // any publication (no zombie, no half-hold). Logged outside the
        // table lock (no logging under registry locks).
        log::dmesg_push_sev(log::ErrorSubsystem::TASK,
                            log::kDmesgBase_TASK + 18,
                            log::LogSeverity::WARN,
                            "Proc-mgr hold table full (child refused)",
                            target_pid);
        if (err_out != nullptr) {
            *err_out = errors::PROC_ERR_CAP_TABLE_FULL;
        }
        return -1;
    }
    holder.ensure_cspace();
    cap::CNode *holder_space = holder.get_cspace();
    if (holder_space == nullptr) {
        if (err_out != nullptr) {
            *err_out = errors::PROC_ERR_HOLD_INSTALL_FAILED;
        }
        return -1;
    }
    // TcbCap::create revalidates liveness (fail-closed across the probe
    // window above); folds into the existing cap_objects counter (F4).
    cap::TcbCap *held_cap = cap::TcbCap::create(target_pid, target_gen);
    if (held_cap == nullptr) {
        if (err_out != nullptr) {
            *err_out = errors::PROC_ERR_HOLD_INSTALL_FAILED;
        }
        return -1;
    }
    // WRITE rights match the Q9 resolve gate (tcb_control_resolve needs
    // WRITE); the CREATOR_ONLY scope keeps branch 6 deny-all, so the hold
    // is usable only via the parent_id client branch (INV-PROC-6).
    uint32_t slot_gen = 0;
    const int cnode_slot =
        holder_space->install(held_cap, cap::CapType::TCB,
                              cap::CapRights::WRITE, &slot_gen);
    if (cnode_slot < 0) {
        held_cap->release();
        if (err_out != nullptr) {
            *err_out = errors::PROC_ERR_HOLD_INSTALL_FAILED;
        }
        return -1;
    }
    // F2 derivation edge: a fresh root (self) edge, the untyped-retype
    // shape. A cross-CNode parent edge would need the holder CNode named
    // inside a granter CNode (record_copy_edge precedent) — no such naming
    // exists at Stage A, and no creator cap can exist yet (nothing holds),
    // so the root leg is the only reachable one; Stage B may parent at the
    // supervisor-held cap once that naming exists.
    const uint32_t parent_depth = static_cast<uint32_t>(
        cap::DerivationTable::instance().child_depth(
            *holder_space, static_cast<uint32_t>(cnode_slot), slot_gen));
    if (parent_depth + 1 > static_cast<uint32_t>(CONFIG_CAP_MAX_DEPTH)) {
        holder_space->remove(static_cast<uint32_t>(cnode_slot));
        held_cap->release();
        if (err_out != nullptr) {
            *err_out = errors::PROC_ERR_HOLD_INSTALL_FAILED;
        }
        return -1;
    }
    const uint8_t edge_depth = static_cast<uint8_t>(parent_depth + 1);
    const bool edge_recorded = cap::DerivationTable::instance().record(
        *holder_space, static_cast<uint32_t>(cnode_slot), slot_gen,
        *holder_space, cap::kDerivSelfNode, 0,
        static_cast<uint32_t>(cnode_slot), slot_gen, edge_depth);
    if (!edge_recorded) {
        // Derivation-full: roll the install back (fail closed — never a
        // silent untracked hold). remove() prunes any partial edge state.
        holder_space->remove(static_cast<uint32_t>(cnode_slot));
        held_cap->release();
        if (err_out != nullptr) {
            *err_out = errors::PROC_ERR_HOLD_INSTALL_FAILED;
        }
        return -1;
    }
    // Publish only after mint + edge both succeed (never a half-hold).
    held_cap->release();
    {
        SpinLockGuard<sync::SpinLock> table_guard(g_hold_lock);
        HoldSlot &slot_entry = g_hold_slots[static_cast<uint32_t>(free_idx)];
        slot_entry.occupied = true;
        slot_entry.holder_id = holder.id;
        slot_entry.holder_gen = holder.generation;
        slot_entry.target_id = target_pid;
        slot_entry.target_gen = target_gen;
        slot_entry.cnode_slot = cnode_slot;
        slot_entry.cnode_gen = slot_gen;
    }
    __atomic_fetch_add(&g_live_holds, 1UL, __ATOMIC_RELEASE);
    return free_idx;
}

TaskControlBlock *lookup(TaskControlBlock &holder, uint64_t target_pid,
                         uint32_t target_gen) noexcept {
    uint32_t found_slot = 0;
    uint32_t found_gen = 0;
    bool found_entry = false;
    {
        SpinLockGuard<sync::SpinLock> table_guard(g_hold_lock);
        for (uint32_t idx = 0; idx < kMaxHeldTcbCaps; ++idx) {
            const HoldSlot &slot_entry = g_hold_slots[idx];
            if (slot_entry.occupied &&
                slot_entry.holder_id == holder.id &&
                slot_entry.holder_gen == holder.generation &&
                slot_entry.target_id == target_pid &&
                slot_entry.target_gen == target_gen) {
                found_slot = static_cast<uint32_t>(slot_entry.cnode_slot);
                found_gen = slot_entry.cnode_gen;
                found_entry = true;
                break;
            }
        }
    }
    if (!found_entry) {
        return nullptr;
    }
    cap::CNode *holder_space = holder.get_cspace();
    if (holder_space == nullptr) {
        return nullptr;
    }
    const uint64_t cap_handle = cap::encode_handle(
        holder_space->cspace_id, found_slot, found_gen);
    KernelObject *pinned =
        cap::lookup(holder_space, cap_handle, cap::CapType::TCB,
                    cap::CapRights::WRITE);
    if (pinned == nullptr) {
        return nullptr;
    }
    auto *named_cap = static_cast<cap::TcbCap *>(pinned);
    const bool tag_ok = (named_cap->tag == cap::TcbCap::kTcbCapTag);
    const bool binds_target = (named_cap->target_id == target_pid &&
                               named_cap->target_gen == target_gen);
    pinned->release();
    if (!tag_ok || !binds_target) {
        return nullptr;
    }
    // Generation equality re-checked at resolve time (pid reuse guard).
    return resolve_live_target(target_pid, target_gen);
}

errors::ProcError release(TaskControlBlock &holder, uint64_t target_pid,
                         uint32_t target_gen) noexcept {
    uint32_t found_slot = 0;
    uint32_t found_gen = 0;
    uint32_t found_idx = 0;
    bool found_entry = false;
    bool gen_mismatch = false;
    {
        SpinLockGuard<sync::SpinLock> table_guard(g_hold_lock);
        for (uint32_t idx = 0; idx < kMaxHeldTcbCaps; ++idx) {
            const HoldSlot &slot_entry = g_hold_slots[idx];
            if (slot_entry.occupied &&
                slot_entry.holder_id == holder.id &&
                slot_entry.holder_gen == holder.generation &&
                slot_entry.target_id == target_pid) {
                if (slot_entry.target_gen != target_gen) {
                    gen_mismatch = true;
                    break;
                }
                found_slot = static_cast<uint32_t>(slot_entry.cnode_slot);
                found_gen = slot_entry.cnode_gen;
                found_idx = idx;
                found_entry = true;
                break;
            }
        }
        if (found_entry) {
            g_hold_slots[found_idx].occupied = false;
        }
    }
    if (gen_mismatch) {
        // PID recycled under the hold: never touch the slot.
        return errors::PROC_ERR_STALE_PID;
    }
    if (!found_entry) {
        return errors::PROC_ERR_OK;
    }
    __atomic_fetch_sub(&g_live_holds, 1UL, __ATOMIC_RELEASE);
    cap::CNode *holder_space = holder.get_cspace();
    if (holder_space != nullptr) {
        // Gen-checked remove: a recycled CNode slot refuses (false), and
        // the entry is already cleared above — idempotent either way. The
        // edge prunes with the slot; the wrapper releases outside the
        // CNode lock (remove_if contract).
        holder_space->remove_if(found_slot, found_gen);
    }
    return errors::PROC_ERR_OK;
}

void release_on_death(uint64_t dead_pid, uint32_t dead_gen) noexcept {
    if (__atomic_load_n(&g_live_holds, __ATOMIC_ACQUIRE) == 0) {
        return;
    }
    // Collect-then-act: at most one job per slot, gathered under the table
    // lock; CNode removals run after the lock is released (never nested —
    // INV-DERIV-1). Holder-dead entries clear by index only: the holder's
    // CNode teardown already released the wrappers and pruned the edges
    // (CNode::dispose forget_node + per-slot release).
    FunnelJob pending_jobs[CONFIG_PROC_MGR_MAX_HELD_TCB_CAPS] = {};
    uint32_t pending_count = 0;
    {
        SpinLockGuard<sync::SpinLock> table_guard(g_hold_lock);
        for (uint32_t idx = 0; idx < kMaxHeldTcbCaps; ++idx) {
            HoldSlot &slot_entry = g_hold_slots[idx];
            if (!slot_entry.occupied) {
                continue;
            }
            const bool holder_dead =
                (slot_entry.holder_id == dead_pid &&
                 slot_entry.holder_gen == dead_gen);
            const bool target_dead =
                (slot_entry.target_id == dead_pid &&
                 slot_entry.target_gen == dead_gen);
            if (!holder_dead && !target_dead) {
                continue;
            }
            if (!holder_dead && target_dead) {
                auto *holder_task =
                    Scheduler::find_task(slot_entry.holder_id);
                if (holder_task != nullptr &&
                    holder_task->magic == TaskControlBlock::TCB_MAGIC &&
                    holder_task->generation == slot_entry.holder_gen) {
                    cap::CNode *holder_space = holder_task->get_cspace();
                    if (holder_space != nullptr &&
                        pending_count < kMaxHeldTcbCaps) {
                        FunnelJob &job_entry = pending_jobs[pending_count];
                        job_entry.holder_space = holder_space;
                        job_entry.cnode_slot =
                            static_cast<uint32_t>(slot_entry.cnode_slot);
                        job_entry.cnode_gen = slot_entry.cnode_gen;
                        ++pending_count;
                    }
                }
            }
            slot_entry.occupied = false;
            __atomic_fetch_sub(&g_live_holds, 1UL, __ATOMIC_RELAXED);
        }
    }
    for (uint32_t job_idx = 0; job_idx < pending_count; ++job_idx) {
        const FunnelJob &job_entry = pending_jobs[job_idx];
        // Gen-checked: a recycled slot refuses; the index is already
        // cleared, so a refusal is an idempotent no-op (double-publish
        // safe — second call finds no entry at all).
        job_entry.holder_space->remove_if(job_entry.cnode_slot,
                                          job_entry.cnode_gen);
    }
}

void snapshot_reset() noexcept {
    SpinLockGuard<sync::SpinLock> table_guard(g_hold_lock);
    for (uint32_t idx = 0; idx < kMaxHeldTcbCaps; ++idx) {
        g_hold_slots[idx].occupied = false;
        g_hold_slots[idx].holder_id = 0;
        g_hold_slots[idx].holder_gen = 0;
        g_hold_slots[idx].target_id = 0;
        g_hold_slots[idx].target_gen = 0;
        g_hold_slots[idx].cnode_slot = -1;
        g_hold_slots[idx].cnode_gen = 0;
    }
    __atomic_store_n(&g_live_holds, 0UL, __ATOMIC_RELEASE);
}

uint64_t live_hold_count() noexcept {
    return __atomic_load_n(&g_live_holds, __ATOMIC_ACQUIRE);
}

} // namespace kernel::proc::proc_tcb_caps
