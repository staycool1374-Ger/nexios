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

/// @file asid.hpp
/// @brief Address-space identifier allocator (issue #352, aarch64 only).
///
/// ASIDs tag TLB entries so TTBR0 switches between user address spaces
/// retain translations instead of flushing.  ASID 0 is reserved for the
/// kernel (TTBR1); user address spaces allocate 1..255 (safe 8-bit range —
/// wider hardware ASIDs are deliberately unused).  On exhaustion the
/// allocator bumps an epoch, issues one global flush, and restarts from 1 —
/// a live ID is never reissued (bitmap-checked).
///
/// Mutation happens at task create/cleanup only (never on the timer-ISR or
/// context-switch hot path).  Purge-before-free: the freed ID's entries are
/// flushed before the ID returns to the pool.

#pragma once

#include <types.hpp>

namespace arch {

/// @brief Maximum ASID value (8-bit safe range, 0 reserved for kernel).
inline constexpr uint16_t ASID_MAX = 255;
/// @brief ASID reserved for the kernel (TTBR1, never allocated).
inline constexpr uint16_t ASID_KERNEL = 0;

/// @brief Probe once at boot (BSP).  ASIDs always exist on AArch64.
void asid_init();
/// @brief True when ASID tagging is active.
bool asid_supported();
/// @brief Allocate a user ASID (1..255), or 0 when unsupported.
///        On exhaustion: epoch bump + one global flush, cursor reset to 1
///        (freed IDs are reused low-first); returns 0 if every ID is still
///        live — the caller falls back untagged.  A live ID is never
///        reissued.
uint16_t asid_alloc();
/// @brief Release a user ASID (0 is a no-op).  Purges the ID's entries
///        before returning it to the pool.
void asid_free(uint16_t asid);
/// @brief Tag a TTBR0 phys with an ASID for publication (0 → raw phys).
///        ASID occupies TTBR0_EL1 bits [55:48].
inline uint64_t asid_tag(uint64_t ttbr0_phys, uint16_t asid) {
    if (asid == ASID_KERNEL)
        return ttbr0_phys;
    return (ttbr0_phys & ~(0xFFULL << 48)) |
           ((static_cast<uint64_t>(asid) & 0xFFULL) << 48);
}
/// @brief Current rollover epoch (diagnostic only).
uint64_t asid_epoch();

} // namespace arch
