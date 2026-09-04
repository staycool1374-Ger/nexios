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

/// @file pager_registry.cpp
/// @brief PagerRegistry implementation (issue #107).  Mirrors the DeathNotify /
/// FrameUserMap discipline: mutate under the registry lock, wake/unmap/poke
/// OUTSIDE it.  The bounded pager contract (paper §4) is enforced here: the
/// faulting client blocks only on its passive registry record + the scheduler;
/// the watchdog and the drain paths are the kernel-side waker sources that do
/// not depend on pager liveness.
///
/// ISR-context lock discipline (S1): delegate_fault runs in the #PF ISR with
/// IF=0 and must NEVER block on s_lock_ (a task-context holder can be tick-
/// preempted and never rescheduled while the ISR spins).  It uses try_lock and
/// fails closed (SIGSEGV) when the lock is held.  The watchdog (on_tick ISR)
/// uses try_lock for the same reason.  All other paths run in task context
/// where the plain SpinLock is safe (holders are preemptible, ISR try_locks
/// skip).

#include <kernel/ipc/pager_registry.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/arch/timer.hpp>
#include <constants.hpp>

namespace kernel::ipc {

PagerRegistry::Slot PagerRegistry::s_slots_[PagerRegistry::kMaxClients];
sync::SpinLock PagerRegistry::s_lock_{};
uint64_t PagerRegistry::s_fault_seq_ = 0;

namespace {

/// @brief Whether @p t is live enough to be a client or a pager.
///        Lock-free by design: find_task() is an atomic id_table read and the
///        caller re-validates under s_lock_ before installing a record.
inline bool task_live(const TaskControlBlock *t) {
    if (!t)
        return false;
    return t->magic == TaskControlBlock::TCB_MAGIC &&
           t->state != TaskState::TERMINATED && t->state != TaskState::REAPED;
}

/// @brief Releases a committed pin + unmaps its PTE.  Called outside s_lock_
///        (the release may run dispose -> invalidate_cap -> s_lock_).
inline void release_committed(uint64_t va, uint64_t pml4, cap::FrameCap *pin) {
    if (va != 0)
        VMM::unmap_frame_from_cap(va, pml4);
    if (pin)
        pin->release();
}

/// @brief Releases a committed-pin table (arrays + count).  Called outside
///        s_lock_ (each release may run dispose -> invalidate_cap -> s_lock_).
/// @param va    Array of committed VAs (page-aligned).
/// @param pml4  Array of client PML4 phys.
/// @param pin   Array of cap pins.
/// @param count Number of entries.
inline void release_all_committed(uint64_t *va, uint64_t *pml4,
                                  cap::FrameCap **pin, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i)
        release_committed(va[i], pml4[i], pin[i]);
}

/// @brief Wakes a BLOCKED pager-fault client exactly once (collect-under-lock /
///        wake-outside-lock; id+generation revalidated; clears
///        blocked_on_pager_fault).  Safe no-op on a dead/drained client.
void wake_client(uint64_t client_id, uint32_t client_gen) {
    TaskControlBlock *c = Scheduler::find_task(client_id);
    if (!task_live(c) || c->generation != client_gen)
        return;
    c->blocked_on_pager_fault = nullptr;
    Scheduler::set_task_ready(*c);
}

} // namespace

bool PagerRegistry::register_client(kernel::TaskControlBlock &client,
                                    uint64_t pager_pid) {
    // Authority: the caller designates a pager for ITSELF only.  A third party
    // can never install a pager on a victim (no capability-less fault
    // injection); pager == client is rejected (a task can never page itself).
    if (pager_pid == 0 || pager_pid == client.id)
        return false;

    TaskControlBlock *pager = Scheduler::find_task(pager_pid);
    if (!task_live(pager))
        return false;

    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    pager = Scheduler::find_task(pager_pid);
    if (!task_live(pager))
        return false;
    // One registration per client: a client that already has a registration
    // (to any pager) cannot register again.
    for (size_t i = 0; i < kMaxClients; ++i) {
        if (s_slots_[i].state != SlotState::FREE &&
            s_slots_[i].client_id == client.id &&
            s_slots_[i].client_gen == client.generation)
            return false;
    }
    for (size_t i = 0; i < kMaxClients; ++i) {
        if (s_slots_[i].state == SlotState::FREE) {
            s_slots_[i].state = SlotState::ACTIVE;
            s_slots_[i].client_id = client.id;
            s_slots_[i].client_gen = client.generation;
            s_slots_[i].pager_id = pager_pid;
            s_slots_[i].pager_gen = pager->generation;
            s_slots_[i].poisoned_va = 0;
            s_slots_[i].poisoned_set = false;
            s_slots_[i].fault = {};
            s_slots_[i].pending = false;
            s_slots_[i].mapped_count = 0;
            kernel::test::ResourceTracker::instance()
                .track_pager_registration_add();
            return true;
        }
    }
    return false; // registry full — fail closed
}

