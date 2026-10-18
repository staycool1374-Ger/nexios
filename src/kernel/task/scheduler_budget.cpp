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
    // Boot-time single-threaded init; relaxed store (issue #375).
    __atomic_store_n(&memory_budget_pages_, total_pages, __ATOMIC_RELAXED);
}

bool Scheduler::reserve_memory_pages(uint64_t count) noexcept {
    // Issue #375: lock-free CAS reservation — the check-then-act must be
    // one atomic step or two CPUs over-commit the budget past zero.
    uint64_t cur =
        __atomic_load_n(&memory_budget_pages_, __ATOMIC_ACQUIRE);
    while (cur >= count) {
        if (__atomic_compare_exchange_n(&memory_budget_pages_, &cur,
                                        cur - count, false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return true;
    }
    return false;
}

void Scheduler::release_memory_pages(uint64_t count) noexcept {
    __atomic_fetch_add(&memory_budget_pages_, count, __ATOMIC_RELAXED);
}

uint64_t Scheduler::remaining_memory_budget() noexcept {
    return __atomic_load_n(&memory_budget_pages_, __ATOMIC_RELAXED);
}
#endif
bool Scheduler::charge_task_memory(uint64_t pages) {
    auto *cur = current_task();
    if (!cur || cur->memory_budget_pages_ == 0)
        return true;  // unlimited
    // Issue #375: atomic check-then-charge (exit_record/reap may read
    // memory_used_pages_ from another CPU — §11.6 all-or-nothing).
    uint64_t used =
        __atomic_load_n(&cur->memory_used_pages_, __ATOMIC_ACQUIRE);
    while (true) {
        if (used + pages > cur->memory_budget_pages_)
            return false;  // over budget
        if (__atomic_compare_exchange_n(&cur->memory_used_pages_, &used,
                                        used + pages, false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            break;
    }
    // Issue #284: attribution ops-counter (always on, saturating).
    uint64_t ops =
        __atomic_load_n(&cur->mem_alloc_ops_, __ATOMIC_RELAXED);
    while (ops < UINT64_MAX) {
        if (__atomic_compare_exchange_n(&cur->mem_alloc_ops_, &ops,
                                        ops + 1, false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            break;
    }
    return true;
}

void Scheduler::credit_task_memory(uint64_t pages) {
    auto *cur = current_task();
    if (!cur)
        return;
    uint64_t used =
        __atomic_load_n(&cur->memory_used_pages_, __ATOMIC_ACQUIRE);
    while (true) {
        if (used == 0)
            return;
        uint64_t next = (pages > used) ? 0 : used - pages;
        if (__atomic_compare_exchange_n(&cur->memory_used_pages_, &used,
                                        next, false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            break;
    }
    // Issue #284: free-ops bump lives ONLY here (PMM::free_page is
    // globally unattributed — freeing task != allocating task).
    uint64_t ops =
        __atomic_load_n(&cur->mem_free_ops_, __ATOMIC_RELAXED);
    while (ops < UINT64_MAX) {
        if (__atomic_compare_exchange_n(&cur->mem_free_ops_, &ops,
                                        ops + 1, false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            break;
    }
}

}  // namespace kernel
