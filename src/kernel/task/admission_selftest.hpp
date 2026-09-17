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

/// @file admission_selftest.hpp
/// @brief Boot-time admission self-test entry point (issue #24).

#pragma once

namespace kernel {

/// @brief Verifies the admission gate math + budget gate + miss-action
///        configuration during kernel init (issue #24).
///
/// Contract: call once, after Scheduler::init() (scheduler + PMM/VMM +
/// memory budget live) and before reboot_from_table() (tables still
/// quiescent: idle [+ monitor] only).  Single BSP.  Performs ZERO
/// allocation and ZERO scheduler-table mutation — synthetic stack TCBs
/// are probed through the read-only admission helpers and never added;
/// the budget check is a failed-reserve probe that cannot mutate the
/// counter.  Plain bool return (no test macros in the boot path): true
/// logs one success line; false logs per-check errors and lets boot
/// continue (fail-closed diagnostics — a diagnostic must never wedge
/// the boot it guards).
bool admission_boot_selftest() noexcept;

} // namespace kernel
