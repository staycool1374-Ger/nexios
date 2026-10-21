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
/// @brief Address-space identifier allocator (issue #352, riscv64 only).
///
/// ASIDs tag TLB entries so SATP switches between user address spaces
/// retain translations instead of flushing.  ASID 0 is reserved for the
/// kernel; user address spaces allocate 1..65535 (full 16-bit range).
/// On exhaustion the allocator bumps an epoch, issues one global flush,
/// and restarts from 1 — a live ID is never reissued (bitmap-checked).
///
/// Mutation happens at task create/cleanup only (never on the timer-ISR or
/// context-switch hot path).  Purge-before-free: the freed ID's entries are
/// flushed before the ID returns to the pool.

#pragma once

#include <types.hpp>

namespace arch {

/// @brief Maximum ASID value (16-bit field, 0 reserved for kernel).
inline constexpr uint16_t ASID_MAX = 65535;
/// @brief ASID reserved for the kernel (never allocated).
inline constexpr uint16_t ASID_KERNEL = 0;

/// @brief Probe once at boot (BSP).  SATP MODE Sv39 always carries ASID.
void asid_init();
/// @brief True when ASID tagging is active.
bool asid_supported();
/// @brief Allocate a user ASID (1..65535), or 0 when unsupported.
///        On exhaustion: epoch bump + one global flush, cursor reset to 1
///        (freed IDs are reused low-first); returns 0 if every ID is still
///        live — the caller falls back untagged.  A live ID is never
///        reissued.
uint16_t asid_alloc();
/// @brief Release a user ASID (0 is a no-op).  Purges the ID's entries
///        before returning it to the pool.
void asid_free(uint16_t asid);
/// @brief Tag a SATP root phys with an ASID for publication (0 → raw phys).
///        ASID occupies SATP bits [59:44].
inline uint64_t asid_tag(uint64_t satp_phys, uint16_t asid) {
    uint64_t ppn = (satp_phys >> 12) & 0xFFFFFFFFFFFULL;
    uint64_t v = (8ULL << 60) | ppn;
    if (asid == ASID_KERNEL)
        return v;
    return v | ((static_cast<uint64_t>(asid) & 0xFFFFULL) << 44);
}
/// @brief Current rollover epoch (diagnostic only).
uint64_t asid_epoch();

} // namespace arch
