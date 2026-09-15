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

/// @file pcid.hpp
/// @brief Process-context identifier allocator (issue #156, x86_64 only).
///
/// PCIDs tag TLB entries so CR3 switches between address spaces retain
/// translations instead of flushing.  PCID 0 is reserved for the kernel
/// PML4 fallback; user address spaces allocate 1..4095.  On exhaustion
/// the allocator bumps an epoch, issues one global flush, and restarts
/// from 1 — a live ID is never reissued (bitmap-checked).
///
/// Mutation happens at task create/cleanup only (never on the
/// timer-ISR or context-switch hot path).  Snapshot save/restore hooks
/// keep test isolation deterministic.

#pragma once

#include <types.hpp>

namespace arch {

/// @brief Maximum PCID value (12-bit field, 0 reserved for kernel).
inline constexpr uint16_t PCID_MAX = 4095;
/// @brief PCID reserved for the kernel PML4 fallback (never allocated).
inline constexpr uint16_t PCID_KERNEL = 0;

/// @brief Probe CPU support once at boot (BSP).
void pcid_init();
/// @brief True when PCID tagging is active (probed + CR4.PCIDE set).
bool pcid_supported();
/// @brief Allocate a user PCID (1..4095), or 0 when unsupported.
///        On exhaustion: epoch bump + one global flush, cursor reset
///        to 1 (freed IDs are reused low-first); returns 0 if every
///        ID is still live — the caller falls back untagged.  A live
///        ID is never reissued.
uint16_t pcid_alloc();
/// @brief Release a user PCID (0 is a no-op).
void pcid_free(uint16_t pcid);
/// @brief Tag a PML4 phys with a PCID for CR3 publish (0 → raw phys).
///        Hot path: single compare; pcid_alloc() only returns nonzero
///        when tagging is active, so no support check is needed here.
inline uint64_t pcid_tag(uint64_t pml4_phys, uint16_t pcid) {
    if (pcid == PCID_KERNEL)
        return pml4_phys;
    return (pml4_phys & ~0xFFFULL) | (pcid & 0xFFFU);
}
/// @brief Current rollover epoch (diagnostic/test only).
uint64_t pcid_epoch();
/// @brief Force allocator availability ON for logic testing (issue
///        #156, D2).  Test-only: lets the bitmap/epoch/rollover paths
///        run on CPUs without PCID hardware.  DANGER: while forced, a
///        dispatched user task would publish a tagged CR3 the hardware
///        rejects (#GP) — never dispatch user tasks while forced, and
///        always close with pcid_test_reset() (plus the isolate hook).
void pcid_test_force();
/// @brief Restore hardware truth after forced testing
///        (g_supported = has_pcid()).  Called by tests at exit and by
///        test isolation on restore.
void pcid_test_reset();

} // namespace arch
