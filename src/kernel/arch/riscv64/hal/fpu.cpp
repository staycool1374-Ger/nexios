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
/// @brief RISC-V 64 FP (F/D extensions) context support (issue #353).

#include <kernel/arch/riscv64/hal/fpu.hpp>
#include <kernel/arch/riscv64/hal/io_impl.hpp>
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
    // S-mode probe (MISA is M-mode-only): FS set to 0b11 must read back.
    // sstatus.FS occupies bits [14:13].
    uint64_t sstatus{};
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    sstatus |= (3ULL << 13);
    asm volatile("csrw sstatus, %0" : : "r"(sstatus) : "memory");
    uint64_t back{};
    asm volatile("csrr %0, sstatus" : "=r"(back));
    return ((back >> 13) & 3ULL) == 3ULL;
}

bool fpu_available() {
    return g_fpu_done;
}

void fpu_init() {
    if (g_fpu_done)
        return;
    if (!fpu_supported()) {
        log_fpu("fpu: F/D absent, degraded", true);
        return;
    }
    fp_enable();
    g_fpu_done = true;
    log_fpu("fpu: F/D enabled (eager switch)", false);
}

void fpu_save(void *area) {
    auto *p = static_cast<uint64_t *>(area);
    // Reads f-regs only (no FP state modified); t0 scratch is declared
    // so the compiler cannot hold a live value across it.
    asm volatile(
        "fsd f0, 0(%0)\n\t"
        "fsd f1, 8(%0)\n\t"
        "fsd f2, 16(%0)\n\t"
        "fsd f3, 24(%0)\n\t"
        "fsd f4, 32(%0)\n\t"
        "fsd f5, 40(%0)\n\t"
        "fsd f6, 48(%0)\n\t"
        "fsd f7, 56(%0)\n\t"
        "fsd f8, 64(%0)\n\t"
        "fsd f9, 72(%0)\n\t"
        "fsd f10, 80(%0)\n\t"
        "fsd f11, 88(%0)\n\t"
        "fsd f12, 96(%0)\n\t"
        "fsd f13, 104(%0)\n\t"
        "fsd f14, 112(%0)\n\t"
        "fsd f15, 120(%0)\n\t"
        "fsd f16, 128(%0)\n\t"
        "fsd f17, 136(%0)\n\t"
        "fsd f18, 144(%0)\n\t"
        "fsd f19, 152(%0)\n\t"
        "fsd f20, 160(%0)\n\t"
        "fsd f21, 168(%0)\n\t"
        "fsd f22, 176(%0)\n\t"
        "fsd f23, 184(%0)\n\t"
        "fsd f24, 192(%0)\n\t"
        "fsd f25, 200(%0)\n\t"
        "fsd f26, 208(%0)\n\t"
        "fsd f27, 216(%0)\n\t"
        "fsd f28, 224(%0)\n\t"
        "fsd f29, 232(%0)\n\t"
        "fsd f30, 240(%0)\n\t"
        "fsd f31, 248(%0)\n\t"
        "csrr t0, fcsr\n\t"
        "sw t0, 256(%0)\n\t"
        :
        : "r"(p)
        : "t0", "memory");
}

void fpu_restore(const void *area) {
    auto *p = static_cast<const uint64_t *>(area);
    // Writes every f-reg (contract: replaces all FP state — callers must
    // save first).  All f-regs are declared clobbered so the compiler
    // holds no live FP values across the call; t0 scratch likewise.
    asm volatile(
        "fld f0, 0(%0)\n\t"
        "fld f1, 8(%0)\n\t"
        "fld f2, 16(%0)\n\t"
        "fld f3, 24(%0)\n\t"
        "fld f4, 32(%0)\n\t"
        "fld f5, 40(%0)\n\t"
        "fld f6, 48(%0)\n\t"
        "fld f7, 56(%0)\n\t"
        "fld f8, 64(%0)\n\t"
        "fld f9, 72(%0)\n\t"
        "fld f10, 80(%0)\n\t"
        "fld f11, 88(%0)\n\t"
        "fld f12, 96(%0)\n\t"
        "fld f13, 104(%0)\n\t"
        "fld f14, 112(%0)\n\t"
        "fld f15, 120(%0)\n\t"
        "fld f16, 128(%0)\n\t"
        "fld f17, 136(%0)\n\t"
        "fld f18, 144(%0)\n\t"
        "fld f19, 152(%0)\n\t"
        "fld f20, 160(%0)\n\t"
        "fld f21, 168(%0)\n\t"
        "fld f22, 176(%0)\n\t"
        "fld f23, 184(%0)\n\t"
        "fld f24, 192(%0)\n\t"
        "fld f25, 200(%0)\n\t"
        "fld f26, 208(%0)\n\t"
        "fld f27, 216(%0)\n\t"
        "fld f28, 224(%0)\n\t"
        "fld f29, 232(%0)\n\t"
        "fld f30, 240(%0)\n\t"
        "fld f31, 248(%0)\n\t"
        "lwu t0, 256(%0)\n\t"
        "csrw fcsr, t0\n\t"
        :
        : "r"(p)
        : "t0", "memory", "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7",
          "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15", "f16",
          "f17", "f18", "f19", "f20", "f21", "f22", "f23", "f24", "f25",
          "f26", "f27", "f28", "f29", "f30", "f31");
}

} // namespace arch
