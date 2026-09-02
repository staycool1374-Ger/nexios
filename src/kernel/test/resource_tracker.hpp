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

/// @file resource_tracker.hpp
/// @brief Kernel resource allocation tracking for test isolation.

#pragma once

#include <types.hpp>
#include <kernel/memory/mempool.hpp>

namespace kernel::test {

struct ResourceCounters {
    size_t mempool_used[MemPool::POOL_COUNT];
    size_t pmm_pages_used;
    size_t tasks;
    size_t bufpool_entries;
    size_t msg_queues;
    size_t notifies;
    size_t event_groups;
    size_t drivers;
    size_t pipe_buffers;
    size_t vnodes;
    size_t open_fds;
    size_t cap_objects;
    size_t cap_slots;
};

#ifdef CONFIG_DEBUG

class ResourceTracker {
  public:
    static ResourceTracker &instance() {
        static ResourceTracker inst;
        return inst;
    }

    void track_mempool_alloc(size_t pool_idx) {
        if (pool_idx < MemPool::POOL_COUNT)
            ++counters_.mempool_used[pool_idx];
    }
    void track_mempool_free(size_t pool_idx) {
        if (pool_idx < MemPool::POOL_COUNT &&
            counters_.mempool_used[pool_idx] > 0)
            --counters_.mempool_used[pool_idx];
    }
    void track_pmm_alloc(size_t pages) {
        counters_.pmm_pages_used += pages;
    }
    void track_pmm_free(size_t pages) {
        counters_.pmm_pages_used -= (counters_.pmm_pages_used >= pages)
                                        ? pages
                                        : counters_.pmm_pages_used;
    }
    void track_task_add() {
        ++counters_.tasks;
    }
    void track_task_remove() {
        if (counters_.tasks > 0)
            --counters_.tasks;
    }
    void track_bufpool_alloc() {
        ++counters_.bufpool_entries;
    }
    void track_bufpool_free() {
        if (counters_.bufpool_entries > 0)
            --counters_.bufpool_entries;
    }
    void track_msg_queue_add() {
        ++counters_.msg_queues;
    }
    void track_msg_queue_remove() {
        if (counters_.msg_queues > 0)
            --counters_.msg_queues;
    }
    void track_notify_add() {
        ++counters_.notifies;
    }
    void track_notify_remove() {
        if (counters_.notifies > 0)
            --counters_.notifies;
    }
    void track_event_group_add() {
        ++counters_.event_groups;
    }
    void track_event_group_remove() {
        if (counters_.event_groups > 0)
            --counters_.event_groups;
    }
    void track_driver_add() {
        ++counters_.drivers;
    }
    void track_driver_remove() {
        if (counters_.drivers > 0)
            --counters_.drivers;
    }
    void track_pipe_buffer_add() {
        ++counters_.pipe_buffers;
    }
    void track_pipe_buffer_remove() {
        if (counters_.pipe_buffers > 0)
            --counters_.pipe_buffers;
    }
    void track_vnode_add() {
        ++counters_.vnodes;
    }
    void track_vnode_remove() {
        if (counters_.vnodes > 0)
            --counters_.vnodes;
    }
    void track_fd_add() {
        ++counters_.open_fds;
    }
    void track_fd_remove() {
        if (counters_.open_fds > 0)
            --counters_.open_fds;
    }
    void track_cap_object_add() {
        ++counters_.cap_objects;
    }
    void track_cap_object_remove() {
        if (counters_.cap_objects > 0)
            --counters_.cap_objects;
    }
    void track_cap_slot_add() {
        ++counters_.cap_slots;
    }
    void track_cap_slot_remove() {
        if (counters_.cap_slots > 0)
            --counters_.cap_slots;
    }

    void capture(ResourceCounters &out) const {
        out = counters_;
    }

    void restore(const ResourceCounters &saved) {
        counters_ = saved;
    }

    bool check(const ResourceCounters &baseline, const char *test_name);

  private:
    ResourceCounters counters_ = {};
};

#else

class ResourceTracker {
  public:
    static ResourceTracker &instance() {
        static ResourceTracker inst;
        return inst;
    }
    void track_mempool_alloc(size_t) {
    }
    void track_mempool_free(size_t) {
    }
    void track_pmm_alloc(size_t) {
    }
    void track_pmm_free(size_t) {
    }
    void track_task_add() {
    }
    void track_task_remove() {
    }
    void track_bufpool_alloc() {
    }
    void track_bufpool_free() {
    }
    void track_msg_queue_add() {
    }
    void track_msg_queue_remove() {
    }
    void track_notify_add() {
    }
    void track_notify_remove() {
    }
    void track_event_group_add() {
    }
    void track_event_group_remove() {
    }
    void track_driver_add() {
    }
    void track_driver_remove() {
    }
    void track_pipe_buffer_add() {
    }
    void track_pipe_buffer_remove() {
    }
    void track_vnode_add() {
    }
    void track_vnode_remove() {
    }
    void track_fd_add() {
    }
    void track_fd_remove() {
    }
    void track_cap_object_add() {
    }
    void track_cap_object_remove() {
    }
    void track_cap_slot_add() {
    }
    void track_cap_slot_remove() {
    }
    void capture(ResourceCounters &) const {
    }
    void restore(const ResourceCounters &) {
    }
    bool check(const ResourceCounters &, const char *) {
        return true;
    }
};

#endif

} // namespace kernel::test
