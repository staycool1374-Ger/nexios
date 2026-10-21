#pragma once

/*
 * NexIOS RTOS — Capability-Based Access Control (CSpace)
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

/// @file irq_vectors.hpp
/// @brief Per-arch user-IRQ capability windows (issue #354 Slice B).
/// IrqCap wraps platform IRQ lines (not MSI-X vectors — those belong to
/// MsixCap on x86_64 only): x86_64 PIC IRQ1–15 (vectors 33–47; 32 is the
/// scheduler tick), aarch64 GIC SPI window (INTIDs 32–255; SGI/PPI incl.
/// the timer PPI 30 are never user-claimable), riscv64 PLIC sources
/// (1–95; 0 is the never-signalled source).

#include <types.hpp>
#include <kernel/nexios_config.h>

#if defined(CONFIG_ARCH_RISCV64)
#include <kernel/arch/riscv64/hal/plic.hpp>
#elif defined(CONFIG_ARCH_AARCH64)
#include <kernel/arch/aarch64/hal/gic.hpp>
#endif

namespace arch {

/// @brief Lowest user-claimable IRQ vector (inclusive).
inline constexpr uint16_t irq_cap_vector_min() {
#if defined(CONFIG_ARCH_X86_64)
    return 33;
#elif defined(CONFIG_ARCH_AARCH64)
    return 32;
#else
    return 1;
#endif
}

/// @brief Highest user-claimable IRQ vector (inclusive).
inline constexpr uint16_t irq_cap_vector_max() {
#if defined(CONFIG_ARCH_X86_64)
    return 47;
#elif defined(CONFIG_ARCH_AARCH64)
    return GIC_MAX_INTID - 1;
#else
    return PLIC_MAX_SOURCES - 1;
#endif
}

/// @brief True when @p vector lies in the user-claimable window.
/// @param vector Raw vector (widened to catch > 255 without truncation).
inline constexpr bool irq_cap_vector_valid(uint16_t vector) {
    return vector >= irq_cap_vector_min() && vector <= irq_cap_vector_max();
}

} // namespace arch
