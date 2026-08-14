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

/// @file ring_buffer.cpp
/// @brief KlogService singleton — kernel console character ring.

#include <kernel/log/ring_buffer.hpp>

namespace kernel {
namespace log {

/// @brief The one and only KlogService (Meyers singleton). Defined here so the
/// ring has no external linkage — all access flows through instance().
KlogService &KlogService::instance() noexcept {
    static KlogService service{};
    return service;
}

} // namespace log
} // namespace kernel
