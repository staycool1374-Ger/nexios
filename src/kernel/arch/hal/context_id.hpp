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

/// @file context_id.hpp
/// @brief Architecture-neutral context-ID (PCID/ASID) API (issue #352).
///
/// Thin wrappers over the per-arch allocators so generic kernel code
/// (task create/cleanup, scheduler root publication) never branches on
/// the architecture:
/// - x86_64 delegates to `pcid_*` (`arch/x86_64/hal/pcid.hpp`).
/// - aarch64 delegates to `asid_*` (`arch/aarch64/hal/asid.hpp`, 8-bit).
/// - riscv64 delegates to `asid_*` (`arch/riscv64/hal/asid.hpp`, 16-bit).
///
/// ID 0 (`CONTEXT_ID_KERNEL`) is reserved for the kernel root on every
/// arch and means "untagged" to `context_id_tag`.  A live ID is never
/// reissued by any backend (bitmap-checked).

#pragma once

#include <types.hpp>

#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/pcid.hpp>
#elif defined(CONFIG_ARCH_AARCH64)
#include <kernel/arch/aarch64/hal/asid.hpp>
#elif defined(CONFIG_ARCH_RISCV64)
#include <kernel/arch/riscv64/hal/asid.hpp>
#endif

namespace arch {

/// @brief Context ID reserved for the kernel root (never allocated).
inline constexpr uint16_t CONTEXT_ID_KERNEL = 0;

/// @brief Probe once at boot (BSP).
inline void context_id_init() {
#if defined(CONFIG_ARCH_X86_64)
    pcid_init();
#else
    asid_init();
#endif
}

/// @brief True when context-ID tagging is active.
inline bool context_id_supported() {
#if defined(CONFIG_ARCH_X86_64)
    return pcid_supported();
#else
    return asid_supported();
#endif
}

/// @brief Allocate a user context ID, or 0 when unsupported/exhausted
///        (caller falls back untagged).  A live ID is never reissued.
inline uint16_t context_id_alloc() {
#if defined(CONFIG_ARCH_X86_64)
    return pcid_alloc();
#else
    return asid_alloc();
#endif
}

/// @brief Release a user context ID (0 is a no-op).  Purges the ID's
///        entries before returning it to the pool.
inline void context_id_free(uint16_t id) {
#if defined(CONFIG_ARCH_X86_64)
    pcid_free(id);
#else
    asid_free(id);
#endif
}

/// @brief Tag a page-table root phys with a context ID for publication
///        (0 → raw phys).  Returns the FULL register value to publish
///        (CR3 / TTBR0_EL1 / SATP).
inline uint64_t context_id_tag(uint64_t root_phys, uint16_t id) {
#if defined(CONFIG_ARCH_X86_64)
    return pcid_tag(root_phys, id);
#else
    return asid_tag(root_phys, id);
#endif
}

/// @brief Current rollover epoch (diagnostic only).
inline uint64_t context_id_epoch() {
#if defined(CONFIG_ARCH_X86_64)
    return pcid_epoch();
#else
    return asid_epoch();
#endif
}

} // namespace arch
