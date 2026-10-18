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

/// @file path_tracer.cpp
/// @brief Backing store for the order-sensitive path-tracing primitive
///        (issues #347, #362).

#include <kernel/core/path_tracer.hpp>

// Zero-initialized at boot (constinit: no dynamic init); written only via
// path_reset()/path_step() on the owning CPU.
constinit uint64_t kernel::g_path_chain[CONFIG_MAX_CPUS] = {};
