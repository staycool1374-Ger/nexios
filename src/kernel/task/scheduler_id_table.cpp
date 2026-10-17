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

/// @file scheduler_id_table.cpp
/// @brief Task-ID hash table for Scheduler (issue #323).
///        Pure move out of scheduler.cpp: no logic changes.

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_id_table_internal.hpp>

namespace kernel {

// alloc_id_err keeps its unqualified SchedulerError/SCHED_ERR_* names
// byte-identical; mirror the old home's TU-wide using (allowed in .cpp
// files per CODING_STYLE §3; headers stay using-free).
using namespace errors;

TaskControlBlock *Scheduler::find_task(uint64_t id) noexcept {
    return id_table_find(id);
}

// ---------------------------------------------------------------------------
// O(1) task-ID→TCB hash table (open addressing, linear probing)
// ---------------------------------------------------------------------------

uint64_t Scheduler::id_table_probe(uint64_t id) {
    return id & ID_TABLE_MASK;
}

bool Scheduler::id_table_insert(uint64_t id, TaskControlBlock *tcb) {
    uint64_t idx = id_table_probe(id);
    for (uint64_t probes = 0; probes < ID_TABLE_SIZE; ++probes) {
        if (id_table_[idx] == nullptr || id_table_[idx] == ID_TOMBSTONE) {
            id_table_[idx] = tcb;
            return true;
        }
        idx = (idx + 1) & ID_TABLE_MASK;
    }
    return false;
}

uint64_t Scheduler::alloc_id() noexcept {
    // H-1/H-2 (audit-scheduler-tasks-v0.4.2): honor the documented contract —
    // return TASK_INVALID when the ID/table capacity is exhausted, and
    // increment the counter atomically.  Monotonic allocation with no ID
    // reuse: TASK_INVALID (UINT64_MAX) is the out-of-capacity sentinel.
    // Issue #25 C1: skip LIVE ids.  reboot_from_table() resets the counter
    // to 1 (so init keeps PID 1) while the AP idle tasks stay live
    // (spared via is_idle_task); without the skip the respawned daemons
    // steal a live AP idle's id (observed: vfsd ID 2 == apidle id 2) →
    // id_table ambiguity, enqueue_ready refusals (degraded daemons), and
    // crossed snapshot restores (AP-idle affinity clobbered → AP runs a
    // foreign task → triple fault).  restore_state()'s next_task_id_
    // rewind is covered the same way.  Bounded retries, same sentinel.
    if (all_tasks_.size() >= MAX_TASKS)
        return UINT64_MAX;
    for (uint64_t tries = 0; tries < MAX_TASKS; ++tries) {
        uint64_t cand =
            __atomic_fetch_add(&next_task_id_, 1UL, __ATOMIC_RELAXED);
        if (id_table_find(cand) == nullptr)
            return cand;
    }
    return UINT64_MAX;
}

void Scheduler::reset_next_task_id(uint64_t id) noexcept {
    __atomic_store_n(&next_task_id_, id, __ATOMIC_RELAXED);
}

void Scheduler::id_table_remove(TaskControlBlock *task) {
    uint64_t idx = id_table_probe(task->id);
    for (uint64_t probe = 0; probe < ID_TABLE_SIZE; ++probe) {
        if (id_table_[idx] == nullptr)
            return;
        if (id_table_[idx] != ID_TOMBSTONE && id_table_[idx] == task) {
            id_table_[idx] = ID_TOMBSTONE;
            return;
        }
        idx = (idx + 1) & ID_TABLE_MASK;
    }
}

TaskControlBlock *Scheduler::id_table_find(uint64_t id) {
    // Issue #25 C1: bounded probe (insert/remove already bound theirs).
    // The old unbounded `while (!= nullptr)` wrapped forever when the
    // table held no nullptr on the probe chain (live entries + tombstones
    // fill it over long suites) and the id was absent — alloc_id's
    // skip-live scan stepped straight into it (test 309 hang: 80-slot
    // table, 127 historical ids).  A present id is always found within
    // ID_TABLE_SIZE probes; absent ids now return null.
    uint64_t idx = id_table_probe(id);
    for (uint64_t probes = 0; probes < ID_TABLE_SIZE; ++probes) {
        if (id_table_[idx] == nullptr)
            return nullptr;
        if (id_table_[idx] != ID_TOMBSTONE && id_table_[idx]->id == id)
            return id_table_[idx];
        idx = (idx + 1) & ID_TABLE_MASK;
    }
    return nullptr;
}

SchedulerError Scheduler::alloc_id_err(uint64_t &out_id) {
    // H-1/H-4: same table-full contract as alloc_id(); error-returning form.
    if (all_tasks_.size() >= MAX_TASKS)
        return SCHED_ERR_TABLE_FULL;
    out_id = __atomic_fetch_add(&next_task_id_, 1UL, __ATOMIC_RELAXED);
    return SCHED_ERR_OK;
}

}  // namespace kernel