bool PagerRegistry::unregister(kernel::TaskControlBlock &caller,
                               uint64_t client_pid) {
    // Authority (F3): the caller may remove its OWN registration, or be the
    // designated pager of the client it drops.  A third party can never evict
    // a victim's registration (availability DoS).
    bool is_self = (caller.id == client_pid);
    // Collect committed pins + pending-fault wake info under the lock; release
    // and wake OUTSIDE it (the release may dispose -> invalidate_cap ->
    // s_lock_).
    uint64_t va[CONFIG_PAGER_MAX_COMMITTED_PAGES];
    uint64_t pml4[CONFIG_PAGER_MAX_COMMITTED_PAGES];
    cap::FrameCap *pin[CONFIG_PAGER_MAX_COMMITTED_PAGES];
    uint32_t count = 0;
    uint64_t wake_id = 0;
    uint32_t wake_gen = 0;
    bool wake = false;
    bool hit = false;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t i = 0; i < kMaxClients; ++i) {
            auto &sl = s_slots_[i];
            if (sl.state == SlotState::FREE || sl.client_id != client_pid)
                continue;
            if (!is_self && !(sl.pager_id == caller.id &&
                              sl.pager_gen == caller.generation))
                continue; // third party — not authorized
            hit = true;
            if (sl.pending) {
                wake_id = sl.fault.client_id;
                wake_gen = sl.fault.client_gen;
                wake = true;
                sl.pending = false;
                kernel::test::ResourceTracker::instance()
                    .track_pager_fault_remove();
            }
            for (uint32_t j = 0; j < sl.mapped_count; ++j) {
                va[count] = sl.mapped_va[j];
                pml4[count] = sl.mapped_pml4[j];
                pin[count] = sl.mapped_pin[j];
                ++count;
            }
            sl.mapped_count = 0;
            sl.state = SlotState::FREE;
            sl.poisoned_set = false;
            kernel::test::ResourceTracker::instance()
                .track_pager_registration_remove();
        }
    }
    release_all_committed(va, pml4, pin, count);
    for (uint32_t j = 0; j < count; ++j)
        kernel::test::ResourceTracker::instance().track_pager_mapping_remove();
    if (wake)
        wake_client(wake_id, wake_gen);
    return hit;
}

int PagerRegistry::recv(kernel::TaskControlBlock &pager, PagerFaultMsg &out) {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    for (size_t i = 0; i < kMaxClients; ++i) {
        auto &sl = s_slots_[i];
        if (sl.state == SlotState::FREE || !sl.pending ||
            sl.pager_id != pager.id || sl.pager_gen != pager.generation)
            continue;
        out.fault_id = sl.fault.fault_id;
        out.client_id = sl.fault.client_id;
        out.fault_va = sl.fault.fault_va;
        out.fault_flags = sl.fault.fault_flags;
        // Two-phase protocol: RECV only COPIES the fault; the record stays
        // PENDING and is consumed by exactly one of {MAP, ABORT, watchdog,
        // drain} (paper §4.3).  Do NOT consume here — a pager that recvs then
        // maps needs the record present.
        return 1;
    }
    return 0; // none pending — never blocks
}

