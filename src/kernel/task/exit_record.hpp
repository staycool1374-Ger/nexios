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

#pragma once

/// @file exit_record.hpp
/// @brief Post-mortem user-task exit records (issue #294). A reaped task
///        takes its TCB to the grave; this ring keeps the numbers that
///        `monstat user` shows after the task is gone. Static POD, no
///        allocation, ResourceTracker-neutral. Daemon exits never enter
///        (same PID rule as the monstat finder).

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace kernel {

struct TaskControlBlock;

/// @brief Snapshot of a dead user task's outcome + totals (issue #294).
///        Only fields meaningful post-mortem: live-only state (stack
///        addresses, queue flags, watchdog, signals, debug) is dropped.
struct TaskExitRecord {
    uint64_t id = 0;
    char name[CONFIG_TASK_NAME_LEN]{};
    uint64_t exit_code = 0;
    uint64_t end_tick = 0;
    uint64_t executed_ticks = 0;
    uint64_t exec_ns_total = 0;
    uint64_t exec_period_ns = 0;
    uint32_t util_per_mille = 0;
    uint64_t deadline_meets = 0;
    uint64_t deadline_miss_count = 0;
    uint64_t wcet_observed_ns = 0;
    uint64_t mem_alloc_ops_ = 0;
    uint64_t mem_free_ops_ = 0;
    uint64_t memory_used_pages_ = 0;
    bool valid = false;
};

/// @brief Ring depth: last 4 user exits, oldest overwritten.
constexpr size_t kTaskExitRingDepth = 4;

/// @brief Record a user task's exit (issues #294). Called from
///        TaskControlBlock::cleanup() start — the universal death
///        funnel. Skips non-user tasks and daemons (vfsd/iocd/watchdogd
///        PIDs); pinned baseline blocks never reach here (early return
///        above the call site). Scalar copy only, no allocation, no
///        locks (atomic slot claim, record_task_entry precedent).
/// @param t Dying task (still fully populated).
/// @param end_tick Timer::ticks() at record (caller-provided,
///        test-injectable).
void idle_record_task_exit(const TaskControlBlock &t,
                           uint64_t end_tick) noexcept;

/// @brief Most-recent valid ring entry that is not a current daemon.
/// @return Pointer to static storage, or nullptr when empty.
const TaskExitRecord *idle_find_exit_record_for_user() noexcept;

/// @brief Ring entry for a specific task id (post-mortem `monstat <id>`).
/// @return Pointer to static storage, or nullptr when absent.
const TaskExitRecord *idle_find_exit_record(uint64_t id) noexcept;

/// @brief Test-only reset: invalidate all ring slots.
void idle_clear_exit_records() noexcept;

} // namespace kernel
