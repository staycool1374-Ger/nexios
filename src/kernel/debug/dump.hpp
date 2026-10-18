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

/// @file dump.hpp
/// @brief Diagnostic dump functions for kernel debugging.
///        All functions use arch::IrqGuard internally to disable IRQs during
///        output. Output goes to serial via
///        Logger::raw_write/print_hex/print_dec.

#include <types.hpp>

namespace kernel {
namespace debug {

/// @brief Dump all scheduler state (indices, counts, flags, deferred-switch
/// globals).
void dump_scheduler_info();

/// @brief Dump all CPU general-purpose registers + CR0/CR2/CR3/CR4.
/// @note x86_64 only; other architectures print a stub.
void dump_cpu_regs();

/// @brief Dump all fields of a single task identified by ID.
/// @param task_id  Task ID to dump.
void dump_task_info(uint64_t task_id);

/// @brief Dump all valid tasks in the scheduler array.
void dump_all_tasks();

/// @brief Record a task's entry (code address) + TCB address for BUGS.md#020
/// wild-write forensics.
void record_task_entry(uint64_t entry, uint64_t tcb);

/// @brief If `value` matches a recently-created task's `entry`, return that
/// task's TCB address; else 0.
uint64_t find_entry_owner(uint64_t value);

} // namespace debug
} // namespace kernel