int PagerRegistry::abort(kernel::TaskControlBlock &pager, uint64_t fault_id) {
    // Collect under the lock; rollback + wake outside it.
    uint64_t client_id = 0;
    uint32_t client_gen = 0;
    bool hit = false;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t i = 0; i < kMaxClients; ++i) {
            auto &sl = s_slots_[i];
            if (sl.state == SlotState::FREE || !sl.pending ||
                sl.pager_id != pager.id || sl.pager_gen != pager.generation)
                continue;
            if (sl.fault.fault_id != fault_id)
                continue;
            client_id = sl.fault.client_id;
            client_gen = sl.fault.client_gen;
            // Consume the record, poison the VA latch (F9), keep the
            // registration (per-fault fail-closed).  An aborted fault has no
            // committed pages (map() commits atomically), so nothing to unmap.
            sl.pending = false;
            sl.poisoned_va = sl.fault.fault_va;
            sl.poisoned_set = true;
            hit = true;
            kernel::test::ResourceTracker::instance().track_pager_fault_remove();
            break;
        }
    }
    if (!hit)
        return -1;
    wake_client(client_id, client_gen);
    return 0;
}

void PagerRegistry::invalidate_cap(cap::FrameCap *fc) {
    // Collect matched committed entries under the lock (per-slot, per-page),
    // unmap + release pins outside it.  Pointer-equality only.  At dispose()
    // time no live committed entry references the cap (each pin keeps it
    // alive), so this is normally a no-op; at revoke() time it clears live
    // entries.  Auditor S2: a slot can hold SEVERAL entries pinned to the same
    // cap (a multi-page map with count>1, or the same cap mapped for repeated
    // faults of one client), so every matching entry in the slot must be
    // removed — not just the first (the old break leaked the rest until the
    // client died).
    uint64_t va[CONFIG_PAGER_MAX_COMMITTED_PAGES];
    uint64_t pml4[CONFIG_PAGER_MAX_COMMITTED_PAGES];
    cap::FrameCap *pin[CONFIG_PAGER_MAX_COMMITTED_PAGES];
    for (size_t i = 0; i < kMaxClients; ++i) {
        uint32_t n = 0;
        {
            SpinLockGuard<sync::SpinLock> guard(s_lock_);
            auto &sl = s_slots_[i];
            if (sl.state == SlotState::FREE || sl.mapped_count == 0)
                continue;
            uint32_t j = 0;
            while (j < sl.mapped_count) {
                if (sl.mapped_pin[j] != fc) {
                    ++j;
                    continue;
                }
                va[n] = sl.mapped_va[j];
                pml4[n] = sl.mapped_pml4[j];
                pin[n] = sl.mapped_pin[j];
                ++n;
                // Remove the entry (swap-with-last).
                sl.mapped_pin[j] = sl.mapped_pin[sl.mapped_count - 1];
                sl.mapped_va[j] = sl.mapped_va[sl.mapped_count - 1];
                sl.mapped_pml4[j] = sl.mapped_pml4[sl.mapped_count - 1];
                sl.mapped_pin[sl.mapped_count - 1] = nullptr;
                sl.mapped_va[sl.mapped_count - 1] = 0;
                sl.mapped_pml4[sl.mapped_count - 1] = 0;
                --sl.mapped_count;
                kernel::test::ResourceTracker::instance()
                    .track_pager_mapping_remove();
                // Do NOT ++j: the swapped-in element must be re-examined.
            }
        }
        for (uint32_t k = 0; k < n; ++k)
            release_committed(va[k], pml4[k], pin[k]);
    }
}

