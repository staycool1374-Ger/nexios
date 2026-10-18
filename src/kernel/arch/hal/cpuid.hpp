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

/// @file cpuid.hpp
/// @brief CPU identification — dispatches to architecture-specific cpuid
/// implementations.

#include <types.hpp>
#include <kernel/nexios_config.h>

/// @cond
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/cpuid_impl.hpp>
#elif defined(CONFIG_ARCH_AARCH64)
#include <kernel/arch/aarch64/hal/cpuid_impl.hpp>
#elif defined(CONFIG_ARCH_RISCV64)
#include <kernel/arch/riscv64/hal/cpuid_impl.hpp>
#else
#error "HAL: no cpuid implementation for this architecture"
#endif
/// @endcond
