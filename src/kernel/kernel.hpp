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

/// @file kernel.hpp
/// @brief Kernel entry points and global declarations.

#pragma once

#include <types.hpp>
#include <kernel/boot/bootinfo.hpp>

extern "C" {
/// @brief Entry point after transitioning to the higher half.
/// @param magic   Multiboot2 magic value.
/// @param mb_info Multiboot2 info structure pointer.
void higherhalf_entry(uint64_t magic, uint64_t mb_info);
/// @brief Kernel panic handler (noreturn).
/// @param msg Panic message string.
void panic(const char *msg) __attribute__((noreturn));
/// @brief C-level interrupt handler dispatcher.
/// @param vector     Interrupt vector number.
/// @param error_code CPU error code (0 if none).
/// @param rip        Instruction pointer at time of interrupt.
/// @param regs       Pointer to saved register array (GPRs).
void handle_interrupt_c(uint64_t vector, uint64_t error_code, uint64_t rip,
                        uint64_t *regs, uint64_t entry_tsc = 0);
}

/// @brief Kernel stack base (bottom address).
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern uint8_t kernel_stack[];

/// @brief Format wall-clock nanoseconds since epoch into "YYYY-MM-DD
/// hh:mm:ss:mmm".
/// @param buf    Output buffer (must be >= 24 bytes).
/// @param size   Buffer size.
/// @param wall_ns  Nanoseconds since 1970-01-01 00:00:00 UTC.
void format_datetime(char *buf, size_t size, uint64_t wall_ns);

namespace kernel {
class TaskControlBlock;
} // namespace kernel

/// @brief Reports a user task's end (clean exit code or fault reason) to
///        dmesg + serial log + framebuffer console (user request: an app
///        ending unexpectedly — or even cleanly, which may itself be
///        unintended — must say why).  Gated inside on user tasks outside
///        test mode (kernel-task churn and test fixtures stay quiet), so
///        call sites stay unconditional.
/// @param task         The ending task (name/id read here).
/// @param clean        True for sys_exit (code_or_sig = exit status),
///                     false for signal/fault death (code_or_sig = signal).
/// @param code_or_sig  Exit status or signal number.
/// @param vector       CPU fault vector for cause text (x86 fault path);
///                     0 when not applicable (generic "fault" wording).
void report_user_task_end(kernel::TaskControlBlock &task, bool clean,
                          uint64_t code_or_sig, uint64_t vector);