void PagerRegistry::drain_task(kernel::TaskControlBlock &t) {
    // Two roles in one scan:
    //  - pager death: evict the registration, wake the client.
    //  - client death: consume the fault, release the committed mappings.
    // Collect all work under the lock, then release + wake outside it.
    struct Work {
        uint64_t client_id;
        uint32_t client_gen;
        bool wake;
        uint64_t pml4;
        cap::FrameCap *pin;
        uint64_t va;
    };
    // Bound: a slot contributes at most CONFIG_PAGER_MAX_COMMITTED_PAGES
    // committed entries PLUS one pending record (auditor S1: the previous
    // kMaxClients * CONFIG_PAGER_MAX_COMMITTED_PAGES could overflow by up to
    // kMaxClients entries when every matched slot had a full committed table
    // AND a pending fault — 8 * 17 = 136 > 128).
    Work work[kMaxClients * (CONFIG_PAGER_MAX_COMMITTED_PAGES + 1)];
    size_t n = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t i = 0; i < kMaxClients; ++i) {
            auto &sl = s_slots_[i];
            if (sl.state == SlotState::FREE)
                continue;
            bool is_pager =
                (sl.pager_id == t.id && sl.pager_gen == t.generation);
            bool is_client =
                (sl.client_id == t.id && sl.client_gen == t.generation);
            if (!is_pager && !is_client)
                continue;
            // Collect every committed pin for release outside the lock.
            for (uint32_t j = 0; j < sl.mapped_count; ++j) {
                work[n].client_id = sl.fault.client_id;
                work[n].client_gen = sl.fault.client_gen;
                work[n].wake = is_pager; // pager death wakes the client
                work[n].pml4 = sl.mapped_pml4[j];
                work[n].pin = sl.mapped_pin[j];
                work[n].va = sl.mapped_va[j];
                ++n;
                kernel::test::ResourceTracker::instance()
                    .track_pager_mapping_remove();
            }
            sl.mapped_count = 0;
            if (sl.pending) {
                work[n].client_id = sl.fault.client_id;
                work[n].client_gen = sl.fault.client_gen;
                work[n].wake = is_pager;
                work[n].pml4 = 0;
                work[n].pin = nullptr;
                work[n].va = 0;
                ++n;
                sl.pending = false;
                kernel::test::ResourceTracker::instance()
                    .track_pager_fault_remove();
            }
            sl.state = SlotState::FREE;
            sl.poisoned_set = false;
            kernel::test::ResourceTracker::instance()
                .track_pager_registration_remove();
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (work[i].va != 0)
            release_committed(work[i].va, work[i].pml4, work[i].pin);
        if (work[i].wake)
            wake_client(work[i].client_id, work[i].client_gen);
    }
}

void PagerRegistry::snapshot_reset() {
    // Clear the whole registry; collect pins and release them OUTSIDE the
    // lock (the last release may run dispose -> invalidate_cap -> s_lock_).
    struct Work {
        uint64_t pml4;
        cap::FrameCap *pin;
        uint64_t va;
    };
    Work work[kMaxClients * CONFIG_PAGER_MAX_COMMITTED_PAGES];
    size_t n = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t i = 0; i < kMaxClients; ++i) {
            auto &sl = s_slots_[i];
            if (sl.state == SlotState::FREE && !sl.pending &&
                sl.mapped_count == 0) {
                sl.poisoned_set = false;
                continue;
            }
            for (uint32_t j = 0; j < sl.mapped_count; ++j) {
                work[n].pml4 = sl.mapped_pml4[j];
                work[n].pin = sl.mapped_pin[j];
                work[n].va = sl.mapped_va[j];
                ++n;
            }
            sl.mapped_count = 0;
            sl.pending = false;
            sl.state = SlotState::FREE;
            sl.poisoned_set = false;
        }
        s_fault_seq_ = 0;
        kernel::test::ResourceTracker::instance()
            .track_pager_registration_reset();
        kernel::test::ResourceTracker::instance().track_pager_fault_reset();
        kernel::test::ResourceTracker::instance().track_pager_mapping_reset();
    }
    for (size_t i = 0; i < n; ++i)
        release_committed(work[i].va, work[i].pml4, work[i].pin);
}

size_t PagerRegistry::live_count() {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    size_t n = 0;
    for (size_t i = 0; i < kMaxClients; ++i)
        if (s_slots_[i].state != SlotState::FREE)
            ++n;
    return n;
}

bool PagerRegistry::is_registered(kernel::TaskControlBlock &client) {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    for (size_t i = 0; i < kMaxClients; ++i) {
        auto &sl = s_slots_[i];
        if (sl.state != SlotState::FREE && sl.client_id == client.id &&
            sl.client_gen == client.generation)
            return true;
    }
    return false;
}

bool PagerRegistry::pending_fault(kernel::TaskControlBlock &pager) {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    for (size_t i = 0; i < kMaxClients; ++i) {
        auto &sl = s_slots_[i];
        if (sl.state != SlotState::FREE && sl.pending &&
            sl.pager_id == pager.id && sl.pager_gen == pager.generation)
            return true;
    }
    return false;
}

