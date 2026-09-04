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

/// @file death_notify.cpp
/// @brief DeathNotify implementation (issue #105 Part B).  Mirrors the
/// FrameUserMap/MmioUserMap discipline: claim/mutate under the registry lock,
/// poke supervisors OUTSIDE it (a Notify poke can reach the scheduler, and the
/// documented order is scheduler_lock_ -> registry locks).  A slot's lifetime
/// is ACTIVE (watch installed) -> PENDING (watched task died, record latched)
/// -> FREE (supervisor consumed it via recv, or drain freed it).

#include <kernel/ipc/death_notify.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/test/resource_tracker.hpp>

namespace kernel::ipc {

DeathNotify::Slot DeathNotify::s_slots_[DeathNotify::kMaxWatches];
sync::SpinLock DeathNotify::s_lock_{};

namespace {

/// @brief Whether @p t is live enough to be watched or to supervise.
///        Lock-free by design: find_task() does an atomic id_table read and
///        the caller re-validates under s_lock_ before installing a slot, so
///        the residual register/death race is recovered by unwatch/drain and
///        documented in docs/specs/death-notify.md.
inline bool task_live(const TaskControlBlock *t) {
    if (!t)
        return false;
    return t->magic == TaskControlBlock::TCB_MAGIC &&
           t->state != TaskState::TERMINATED && t->state != TaskState::REAPED;
}

} // namespace

bool DeathNotify::watch(kernel::TaskControlBlock &actor, uint64_t watched_pid,
                        uint64_t supervisor_pid) {
    // Authority gate: the actor must be either the watched task or the
    // supervisor it designates.  A third party can never install a watch (a
    // spurious wakeup pulse on a victim supervisor is still denied).
    const uint64_t sup = supervisor_pid != 0 ? supervisor_pid : actor.id;
    if (!(actor.id == watched_pid || actor.id == sup))
        return false;
    if (sup == watched_pid)
        return false; // self-watch is meaningless

    // Resolve both tasks lock-free first (cheap reject), then re-validate
    // under s_lock_ at install time.
    TaskControlBlock *watched = Scheduler::find_task(watched_pid);
    TaskControlBlock *supervisor = Scheduler::find_task(sup);
    if (!task_live(watched) || !task_live(supervisor))
        return false;

    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    // Re-validate under the lock: the tasks may have died between the
    // lock-free resolution and the slot claim (find_task is lock-free).
    watched = Scheduler::find_task(watched_pid);
    supervisor = Scheduler::find_task(sup);
    if (!task_live(watched) || !task_live(supervisor))
        return false;
    for (size_t i = 0; i < kMaxWatches; ++i) {
        if (s_slots_[i].state == SlotState::FREE) {
            s_slots_[i].state = SlotState::ACTIVE;
            s_slots_[i].watched_id = watched_pid;
            s_slots_[i].watched_gen = watched->generation;
            s_slots_[i].supervisor_id = sup;
            s_slots_[i].supervisor_gen = supervisor->generation;
            s_slots_[i].record = {};
            kernel::test::ResourceTracker::instance().track_death_watch_add();
            return true;
        }
    }
    return false; // registry full — fail closed
}

void DeathNotify::unwatch(kernel::TaskControlBlock &supervisor,
                          uint64_t watched_pid) {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    for (size_t i = 0; i < kMaxWatches; ++i) {
        auto &sl = s_slots_[i];
        if (sl.state != SlotState::FREE &&
            sl.supervisor_id == supervisor.id &&
            sl.supervisor_gen == supervisor.generation &&
            sl.watched_id == watched_pid) {
            sl.state = SlotState::FREE;
            kernel::test::ResourceTracker::instance().
                track_death_watch_remove();
        }
    }
}

int DeathNotify::recv(kernel::TaskControlBlock &supervisor, DeathRecord &out) {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    for (size_t i = 0; i < kMaxWatches; ++i) {
        auto &sl = s_slots_[i];
        if (sl.state == SlotState::PENDING &&
            sl.supervisor_id == supervisor.id &&
            sl.supervisor_gen == supervisor.generation) {
            out = sl.record;
            sl.state = SlotState::FREE;
            kernel::test::ResourceTracker::instance().
                track_death_watch_remove();
            return 1;
        }
    }
    return 0; // none pending — never blocks
}

void DeathNotify::on_task_death(kernel::TaskControlBlock &dead) {
    // Collect the supervisor (id, generation) pairs to poke OUTSIDE the lock.
    // Bounded by kMaxWatches (one slot per watch; a task can be watched by
    // many).  The generation is captured alongside the id so the poke-time
    // re-resolution revalidates BOTH — a recycled TCB with the same id but a
    // fresh generation is never poked (docs/specs/death-notify.md invariant #4).
    struct Poke {
        uint64_t id;
        uint32_t gen;
    };
    Poke pokes[kMaxWatches];
    size_t n_pokes = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(s_lock_);
        for (size_t i = 0; i < kMaxWatches; ++i) {
            auto &sl = s_slots_[i];
            if (sl.state != SlotState::ACTIVE)
                continue;
            if (sl.watched_id == dead.id && sl.watched_gen == dead.generation) {
                sl.state = SlotState::PENDING;
                sl.record.dead_id = dead.id;
                sl.record.exit_code = dead.exit_code;
                sl.record.flags =
                    (dead.exit_code & (1ULL << 63)) ? DEATH_FLAG_SIGNAL : 0;
                if (sl.supervisor_id != dead.id) {
                    pokes[n_pokes].id = sl.supervisor_id;
                    pokes[n_pokes].gen = sl.supervisor_gen;
                    ++n_pokes;
                }
            }
        }
    }
    // Poke each supervisor outside the lock.  Re-resolve by id + generation
    // and go through Notify::notify(), which guards TERMINATED/REAPED plus the
    // captured generation — a poke to a just-died supervisor is a safe no-op.
    for (size_t i = 0; i < n_pokes; ++i) {
        TaskControlBlock *sup = Scheduler::find_task(pokes[i].id);
        if (!task_live(sup) || sup->generation != pokes[i].gen)
            continue;
        sup->notify.notify(static_cast<uint64_t>(DEATH_WAKE_PULSE));
    }
    // Free the dead task's own slots (incl. any ACTIVE self-watch it held as a
    // supervisor for others) before its Notify is destroyed in cleanup().
    drain_task(dead);
}

