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

/// @file scheduler_budget.cpp
/// @brief Global + per-task memory accounting for Scheduler (issue #323).
///        Pure move out of scheduler.cpp: no logic changes.

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_budget_internal.hpp>

namespace kernel {

#if CONFIG_MEMORY_BUDGET
constinit uint64_t Scheduler::memory_budget_pages_ = 0;
#endif
#if CONFIG_MEMORY_BUDGET
void Scheduler::init_memory_budget(uint64_t total_pages) noexcept {
    memory_budget_pages_ = total_pages;
}

bool Scheduler::reserve_memory_pages(uint64_t count) noexcept {
    if (memory_budget_pages_ < count)
        return false;
    memory_budget_pages_ -= count;
    return true;
}

void Scheduler::release_memory_pages(uint64_t count) noexcept {
    memory_budget_pages_ += count;
}

uint64_t Scheduler::remaining_memory_budget() noexcept {
    return memory_budget_pages_;
}
#endif
bool Scheduler::charge_task_memory(uint64_t pages) {
    auto *cur = current_task();
    if (!cur || cur->memory_budget_pages_ == 0)
        return true;  // unlimited
    if (cur->memory_used_pages_ + pages > cur->memory_budget_pages_)
        return false;  // over budget
    cur->memory_used_pages_ += pages;
    // Issue #284: attribution ops-counter (always on, saturating).
    if (cur->mem_alloc_ops_ < UINT64_MAX)
        ++cur->mem_alloc_ops_;
    return true;
}

void Scheduler::credit_task_memory(uint64_t pages) {
    auto *cur = current_task();
    if (!cur || cur->memory_used_pages_ == 0)
        return;
    if (pages > cur->memory_used_pages_)
        cur->memory_used_pages_ = 0;
    else
        cur->memory_used_pages_ -= pages;
    // Issue #284: free-ops bump lives ONLY here (PMM::free_page is
    // globally unattributed — freeing task != allocating task).
    if (cur->mem_free_ops_ < UINT64_MAX)
        ++cur->mem_free_ops_;
}

}  // namespace kernel
