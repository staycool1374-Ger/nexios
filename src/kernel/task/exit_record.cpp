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

/// @file exit_record.cpp
/// @brief Post-mortem user-task exit records (issue #294). Static ring,
///        atomic slot claim (record_task_entry precedent), no locks, no
///        allocation. Daemon + kernel exits never enter.

#include <kernel/task/exit_record.hpp>
#include <kernel/task/task.hpp>
#include <kernel/vfs/vfsd.hpp>
#include <kernel/driver/iocd.hpp>
#include <kernel/watchdog/watchdogd.hpp>
#include <kernel/arch/hal/timer.hpp>

namespace kernel {

namespace {

TaskExitRecord g_exit_ring[kTaskExitRingDepth] = {};
uint64_t g_exit_idx = 0;

} // namespace

void idle_record_task_exit(const TaskControlBlock &t,
                           uint64_t end_tick) noexcept {
    if (!t.is_user_)
        return;
    if (t.id == vfsd::get_vfsd_pid() || t.id == iocd::get_iocd_pid() ||
        t.id == watchdogd::get_watchdogd_pid())
        return;
    const size_t slot = __atomic_fetch_add(&g_exit_idx, 1ULL,
                                           __ATOMIC_RELAXED) %
                        kTaskExitRingDepth;
    TaskExitRecord &r = g_exit_ring[slot];
    r.id = t.id;
    __builtin_strncpy(r.name, t.name, CONFIG_TASK_NAME_LEN - 1);
    r.name[CONFIG_TASK_NAME_LEN - 1] = '\0';
    r.exit_code = t.exit_code;
    r.end_tick = end_tick;
    r.executed_ticks = t.executed_ticks;
    r.exec_ns_total = t.exec_ns_total;
    r.exec_period_ns = t.exec_period_ns;
    uint64_t util = t.util_per_mille;
    r.util_per_mille = (util > 1000) ? 1000u : static_cast<uint32_t>(util);
    r.deadline_meets = t.deadline_meets;
    r.deadline_miss_count = t.deadline_miss_count;
    r.wcet_observed_ns = t.wcet_observed_ns;
    r.mem_alloc_ops_ = t.mem_alloc_ops_;
    r.mem_free_ops_ = t.mem_free_ops_;
    r.memory_used_pages_ = t.memory_used_pages_;
    r.valid = true;
}

const TaskExitRecord *idle_find_exit_record_for_user() noexcept {
    const TaskExitRecord *best = nullptr;
    for (size_t i = 0; i < kTaskExitRingDepth; ++i) {
        const TaskExitRecord &r = g_exit_ring[i];
        if (!r.valid)
            continue;
        if (r.id == vfsd::get_vfsd_pid() ||
            r.id == iocd::get_iocd_pid() ||
            r.id == watchdogd::get_watchdogd_pid())
            continue;
        if (best == nullptr || r.end_tick >= best->end_tick)
            best = &r;
    }
    return best;
}

const TaskExitRecord *idle_find_exit_record(uint64_t id) noexcept {
    const TaskExitRecord *best = nullptr;
    for (size_t i = 0; i < kTaskExitRingDepth; ++i) {
        const TaskExitRecord &r = g_exit_ring[i];
        if (!r.valid || r.id != id)
            continue;
        if (best == nullptr || r.end_tick >= best->end_tick)
            best = &r;
    }
    return best;
}

void idle_clear_exit_records() noexcept {
    for (size_t i = 0; i < kTaskExitRingDepth; ++i)
        g_exit_ring[i].valid = false;
}

} // namespace kernel
