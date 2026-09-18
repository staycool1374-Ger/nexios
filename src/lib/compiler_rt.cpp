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

/// @file compiler_rt.cpp
/// @brief Software implementations of the gcc runtime helpers __clzdi2 /
///        __ctzdi2.  RISC-V ONLY (issue #182 / #184): rv64imafdc without the
///        Zbb extension lowers __builtin_clzll/__builtin_ctzll to libcalls,
///        and the prebuilt libgcc.a cannot satisfy them (R_RISCV_HI20
///        medany/medlow relocation mismatch), so the kernel provides its
///        own copies.  x86_64 and AArch64 lower these builtins to inline
///        instructions (verified: zero undefined refs in their objects),
///        so the functions are compiled out there — and out of the x86
///        coverage denominator.

#if defined(CONFIG_ARCH_RISCV64)

extern "C" {

unsigned long __clzdi2(unsigned long val) {
    if (val == 0) return 64;
    unsigned long count = 0;
    if ((val >> 32) == 0) { count += 32; val <<= 32; }
    if ((val >> 48) == 0) { count += 16; val <<= 16; }
    if ((val >> 56) == 0) { count += 8; val <<= 8; }
    if ((val >> 60) == 0) { count += 4; val <<= 4; }
    if ((val >> 62) == 0) { count += 2; val <<= 2; }
    if ((val >> 63) == 0) { count += 1; }
    return count;
}

unsigned long __ctzdi2(unsigned long val) {
    if (val == 0) return 64;
    unsigned long count = 0;
    if ((val & 0xFFFFFFFFUL) == 0) { count += 32; val >>= 32; }
    if ((val & 0xFFFFUL) == 0) { count += 16; val >>= 16; }
    if ((val & 0xFFUL) == 0) { count += 8; val >>= 8; }
    if ((val & 0xFUL) == 0) { count += 4; val >>= 4; }
    if ((val & 0x3UL) == 0) { count += 2; val >>= 2; }
    if ((val & 0x1UL) == 0) { count += 1; }
    return count;
}

}

#endif // defined(CONFIG_ARCH_RISCV64)
