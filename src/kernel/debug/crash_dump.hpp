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

/// @file crash_dump.hpp
/// @brief First-chance raw crash dump (issue #308, debugd §8.3): raw polled
///        register dump to UART + halt, with no Logger formatting (Logger
///        may be the casualty) and no recovery attempts. Terminal path only.

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace kernel {
namespace debug {

/// @brief Architecture that produced the first-chance frame.
enum class FirstChanceArch : uint8_t { X86_64 = 0, AARCH64 = 1, RISCV64 = 2 };

/// @brief First-chance cause: unrecoverable CPU exception or stack overflow.
enum class FirstChanceCause : uint8_t { CPU_EXCEPTION = 0, STACK_OVERFLOW = 1 };

/// @brief Arch-neutral first-chance frame. Field slots are reused per arch:
///        x86_64: addr=CR2, pc=RIP, sp=RSP, pgtbl0=CR3, pgtbl1=unused,
///        vector/error as trapped. aarch64: addr=FAR, pc=ELR, sp=SP,
///        pgtbl0=TTBR0, pgtbl1=TTBR1, vector=ESR. riscv64: addr=stval,
///        pc=sepc, sp=SP, pgtbl0=SATP, pgtbl1=unused, vector=scause.
struct FirstChanceFrame {
    FirstChanceArch arch;
    FirstChanceCause cause;
    uint64_t vector;
    uint64_t error;
    uint64_t addr;
    uint64_t pc;
    uint64_t sp;
    uint64_t pgtbl0;
    uint64_t pgtbl1;
};

/// @brief Render a frame into a bounded NUL-free byte stream (no Logger,
///        no libc, no allocation). Returns bytes written (<= cap).
size_t format_first_chance(const FirstChanceFrame &frame, char *out,
                           size_t cap) noexcept;

/// @brief Emit a frame via raw polled UART only (arch::Serial). Records the
///        frame in the debug latch when CONFIG_DEBUG is set.
void first_chance_emit(const FirstChanceFrame &frame) noexcept;

/// @brief Emit then halt. Returns only under the armed CONFIG_DEBUG seam
///        (test override); production halt never returns.
void first_chance_halt(const FirstChanceFrame &frame) noexcept;

/// @brief Faulting RSP from an x86 trap frame (issue #308): the CPU pushes
///        SS:RSP only on a privilege change (user fault, CS.RPL=3); a
///        kernel-mode fault has no pushed RSP, so report 0 rather than a
///        stack-content word. regs layout: [17]=RIP [18]=CS [19]=RFLAGS
///        [20]=RSP [21]=SS (isr_stubs.asm uniform 16-byte stub frame).
inline uint64_t x86_fault_sp(const uint64_t *regs) noexcept {
    return (regs[18] & 3) ? regs[20] : 0;
}

/// @brief Per-arch frame constructors from trap context words.
FirstChanceFrame make_x86_frame(uint64_t vector, uint64_t error,
                                uint64_t rip, uint64_t sp, uint64_t cr2,
                                uint64_t cr3) noexcept;
FirstChanceFrame make_aarch64_frame(uint64_t esr, uint64_t far, uint64_t elr,
                                    uint64_t sp, uint64_t ttbr0,
                                    uint64_t ttbr1) noexcept;
FirstChanceFrame make_riscv_frame(uint64_t scause, uint64_t sepc,
                                  uint64_t stval, uint64_t sp,
                                  uint64_t satp) noexcept;

/// @brief Capture the current register state as a frame (stack-overflow
///        path: no trap frame available). Values are the capture point's
///        state, not a faulting frame.
FirstChanceFrame first_chance_frame_here(FirstChanceCause cause) noexcept;

#if CONFIG_DEBUG
/// @brief Test seam: arm the halt override (returns instead of halting).
void test_arm_crash_halt() noexcept;
/// @brief Test seam: halt invocations since last reset.
uint64_t test_crash_halt_count() noexcept;
/// @brief Test seam: last emitted frame.
FirstChanceFrame test_last_frame() noexcept;
/// @brief Test seam: disarm + clear count and frame (snapshot restore).
void test_reset_crash_halt() noexcept;
#endif

} // namespace debug
} // namespace kernel

namespace arch {

/// @brief Halt the CPU after a first-chance dump (never returns in
///        production). Weak default; CONFIG_DEBUG test override returns
///        only when explicitly armed.
void crash_halt() noexcept;

} // namespace arch
