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

/// @file msr.hpp
/// @brief Model-Specific Register (MSR) read/write (x86_64) / no-op stubs
/// (AArch64, RISC-V).

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>

/// @cond
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/msr_impl.hpp>
/// @endcond
#elif defined(CONFIG_ARCH_AARCH64) || defined(CONFIG_ARCH_RISCV64)
// AArch64/RISC-V do not have x86-style MSRs; system registers use CSR directly.
// Provide empty stubs so code that conditionally calls wrmsr/rdmsr compiles.
namespace arch {
/// @brief Write to an x86 model-specific register (no-op on non-x86).
/// @param msr The MSR index.
/// @param val The value to write.
inline void wrmsr(uint32_t msr, uint64_t val) {
    (void)msr;
    (void)val;
}
/// @brief Read from an x86 model-specific register (no-op on non-x86).
/// @param msr The MSR index.
/// @return The MSR value (always 0 on non-x86).
inline uint64_t rdmsr(uint32_t msr) {
    (void)msr;
    return 0;
}
} // namespace arch
/// @cond
#else
#error "HAL: no msr implementation for this architecture"
#endif
/// @endcond
