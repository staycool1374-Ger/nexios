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

/// @file proc_fork_carve.hpp
/// @brief Carve-composed fork composer (issue #564, F11 Stage C): builds
///        the fork child from retype + VSpace map + foreign-frame map +
///        carved-root activation, with no kernel deep copy.  Selected by
///        ProcSvc::fork under CONFIG_PROC_MGR_CARVE_FORK; the OFF build
///        keeps the kernel-composed clone statement-identical (INV-CARVE-10).

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace kernel {
struct TaskControlBlock;
} // namespace kernel

namespace kernel::proc {

#if CONFIG_PROC_MGR_CARVE_FORK
/// @brief Compose a fork child from carved objects (issue #564, F11
///        Stage C).  Runs in the forking parent's context (caller regs).
///        Returns the unpublished child (the caller applies the
///        pre-publication hold + add_task_err tail); nullptr on any
///        failure (rollback + drain already done, +22 pushed).
TaskControlBlock *carve_fork(uint64_t *regs) noexcept;
#endif

} // namespace kernel::proc
