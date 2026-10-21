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

/// @file fpu.cpp
/// @brief AArch64 FP/SIMD (NEON) context support (issue #353).

#include <kernel/arch/aarch64/hal/fpu.hpp>
#include <kernel/arch/aarch64/hal/cpuid_impl.hpp>
#include <kernel/arch/aarch64/hal/io_impl.hpp>
#include <kernel/log/dmesg.hpp>

namespace arch {
namespace {

// BOOT_ONLY latch (issue #198 pattern): namespace scope, latched on
// success only.
bool g_fpu_done = false;

void log_fpu(const char *msg, bool is_error) {
    kernel::log::dmesg_push_sev(
        kernel::log::ErrorSubsystem::BASE, kernel::log::kDmesgBase_BASE,
        is_error ? kernel::log::LogSeverity::ERROR
                 : kernel::log::LogSeverity::INFO,
        msg, 0);
}

} // namespace

bool fpu_supported() {
    return has_fpu();
}

bool fpu_available() {
    return g_fpu_done;
}

void fpu_init() {
    if (g_fpu_done)
        return;
    if (!has_fpu()) {
        log_fpu("fpu: FP/SIMD absent, degraded", true);
        return;
    }
    fp_enable();
    g_fpu_done = true;
    log_fpu("fpu: NEON enabled (eager switch)", false);
}

void fpu_save(void *area) {
    auto *p = static_cast<uint8_t *>(area);
    // Reads Q-regs only (no FP state modified); x1 scratch is declared
    // so the compiler cannot hold a live value across it.  Q-reg reads
    // need no clobbers.
    asm volatile(
        "stp q0, q1, [%0, #0]\n\t"
        "stp q2, q3, [%0, #32]\n\t"
        "stp q4, q5, [%0, #64]\n\t"
        "stp q6, q7, [%0, #96]\n\t"
        "stp q8, q9, [%0, #128]\n\t"
        "stp q10, q11, [%0, #160]\n\t"
        "stp q12, q13, [%0, #192]\n\t"
        "stp q14, q15, [%0, #224]\n\t"
        "stp q16, q17, [%0, #256]\n\t"
        "stp q18, q19, [%0, #288]\n\t"
        "stp q20, q21, [%0, #320]\n\t"
        "stp q22, q23, [%0, #352]\n\t"
        "stp q24, q25, [%0, #384]\n\t"
        "stp q26, q27, [%0, #416]\n\t"
        "stp q28, q29, [%0, #448]\n\t"
        "stp q30, q31, [%0, #480]\n\t"
        "mrs x1, fpcr\n\t"
        "str w1, [%0, #512]\n\t"
        "mrs x1, fpsr\n\t"
        "str w1, [%0, #516]\n\t"
        :
        : "r"(p)
        : "x1", "memory");
}

void fpu_restore(const void *area) {
    auto *p = static_cast<const uint8_t *>(area);
    // Writes every Q-reg (contract: replaces all FP state — callers must
    // save first).  All Q-regs are declared clobbered so the compiler
    // holds no live FP values across the call; x1 scratch likewise.
    asm volatile(
        "ldp q0, q1, [%0, #0]\n\t"
        "ldp q2, q3, [%0, #32]\n\t"
        "ldp q4, q5, [%0, #64]\n\t"
        "ldp q6, q7, [%0, #96]\n\t"
        "ldp q8, q9, [%0, #128]\n\t"
        "ldp q10, q11, [%0, #160]\n\t"
        "ldp q12, q13, [%0, #192]\n\t"
        "ldp q14, q15, [%0, #224]\n\t"
        "ldp q16, q17, [%0, #256]\n\t"
        "ldp q18, q19, [%0, #288]\n\t"
        "ldp q20, q21, [%0, #320]\n\t"
        "ldp q22, q23, [%0, #352]\n\t"
        "ldp q24, q25, [%0, #384]\n\t"
        "ldp q26, q27, [%0, #416]\n\t"
        "ldp q28, q29, [%0, #448]\n\t"
        "ldp q30, q31, [%0, #480]\n\t"
        "ldr w1, [%0, #512]\n\t"
        "msr fpcr, x1\n\t"
        "ldr w1, [%0, #516]\n\t"
        "msr fpsr, x1\n\t"
        :
        : "r"(p)
        : "x1", "memory", "q0", "q1", "q2", "q3", "q4", "q5", "q6", "q7",
          "q8", "q9", "q10", "q11", "q12", "q13", "q14", "q15", "q16",
          "q17", "q18", "q19", "q20", "q21", "q22", "q23", "q24", "q25",
          "q26", "q27", "q28", "q29", "q30", "q31");
}

} // namespace arch