bool PagerRegistry::delegate_fault(kernel::TaskControlBlock &client,
                                   uint64_t error_code, uint64_t *regs,
                                   uint64_t cr2) {
    (void)regs; // the #PF ISR epilogue preserves the exception frame (§4.8)

    // ---- Classification F1-F10 (paper §3.2).  Any reject -> SIGSEGV path. ----
    // F2: NOT inside a stac/clac recovery window (the recover-IP redirect
    //     returns before this branch is reached, so this is structurally true;
    //     kept as an explicit guard for defence in depth).
    if (kernel::gs::user_access_recover_ip() != 0)
        return false;
    // F3: not-present fault (P bit 0 clear).
    if (error_code & 1)
        return false;
    // F4: no reserved-bit set (page-table structure sane).
    if (error_code & (1ULL << 3))
        return false;
    // F5: genuine user-address access (U/S bit set).
    if (!(error_code & (1ULL << 2)))
        return false;
    // F6: data access only (I/D bit clear; v1 NX-only).
    if (error_code & (1ULL << 4))
        return false;
    // F7: fault VA in the user half.
    const uint64_t fault_va =
        cr2 & ~(static_cast<uint64_t>(arch::PAGE_SIZE) - 1);
    if (cr2 >= USER_SPACE_LIMIT)
        return false;

    // F8 + record creation (under the lock).
    //
    // CRITICAL (S1): this runs in the #PF ISR with IF=0.  A task-context
    // holder of s_lock_ (register/recv/map/abort/unregister) can be tick-
    // preempted and never rescheduled while the ISR spins on the lock.  Use
    // try_lock and FAIL CLOSED (SIGSEGV) when the lock is held — a transient
    // SIGSEGV is far better than a permanent ISR spin deadlock.
    if (!s_lock_.try_lock())
        return false;
    uint64_t pager_id = 0;
    uint32_t pager_gen = 0;
    size_t slot_idx = kMaxClients;
    {
        for (size_t i = 0; i < kMaxClients; ++i) {
            auto &sl = s_slots_[i];
            if (sl.state == SlotState::FREE || sl.client_id != client.id ||
                sl.client_gen != client.generation)
                continue;
            // F8: live pager, pager != client.
            if (sl.pager_id == client.id) {
                s_lock_.unlock();
                return false;
            }
            TaskControlBlock *p = Scheduler::find_task(sl.pager_id);
            if (!task_live(p) || p->generation != sl.pager_gen) {
                s_lock_.unlock();
                return false;
            }
            // F9: not the poisoned-aborted VA latch.
            if (sl.poisoned_set && sl.poisoned_va == fault_va) {
                s_lock_.unlock();
                return false;
            }
            // F10: no collision with an installed canary slot.
            if (client.canary_installed != 0) {
                bool collide = false;
                for (int k = 0; k < TaskControlBlock::CANARY_SEGMENTS; ++k) {
                    if ((client.canary_installed & (1U << k)) == 0)
                        continue;
                    uint64_t cb = client.canary_before[k];
                    uint64_t ca = client.canary_after[k];
                    if ((fault_va >= cb && fault_va < cb + arch::PAGE_SIZE) ||
                        (fault_va >= ca && fault_va < ca + arch::PAGE_SIZE)) {
                        collide = true;
                        break;
                    }
                }
                if (collide) {
                    s_lock_.unlock();
                    return false;
                }
            }
            // B1: the caller is RUNNING, so no pending fault exists for this
            // slot (a client with a pending fault is BLOCKED).  Structural.
            if (sl.pending) {
                s_lock_.unlock();
                return false;
            }
            // A successful delegation of a DIFFERENT VA clears the poison
            // latch (paper §4.5).
            if (sl.poisoned_set && sl.poisoned_va != fault_va)
                sl.poisoned_set = false;

            ++s_fault_seq_;
            sl.fault = {};
            sl.fault.fault_id = s_fault_seq_;
            sl.fault.client_id = client.id;
            sl.fault.client_gen = client.generation;
            sl.fault.pager_id = sl.pager_id;
            sl.fault.pager_gen = sl.pager_gen;
            sl.fault.fault_va = fault_va;
            sl.fault.fault_flags = error_code;
            sl.fault.deadline_tick =
                arch::Timer::ticks() + CONFIG_PAGER_FAULT_TIMEOUT_TICKS;
            sl.fault.client_pml4 = client.page_table_;
            sl.pending = true;
            pager_id = sl.pager_id;
            pager_gen = sl.pager_gen;
            slot_idx = i;
            kernel::test::ResourceTracker::instance().track_pager_fault_add();
            break;
        }
        s_lock_.unlock();
    }
    if (slot_idx == kMaxClients)
        return false; // no registration -> SIGSEGV

    // Pulse the pager OUTSIDE the lock (id+gen revalidated; safe no-op on a
    // dying pager — DeathNotify pattern).
    {
        TaskControlBlock *p = Scheduler::find_task(pager_id);
        if (task_live(p) && p->generation == pager_gen)
            p->notify.notify(static_cast<uint64_t>(PAGER_FAULT_PULSE));
    }

    // Block the client inside the #PF ISR (paper §4.8): the exception frame
    // stays on the client's kernel stack; the ISR epilogue saves it into
    // context.rsp and iretq's to the next task.  Resume restores the frame so
    // the faulting instruction retries.  switch_away_from_terminating only
    // re-enqueues a RUNNING current — a BLOCKED one is left dequeued.
    client.state = TaskState::BLOCKED;
    client.blocked_on_pager_fault = &s_slots_[slot_idx].fault;
    Scheduler::dequeue_ready(client);
    Scheduler::switch_away_from_terminating(client);
    return true;
}

