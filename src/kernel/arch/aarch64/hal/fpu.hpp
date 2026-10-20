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

/// @file fpu.hpp
/// @brief AArch64 FP/SIMD (NEON) context support (issue #353).
///
/// Save-area layout (528 bytes, 16-byte multiple):
///   [0..512)   Q0-Q31 (32 x 16 bytes, stp order)
///   [512..516) FPCR (w)
///   [516..520) FPSR (w)
///   [520..528) reserved (zero)
///
/// Model: EAGER save/restore at task switch (unlike x86 lazy #NM).
/// Rationale: no kernel -mgeneral-regs-only build change is required —
/// save-before-restore ordering inside one asm block preserves compiler
/// temporaries via memory, and only integer regs are used as scratch.

#pragma once

#include <types.hpp>

namespace arch {

/// @brief FP/SIMD save-area size in bytes (528).
inline constexpr uint64_t FPU_STATE_AREA_SIZE = 528;
/// @brief Required save-area alignment.
inline constexpr uint64_t FPU_STATE_AREA_ALIGN = 64;
/// @brief FPCR offset within the area.
inline constexpr uint64_t FPU_FPCR_OFFSET = 512;
/// @brief FPSR offset within the area.
inline constexpr uint64_t FPU_FPSR_OFFSET = 516;

/// @brief Probe FP+SIMD via ID_AA64PFR0_EL1 (mirrors has_fpu).
bool fpu_supported();
/// @brief True once fpu_init succeeded (audit S3: the switch block
///        gates on this so absent-HW configs never execute FP insns).
bool fpu_available();
/// @brief Enable FP/SIMD (CPACR.FPEN) + loud log; idempotent.
void fpu_init();
/// @brief Save Q0-Q31 + FPCR/FPSR into area (528 bytes).
void fpu_save(void *area);
/// @brief Restore Q0-Q31 + FPCR/FPSR from area.
void fpu_restore(const void *area);
/// @brief Ensure FP is enabled for the following save/restore (issue
///        #353 step 1b safety net for stale frames).  No-op on aarch64:
///        CPACR_EL1.FPEN is global and never restored per-task, so once
///        fpu_init runs it stays enabled.
inline void fpu_ensure_enabled() {}
/// @brief Eager switch helper (issue #353 step 1b): save current regs to
///        old_area, then restore next regs from new_area, back-to-back.
///        Pure memory operation (no TCB dependency — the caller owns
///        gating, generation, and used-flag updates).  Save-first order
///        preserves compiler temporaries via memory.
inline void fpu_switch_area(void *old_area, const void *new_area) {
    fpu_save(old_area);
    fpu_restore(new_area);
}

} // namespace arch
