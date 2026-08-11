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

/// @file version.hpp
/// @brief Kernel version information.

#pragma once

#include <types.hpp>

/// @brief Kernel version — single source of truth.
/// Format: "vmajor.minor.patch-stage"
#define KERNEL_VERSION_STRING "v0.3.13-dev"

namespace kernel {

/// @brief Kernel version constants and string formatting.
/// @note Major/minor/patch and build timestamp are defined at compile time.
struct Version {
    static constexpr unsigned major = 0;
    static constexpr unsigned minor = 3;
    static constexpr unsigned patch = 13;
    static constexpr const char* stage = "dev";

    static const char* string();
    static const char* build_date();
    static const char* build_time();
    static const char* full_string();
};

} // namespace kernel