void DeathNotify::drain_task(kernel::TaskControlBlock &supervisor) {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    for (size_t i = 0; i < kMaxWatches; ++i) {
        auto &sl = s_slots_[i];
        if (sl.state != SlotState::FREE &&
            sl.supervisor_id == supervisor.id &&
            sl.supervisor_gen == supervisor.generation) {
            sl.state = SlotState::FREE;
            kernel::test::ResourceTracker::instance().
                track_death_watch_remove();
        }
    }
}

void DeathNotify::snapshot_reset() {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    for (size_t i = 0; i < kMaxWatches; ++i) {
        s_slots_[i].state = SlotState::FREE;
        s_slots_[i].watched_id = 0;
        s_slots_[i].watched_gen = 0;
        s_slots_[i].supervisor_id = 0;
        s_slots_[i].supervisor_gen = 0;
        s_slots_[i].record = {};
    }
    // The registry holds no owned resources (no cap pins), so no per-slot
    // release is needed — the counter is reset to the pristine 0.
    kernel::test::ResourceTracker::instance().track_death_watch_reset();
}

size_t DeathNotify::live_count() {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    size_t n = 0;
    for (size_t i = 0; i < kMaxWatches; ++i)
        if (s_slots_[i].state != SlotState::FREE)
            ++n;
    return n;
}

bool DeathNotify::is_watching(kernel::TaskControlBlock &supervisor,
                              uint64_t watched_pid) {
    SpinLockGuard<sync::SpinLock> guard(s_lock_);
    for (size_t i = 0; i < kMaxWatches; ++i) {
        auto &sl = s_slots_[i];
        if (sl.state != SlotState::FREE &&
            sl.supervisor_id == supervisor.id &&
            sl.supervisor_gen == supervisor.generation &&
            sl.watched_id == watched_pid)
            return true;
    }
    return false;
}

} // namespace kernel::ipc