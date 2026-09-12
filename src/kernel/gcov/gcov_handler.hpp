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

/// @file gcov_handler.hpp
/// @brief Dynamic function-level coverage — executed-function table and the
/// framed serial dump consumed by tools/coverage_report.py (issue #122).

#pragma once

#include <types.hpp>

extern "C" {

/// @brief Serialise the executed-function set over COM1 (Phase B).
/// Emits a framed, checksummed record (see docs/specs/coverage.md).
/// Safe to call with interrupts disabled; never blocks unbounded.
void gcov_flush_to_serial();

/// @brief Serialise every translation unit's gcov profile over COM1 (Phase A,
/// issue #123).  Emits one "GCOV:<len>:<filename>\n<payload>" record per
/// instrumented translation unit, framed by @@GCDABEGIN@@/@@GCDAEND@@.
/// Only provided in the -fprofile-arcs build; safe with interrupts disabled.
void gcov_line_dump_to_serial();

/// @brief Run the linker-collected static constructor table so every
/// instrumented translation unit registers its gcov profile.  Must be called
/// before any profile data is needed; only provided in the -fprofile-arcs
/// build.  Bounded by the table's own sentinels.
void gcov_run_ctors();

} // extern "C"
