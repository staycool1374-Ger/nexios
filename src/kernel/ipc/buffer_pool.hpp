#pragma once

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

/// @file buffer_pool.hpp
/// @brief Zero-copy buffer pool — allocate, transfer, map, free buffers via
/// handles.

#include <types.hpp>
#include <constants.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/task/task.hpp>
#include <kernel/ipc/buffer_pool_errors.hpp>
#include <kernel/nexios_config.h>

namespace kernel {

class BufferPool {
  public:
    static constexpr size_t MAX_BUFFERS = 1024;
    static constexpr size_t BUFFER_SIZE = arch::PAGE_SIZE;
    static constexpr size_t POOL_PAGES = CONFIG_BUFFER_POOL_PAGES;

    static constexpr int64_t LIST_EMPTY = -1;
    static constexpr int64_t BUF_INVALID_HANDLE = -2;
    static constexpr int64_t BUF_INVALID_INDEX = -3;

    /// @brief A single buffer-pool entry.
    struct Entry {
        uint64_t
            phys_addr; ///< Physical address of the backing page (0 = free).
        uint32_t generation; ///< Cookie incrementing on each alloc
                             ///< (stale-handle detection).
        uint32_t refcount;   ///< Reference count (unused currently, reserved).
        uint32_t owner_task; ///< Task ID of the current owner.
        uint64_t mapped_va;  ///< Virtual address where mapped (0 = unmapped).
        int32_t list_prev;   ///< Previous entry in per-task linked list.
        int32_t
            list_next; ///< Next entry in per-task linked list (or free-list).

        Entry()
            : phys_addr(0), generation(0), refcount(0), owner_task(0),
              mapped_va(0), list_prev(-1), list_next(-1) {
        }
    };

    /// @brief Initialize the buffer pool (free list, slab allocator).
    static void init();

    /// @brief Allocate a buffer, map it at @p virt_addr in the given
    /// task's address space.
    /// @return handle, or 0 on failure.
    static uint64_t alloc(TaskControlBlock &task, uint64_t virt_addr);

    /// @brief Free a buffer: unmap, deref, return to pool.
    static bool free(TaskControlBlock &task, uint64_t handle);

    /// @brief Map an existing (transferred) buffer at @p virt_addr in
    /// the caller's space.
    static bool map(TaskControlBlock &task, uint64_t handle,
                    uint64_t virt_addr);

    /// @brief Unmap a buffer without freeing it.
    static bool unmap(TaskControlBlock &task, uint64_t handle);

    /// @brief Transfer ownership to another task (used by IPC send).
    static bool transfer(uint64_t handle, TaskControlBlock &from,
                         TaskControlBlock &target_task);

    /// @brief Unmap all buffers owned by a task (called from cleanup/exec).
    static void unmap_all(TaskControlBlock &task);

    /// @brief Validate a handle and return its index, or -1.
    static int32_t validate(uint64_t handle);

    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static Entry entries[MAX_BUFFERS];

    /// @brief Red-zone guard word placed IMMEDIATELY AFTER `entries[]`.
    /// BUGS.md#020: an out-of-bounds `entries[idx]` write (idx >= MAX_BUFFERS)
    /// lands here and corrupts this sentinel instead of adjacent globals
    /// (scheduler/TCB structures).  Checked on every BufferPool op so the
    /// overflow is caught at its source with a precise message.
    static constexpr uint64_t entries_guard_ = 0xCACACACACACACACAULL;
    static constexpr uint64_t ENTRIES_GUARD_MAGIC = 0xCACACACACACACACAULL;

    /// @name Test-isolation helpers (snapshot / restore)
    /// @brief Copy all entries + free_head + next_cookie into flat buffer.
    static void capture_state(uint8_t *dst, size_t max_bytes);
    /// @brief Restore entries + free_head + next_cookie from flat buffer.
    static void restore_state(const uint8_t *src, size_t max_bytes);
    /// @brief Bytes needed for BufferPool state capture.
    static constexpr size_t state_bytes() {
        return sizeof(entries) + sizeof(free_head_) + sizeof(next_cookie_) +
               sizeof(pool_count_) + sizeof(pool_pages_);
    }
    /// @brief Debug-only: current cached page count (v0.3.11 leak pinning).
    static size_t pool_count_debug() noexcept { return pool_count_; }
    /// @brief Debug-only: read the i-th cached page phys (v0.3.11).
    static uint64_t pool_page_debug(size_t i) noexcept {
        return (i < POOL_PAGES) ? pool_pages_[i] : 0;
    }

  private:
    static constinit int32_t
        free_head_; ///< Head of the free-entry linked list.
    static constinit uint32_t
        next_cookie_; ///< Monotonically increasing cookie counter.

    /// @brief Pre-allocated physical pages for buffer data (no dynamic PMM).
    static uint64_t pool_pages_[POOL_PAGES];
    static constinit size_t pool_count_;

    /// @brief Pop a page from the pre-allocated pool.
    static uint64_t alloc_page();
    /// @brief Return a page to the pre-allocated pool.
    static void free_page(uint64_t phys);

    /// @brief Allocate an entry from the free list.
    static int32_t alloc_entry();
    /// @brief Return an entry to the free list.
    static void free_entry(int32_t idx);
};

} // namespace kernel
