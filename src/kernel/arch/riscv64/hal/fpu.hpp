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
/// @brief RISC-V 64 FP (F/D extensions) context support (issue #353).
///
/// Save-area layout (272 bytes, 8-byte multiple):
///   [0..256)   f0-f31 (32 x 8 bytes, fsd order, D width)
///   [256..260) fcsr (w)
///   [260..272) reserved (zero)
///
/// Model: EAGER save/restore at task switch (unlike x86 lazy #NM).
/// Detection is an sstatus.FS write test (MISA is M-mode-only and
/// unreadable from S-mode): FS set to 0b11 must read back 0b11.

#pragma once

#include <types.hpp>

namespace arch {

/// @brief FP save-area size in bytes (272).
inline constexpr uint64_t FPU_STATE_AREA_SIZE = 272;
/// @brief Required save-area alignment.
inline constexpr uint64_t FPU_STATE_AREA_ALIGN = 64;
/// @brief fcsr offset within the area.
inline constexpr uint64_t FPU_FCSR_OFFSET = 256;

/// @brief Probe F/D via sstatus.FS write test (MISA is M-mode-only).
bool fpu_supported();
/// @brief Enable FP (sstatus.FS=0b11) + loud log; idempotent.
void fpu_init();
/// @brief Save f0-f31 + fcsr into area (272 bytes).
void fpu_save(void *area);
/// @brief Restore f0-f31 + fcsr from area.
void fpu_restore(const void *area);

} // namespace arch
