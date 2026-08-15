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

/// @file endpoint.hpp
/// @brief Capability-backed IPC endpoint object (shared-heap KernelObject).

#pragma once

#include <types.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/task/task.hpp>
#include <kernel/sync/spinlock.hpp>

namespace kernel::cap {

/// @brief A capability-gated IPC endpoint: a heap-resident message queue plus
///        a receiver binding and badge.  Shared-heap class (PipeBuffer
///        pattern): lifetime is pinned by capability slots; dispose() runs on
///        the last reference.
///
/// @note The embedded MessageQueue is heap-resident and explicitly init()'d
///       (never deleted — Endpoint is freed via MemPool::free in dispose(),
///       so MessageQueue's destructor is not invoked; blocked senders must be
///       drained before the last release).
class Endpoint : public KernelObject {
  public:
    MessageQueue q;
    TaskControlBlock *bound_receiver = nullptr;
    uint32_t badge = 0;
    sync::SpinLock lock_;

    /// @brief Allocates an Endpoint from the MemPool, pool-marks it and
    ///        initialises the embedded queue.  Returns nullptr on failure.
    static Endpoint *create(uint32_t badge);

    /// @brief Final teardown: detaches the receiver binding and returns the
    ///        block to the MemPool.
    void dispose() noexcept override;

    /// @brief Genuinely shared (referenced by capability slots).
    bool is_shared() const noexcept override { return true; }
};

} // namespace kernel::cap