int PagerRegistry::map(kernel::TaskControlBlock &pager, uint64_t fault_id,
                       cap::FrameCap *fc, uint64_t count, uint64_t flags) {
    (void)flags; // v1 maps NX-only; no flag-gated executability (paper §2.2)
    if (!fc || fc->revoked() || fc->phys == 0 || fc->count == 0)
        return -1;
    // S3 (audit): a kernel-backed cap is never user-mappable — mapping kernel
    // physical memory into the client's user window would be a privilege
    // escalation (defence-in-depth; fail closed like FrameUserMap).
    if (!fc->is_user)
        return -1;
    if (count == 0 || count > CONFIG_PAGER_MAX_PAGES_PER_FAULT)
        return -1;
    // F4 (audit): never map beyond the cap's extent — a 1-page cap cannot
    // map 4 pages (would expose adjacent user-owned frames).
    if (count > fc->count)
        return -1;

    // The whole map (phases 1-3) runs under IrqGuard: the VMM walk may
    // allocate page-table pages and the commit must be atomic against the
    // client being terminated concurrently (F6).  A BLOCKED pager-fault client
    // can be SIGKILL'd; if cleanup ran free_user_pages mid-map, phase 3 would
    // write into freed page-table memory.  IrqGuard makes the map atomic w.r.t.
    // the scheduler (no tick -> no deferred switch -> no termination).
    arch::IrqGuard ig{};

    uint64_t fault_va = 0;
    uint64_t client_pml4 = 0;
    uint64_t client_id = 0;
    uint32_t client_gen = 0;
    size_t slot_idx = kMaxClients;
    // Phase 1: claim the record + set the MAP_IN_PROGRESS pin under the lock.
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t i = 0; i < kMaxClients; ++i) {
            auto &sl = s_slots_[i];
            if (sl.state == SlotState::FREE || !sl.pending ||
                sl.pager_id != pager.id || sl.pager_gen != pager.generation)
                continue;
            if (sl.fault.fault_id != fault_id || sl.fault.map_in_progress)
                return -1;
            // The committed table must have room for @p count more pages.
            if (sl.mapped_count + count > CONFIG_PAGER_MAX_COMMITTED_PAGES)
                return -1;
            fault_va = sl.fault.fault_va;
            client_pml4 = sl.fault.client_pml4;
            client_id = sl.fault.client_id;
            client_gen = sl.fault.client_gen;
            slot_idx = i;
            sl.fault.map_in_progress = true;
            break;
        }
    }
    if (slot_idx == kMaxClients)
        return -1;

    // Phase 2: map the pager's frames into the CLIENT's PML4 OUTSIDE the lock
    // (map_page_in_pml4 may allocate table pages; NX-only, user-accessible).
    // Still inside IrqGuard — the map is atomic.
    for (uint64_t i = 0; i < count; ++i) {
        VMM::map_page_in_pml4(fault_va + i * arch::PAGE_SIZE,
                              fc->phys + i * arch::PAGE_SIZE, /*user=*/true,
                              /*executable=*/false, client_pml4);
    }

    // Phase 3: re-acquire the lock; re-validate the record still PENDING and
    // the cap NOT revoked (closes the revoke-during-map race with
    // invalidate_cap).  On success, commit into the committed table (F1: the
    // pins are held until drain/invalidate/unregister/snapshot).  On failure,
    // roll back the phase-2 PTEs and release any partially-acquired pins
    // OUTSIDE the lock (S3 audit: a release may run dispose -> invalidate_cap
    // -> s_lock_, which must never happen under the registry lock).
    bool committed = false;
    cap::FrameCap *rollback_pin[CONFIG_PAGER_MAX_PAGES_PER_FAULT];
    uint32_t n_rollback = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        auto &sl = s_slots_[slot_idx];
        if (sl.state != SlotState::FREE && sl.pending &&
            sl.fault.fault_id == fault_id && !fc->revoked()) {
            committed = true;
            for (uint64_t i = 0; i < count; ++i) {
                if (!fc->acquire()) {
                    // Cap revoked concurrently — undo what we acquired so far.
                    for (uint64_t j = 0; j < i; ++j) {
                        --sl.mapped_count;
                        rollback_pin[n_rollback++] =
                            sl.mapped_pin[sl.mapped_count];
                        sl.mapped_va[sl.mapped_count] = 0;
                        sl.mapped_pin[sl.mapped_count] = nullptr;
                        sl.mapped_pml4[sl.mapped_count] = 0;
                        kernel::test::ResourceTracker::instance()
                            .track_pager_mapping_remove();
                    }
                    committed = false;
                    break;
                }
                sl.mapped_va[sl.mapped_count] = fault_va + i * arch::PAGE_SIZE;
                sl.mapped_pml4[sl.mapped_count] = client_pml4;
                sl.mapped_pin[sl.mapped_count] = fc;
                ++sl.mapped_count;
                kernel::test::ResourceTracker::instance()
                    .track_pager_mapping_add();
            }
        }
        if (committed) {
            sl.pending = false;
            sl.fault.map_in_progress = false;
            kernel::test::ResourceTracker::instance()
                .track_pager_fault_remove();
        } else {
            sl.fault.map_in_progress = false;
        }
    }
    // Release rolled-back pins OUTSIDE the lock (may dispose -> invalidate_cap).
    for (uint32_t j = 0; j < n_rollback; ++j)
        rollback_pin[j]->release();
    if (!committed) {
        // Roll back the phase-2 PTEs (map may have partially written).
        for (uint64_t i = 0; i < count; ++i)
            VMM::unmap_frame_from_cap(fault_va + i * arch::PAGE_SIZE,
                                      client_pml4);
        return -1;
    }

    // Wake the client exactly once, outside the lock (still under IrqGuard —
    // set_task_ready takes IrqGuard itself, nesting is fine).
    wake_client(client_id, client_gen);
    return 0;
}

