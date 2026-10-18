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

/// @file assert.hpp
/// @brief Debug assertion macros and the panic handler declaration.

#pragma once

#include <types.hpp>
#include <logger.hpp>
#include <concepts.hpp>

/// @brief Triggers a kernel panic with a message (noreturn).
extern "C" void panic(const char* msg);

/// @brief Primary template for per-module error-string lookup.
///        Each module that defines error codes specialises this template
///        (e.g. <kernel/task/task_errors.hpp>).
namespace kernel::errors {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    template<kernel::ErrorEnum E>
    inline const char* error_string(E) { return "Unknown error"; }
}

/// @brief Helper: stringify without macro expansion (single level).
#define STRINGIFY(x) #x

/// @brief Helper: expand then stringify (two levels so __LINE__ expands first).
#define EXPAND_AND_STRINGIFY(x) STRINGIFY(x)

/// @brief Fatal invariant check — panics if the condition is false.
///        Use this for conditions that should *never* fail.
#define ENSURE(cond) \
    do { \
        if (!(cond)) [[unlikely]] { \
            panic("ENSURE failed: " #cond " at " __FILE__ ":" \
                  EXPAND_AND_STRINGIFY(__LINE__)); \
        } \
    } while (0)

/// @brief Non-fatal error assertion — logs the error with file/line in debug
///        builds and compiles to a no-op in release builds.
/// @param err  An enum value from a module error header; the associated
///             error message is looked up via kernel::errors::error_string().
#ifdef CONFIG_DEBUG
#define ASSERT(err) \
    do { \
        kernel::Logger::error("[%s:%u] %s", \
            __FILE__, __LINE__, \
            kernel::errors::error_string(err)); \
    } while (0)
#else
#define ASSERT(err) ((void)0)
#endif


