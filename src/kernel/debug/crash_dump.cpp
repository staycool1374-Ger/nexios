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

/// @file crash_dump.cpp
/// @brief First-chance raw crash dump (issue #308): bounded formatter
///        (no Logger/libc), arch::Serial emitter, weak halt default.

#include <kernel/debug/crash_dump.hpp>
#include <kernel/arch/serial.hpp>
#include <kernel/arch/io.hpp>
#include <types.hpp>

namespace kernel {
namespace debug {

namespace {

// Bounded helpers: no allocation, no locks, no formatting library.
void emit_str(const char *s) {
    while (*s)
        arch::Serial::putchar(*s++);
}

void emit_hex(uint64_t v) {
    emit_str("0x");
    for (int sh = 60; sh >= 0; sh -= 4)
        arch::Serial::putchar(
            "0123456789abcdef"[(v >> sh) & 0xF]);
}

size_t fmt_str(char *out, size_t cap, size_t p, const char *s) {
    while (*s && p < cap)
        out[p++] = *s++;
    return p;
}

size_t fmt_hex(char *out, size_t cap, size_t p, uint64_t v) {
    p = fmt_str(out, cap, p, "0x");
    for (int sh = 60; sh >= 0 && p < cap; sh -= 4)
        out[p++] = "0123456789abcdef"[(v >> sh) & 0xF];
    return p;
}

const char *arch_name(FirstChanceArch a) {
    switch (a) {
    case FirstChanceArch::X86_64:
        return "x86_64";
    case FirstChanceArch::AARCH64:
        return "aarch64";
    default:
        return "riscv64";
    }
}

const char *cause_name(FirstChanceCause c) {
    return (c == FirstChanceCause::STACK_OVERFLOW) ? "STACK-OVERFLOW"
                                                  : "CPU-EXCEPTION";
}

} // namespace

#if CONFIG_DEBUG
// Debug latch + last frame (this TU is the sole writer; test seam
// functions are the readers). Named scope so the weak arch::crash_halt
// below can reference them.
bool g_halt_armed = false;
uint64_t g_halt_count = 0;
FirstChanceFrame g_last_frame{FirstChanceArch::X86_64,
                              FirstChanceCause::CPU_EXCEPTION,
                              0, 0, 0, 0, 0, 0, 0};
#endif

size_t format_first_chance(const FirstChanceFrame &frame, char *out,
                           size_t cap) noexcept {
    size_t p = 0;
    p = fmt_str(out, cap, p, "\n[FIRST-CHANCE] ");
    p = fmt_str(out, cap, p, cause_name(frame.cause));
    p = fmt_str(out, cap, p, " (raw) arch=");
    p = fmt_str(out, cap, p, arch_name(frame.arch));
    // Retain the CPU EXCEPTION token for tools/gpf-debug.sh.
    p = fmt_str(out, cap, p, " CPU EXCEPTION vec=");
    p = fmt_hex(out, cap, p, frame.vector);
    p = fmt_str(out, cap, p, " err=");
    p = fmt_hex(out, cap, p, frame.error);
    p = fmt_str(out, cap, p, " addr=");
    p = fmt_hex(out, cap, p, frame.addr);
    p = fmt_str(out, cap, p, " pc=");
    p = fmt_hex(out, cap, p, frame.pc);
    p = fmt_str(out, cap, p, " sp=");
    p = fmt_hex(out, cap, p, frame.sp);
    p = fmt_str(out, cap, p, " pgtbl0=");
    p = fmt_hex(out, cap, p, frame.pgtbl0);
    p = fmt_str(out, cap, p, " pgtbl1=");
    p = fmt_hex(out, cap, p, frame.pgtbl1);
    p = fmt_str(out, cap, p, "\n");
    return p;
}

void first_chance_emit(const FirstChanceFrame &frame) noexcept {
    switch (frame.arch) {
    case FirstChanceArch::X86_64:
        emit_str("\n[FIRST-CHANCE] CPU-EXCEPTION (raw) arch=x86_64 CPU "
                 "EXCEPTION");
        break;
    case FirstChanceArch::AARCH64:
        emit_str("\n[FIRST-CHANCE] CPU-EXCEPTION (raw) arch=aarch64 CPU "
                 "EXCEPTION");
        break;
    default:
        emit_str("\n[FIRST-CHANCE] CPU-EXCEPTION (raw) arch=riscv64 CPU "
                 "EXCEPTION");
        break;
    }
    if (frame.cause == FirstChanceCause::STACK_OVERFLOW)
        emit_str(" [STACK-OVERFLOW]");
    emit_str(" vec=");
    emit_hex(frame.vector);
    emit_str(" err=");
    emit_hex(frame.error);
    emit_str(" addr=");
    emit_hex(frame.addr);
    emit_str(" pc=");
    emit_hex(frame.pc);
    emit_str(" sp=");
    emit_hex(frame.sp);
    emit_str(" pgtbl0=");
    emit_hex(frame.pgtbl0);
    emit_str(" pgtbl1=");
    emit_hex(frame.pgtbl1);
    emit_str("\n");
#if CONFIG_DEBUG
    g_last_frame = frame;
#endif
}

void first_chance_halt(const FirstChanceFrame &frame) noexcept {
    first_chance_emit(frame);
    arch::crash_halt();
#if CONFIG_DEBUG
    // Test seam only: production crash_halt never returns.
    ++g_halt_count;
#endif
}

FirstChanceFrame make_x86_frame(uint64_t vector, uint64_t error,
                                uint64_t rip, uint64_t sp, uint64_t cr2,
                                uint64_t cr3) noexcept {
    FirstChanceFrame f{FirstChanceArch::X86_64,
                       FirstChanceCause::CPU_EXCEPTION,
                       vector, error, cr2, rip, sp, cr3, 0};
    return f;
}

FirstChanceFrame make_aarch64_frame(uint64_t esr, uint64_t far, uint64_t elr,
                                    uint64_t sp, uint64_t ttbr0,
                                    uint64_t ttbr1) noexcept {
    FirstChanceFrame f{FirstChanceArch::AARCH64,
                       FirstChanceCause::CPU_EXCEPTION,
                       esr, 0, far, elr, sp, ttbr0, ttbr1};
    return f;
}

FirstChanceFrame make_riscv_frame(uint64_t scause, uint64_t sepc,
                                  uint64_t stval, uint64_t sp,
                                  uint64_t satp) noexcept {
    FirstChanceFrame f{FirstChanceArch::RISCV64,
                       FirstChanceCause::CPU_EXCEPTION,
                       scause, 0, stval, sepc, sp, satp, 0};
    return f;
}

FirstChanceFrame first_chance_frame_here(FirstChanceCause cause) noexcept {
    uint64_t sp = 0;
#if defined(CONFIG_ARCH_X86_64)
    asm volatile("mov %%rsp, %0" : "=r"(sp));
    FirstChanceFrame f{FirstChanceArch::X86_64, cause, 0, 0, 0, 0, sp, 0, 0};
#elif defined(CONFIG_ARCH_AARCH64)
    asm volatile("mov %0, sp" : "=r"(sp));
    FirstChanceFrame f{FirstChanceArch::AARCH64, cause, 0, 0, 0, 0, sp, 0, 0};
#else
    asm volatile("mv %0, sp" : "=r"(sp));
    FirstChanceFrame f{FirstChanceArch::RISCV64, cause, 0, 0, 0, 0, sp, 0, 0};
#endif
    return f;
}

#if CONFIG_DEBUG
void test_arm_crash_halt() noexcept { g_halt_armed = true; }
uint64_t test_crash_halt_count() noexcept { return g_halt_count; }
FirstChanceFrame test_last_frame() noexcept { return g_last_frame; }
void test_reset_crash_halt() noexcept {
    g_halt_armed = false;
    g_halt_count = 0;
}
#endif

} // namespace debug
} // namespace kernel

namespace arch {

// Weak default: IRQs off + halt loop, never returns. The CONFIG_DEBUG test
// override returns only when explicitly armed.
__attribute__((weak)) void crash_halt() noexcept {
#if CONFIG_DEBUG
    if (kernel::debug::g_halt_armed)
        return;
#endif
    arch::cli();
    for (;;)
        arch::hlt();
}

} // namespace arch