void PagerRegistry::watchdog_scan(uint64_t now) {
    // Runs under scheduler_lock_ + IrqGuard (on_tick), i.e. in ISR context.
    // CRITICAL: use try_lock — a task-context holder of s_lock_ (register /
    // recv / map / abort / drain) can be preempted by this tick, and blocking
    // here would spin the ISR forever (the holder can never be rescheduled
    // while the ISR spins).  Skipping a tick is harmless: the 1000-tick
    // deadline absorbs a skipped scan, and a holder is mid-operation and
    // releases imminently.
    if (!s_lock_.try_lock())
        return; // registry lock held by task context — skip this tick
    struct Work {
        uint64_t client_id;
        uint32_t client_gen;
    };
    Work work[kMaxClients];
    size_t n = 0;
    {
        for (size_t i = 0; i < kMaxClients; ++i) {
            auto &sl = s_slots_[i];
            if (sl.state == SlotState::FREE || !sl.pending)
                continue;
            if (sl.fault.map_in_progress)
                continue; // mid-map — defer (deadline absorbs the window)
            if (sl.fault.deadline_tick > now)
                continue;
            work[n].client_id = sl.fault.client_id;
            work[n].client_gen = sl.fault.client_gen;
            ++n;
            sl.pending = false;
            sl.poisoned_va = sl.fault.fault_va; // F9 latch
            sl.poisoned_set = true;
            kernel::test::ResourceTracker::instance().track_pager_fault_remove();
            // An expired fault has no committed pages (map() commits
            // atomically under IrqGuard); nothing to unmap.
        }
        s_lock_.unlock();
    }
    for (size_t i = 0; i < n; ++i)
        wake_client(work[i].client_id, work[i].client_gen);
}

} // namespace kernel::ipc