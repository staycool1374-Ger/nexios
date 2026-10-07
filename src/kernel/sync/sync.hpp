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

/// @file sync.hpp
/// @brief Synchronisation primitives umbrella header — includes all sync types.

#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/sync/mutex.hpp>
#include <kernel/sync/queue.hpp>
#include <kernel/sync/notify.hpp>
#include <kernel/sync/eventgroup.hpp>

namespace kernel {
namespace sync {

/// @brief Initialize all kernel synchronization primitives.
void init_all();

} // namespace sync
} // namespace kernel
