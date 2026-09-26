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

/// @file debug_regs.hpp
/// @brief Debugger register codecs (issue #225). See debug_regs.cpp.

#pragma once

#include <types.hpp>

namespace kernel {
struct TaskControlBlock;
}

namespace kernel::debug {

/// @brief GDB register blob size in bytes for this arch (164/272/264).
size_t debug_blob_bytes() noexcept;

/// @brief U-trap frame slot base for a task (kstack_top - frame size),
///        or nullptr when the task has no kernel stack (yet).
uint64_t *debug_frame_slot(const TaskControlBlock &tcb) noexcept;

/// @brief True when the slot holds a user-mode trap frame (arch magic
///        check). Fail-closed gate for all codec reads/writes.
bool debug_frame_is_user(const uint64_t *frame) noexcept;

/// @brief Read the task's user registers into blob_out (qwords).
/// @return false when the slot is missing/non-user or the blob is short.
bool debug_read_regs(const TaskControlBlock &tcb, uint64_t *blob_out,
                     size_t blob_qwords) noexcept;

/// @brief Write blob_in into the task's live U-trap frame (takes effect
///        on resume — the return path restores from that frame).
/// @return false when the slot is missing/non-user or the blob is short.
bool debug_write_regs(TaskControlBlock &tcb, const uint64_t *blob_in,
                      size_t blob_qwords) noexcept;

/// @brief Arm a single step in the task's live U-trap frame (issue #226):
///        x86 sets TF (RFLAGS bit 8, tick or syscall frame slot) with IF
///        masked across the one instruction. aarch64/RISC-V report false —
///        the caller emulates via a temp breakpoint instead (AArch64 is
///        fixed-4B so pc+4 is exact; RISC-V decodes RVC; neither depends
///        on the QEMU/silicon single-step debug model).
/// @return false when the slot is missing/non-user or the arch emulates.
bool debug_step_arm(TaskControlBlock &tcb) noexcept;

/// @brief Disarm a step armed by debug_step_arm (issue #226): clears TF
///        and restores IF. Fail-open true when the frame is gone (target
///        died — nothing to clear, and completion must still proceed).
void debug_step_disarm(TaskControlBlock &tcb) noexcept;

} // namespace kernel::debug
