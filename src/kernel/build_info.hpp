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

/// @file build_info.hpp
/// @brief Compile-time build identity (issue #357): architecture, build
///        type, debug flag, pointer width, and framebuffer backend.
///        Values arrive as bare preprocessor tokens from the Makefile
///        (`-DNEXIOS_ARCH_NAME=$(ARCH)`, `-DBUILD_TYPE=debug|release` —
///        never quoted, per the #244 rule) and are stringified here.
///        Constexpr-only: no runtime state, no thread interaction.

#include <types.hpp>

#ifndef NEXIOS_ARCH_NAME
#define NEXIOS_ARCH_NAME unknown
#endif
#ifndef BUILD_TYPE
#define BUILD_TYPE unknown
#endif

#define NEXIOS_BUILD_STR_INNER(x) #x
#define NEXIOS_BUILD_STR(x) NEXIOS_BUILD_STR_INNER(x)

namespace kernel::build_info {

/// @brief Bounded string equality (16 chars — arch/type names are short).
constexpr bool streq(const char *left, const char *right) {
    for (int idx = 0; idx < 16; ++idx) {
        if (left[idx] != right[idx])
            return false;
        if (left[idx] == '\0')
            return true;
    }
    return left[16] == '\0' && right[16] == '\0';
}

inline constexpr const char *arch_name = NEXIOS_BUILD_STR(NEXIOS_ARCH_NAME);
inline constexpr const char *build_type = NEXIOS_BUILD_STR(BUILD_TYPE);

#if defined(CONFIG_DEBUG)
inline constexpr bool debug_build = true;
#else
inline constexpr bool debug_build = false;
#endif

inline constexpr unsigned bits = sizeof(void *) * 8;

#if defined(CONFIG_ARCH_X86_64)
inline constexpr const char *fb_backend = "multiboot2";
inline constexpr bool fb_compiled_in = true;
#else
inline constexpr const char *fb_backend = "none";
inline constexpr bool fb_compiled_in = false;
#endif

static_assert(bits == 64);

#if defined(CONFIG_ARCH_X86_64)
static_assert(streq(arch_name, "x86_64"));
#elif defined(CONFIG_ARCH_AARCH64)
static_assert(streq(arch_name, "aarch64"));
#elif defined(CONFIG_ARCH_RISCV64)
static_assert(streq(arch_name, "riscv64"));
#endif

#if defined(CONFIG_DEBUG)
static_assert(streq(build_type, "debug"));
#else
static_assert(streq(build_type, "release"));
#endif

} // namespace kernel::build_info
