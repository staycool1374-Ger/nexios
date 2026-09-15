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

/// @file cpuid_impl.hpp
/// @brief x86_64 CPUID instruction wrappers — feature detection flags and
/// helpers.

#pragma once

#include <types.hpp>

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
namespace arch {

/// @brief Result of a CPUID instruction — holds the four output registers.
struct CpuIdResult {
    uint32_t eax; ///< EAX output.
    uint32_t ebx; ///< EBX output.
    uint32_t ecx; ///< ECX output.
    uint32_t edx; ///< EDX output.
};

/// @brief Execute the CPUID instruction.
/// @param leaf CPUID leaf (EAX input).
/// @param subleaf CPUID sub-leaf (ECX input, default 0).
/// @return CpuIdResult containing the four output registers.
inline CpuIdResult cpuid(uint32_t leaf, uint32_t subleaf = 0) {
    CpuIdResult r{};
    asm volatile("cpuid"
                 : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                 : "a"(leaf), "c"(subleaf));
    return r;
}

/// @name Feature flags (EDX, leaf 1)
/// @brief CPU feature bits returned in EDX from CPUID leaf 1.
/// @{
inline constexpr uint32_t CPUID_EDX1_FPU = 1u << 0;
inline constexpr uint32_t CPUID_EDX1_FXSR = 1u << 24;
inline constexpr uint32_t CPUID_EDX1_SSE = 1u << 25;
inline constexpr uint32_t CPUID_EDX1_SSE2 = 1u << 26;
/// @}
/// @name Feature flags (ECX, leaf 1)
/// @brief CPU feature bits returned in ECX from CPUID leaf 1.
/// @{
inline constexpr uint32_t CPUID_ECX1_SSE3 = 1u << 0;
inline constexpr uint32_t CPUID_ECX1_SSSE3 = 1u << 9;
inline constexpr uint32_t CPUID_ECX1_SSE4_1 = 1u << 19;
inline constexpr uint32_t CPUID_ECX1_SSE4_2 = 1u << 20;
inline constexpr uint32_t CPUID_ECX1_RDRAND = 1u << 30;
/// @brief PCID feature bit (ECX, leaf 1): process-context identifiers.
inline constexpr uint32_t CPUID_ECX1_PCID = 1u << 17;
/// @}
/// @brief RDRAND feature bit (EBX, leaf 7, subleaf 0).
inline constexpr uint32_t CPUID_EBX7_RDSEED = 1u << 18;
/// @brief INVPCID feature bit (EBX, leaf 7, subleaf 0).
inline constexpr uint32_t CPUID_EBX7_INVPCID = 1u << 10;

/// @brief Check if the CPU has an x87 FPU.
/// @return true if the FPU feature bit is set.
inline bool has_fpu() {
    return (cpuid(1).edx & CPUID_EDX1_FPU) != 0;
}
/// @brief Check if the CPU supports FXSAVE/FXRSTOR.
/// @return true if the FXSR feature bit is set.
inline bool has_fxsr() {
    return (cpuid(1).edx & CPUID_EDX1_FXSR) != 0;
}
/// @brief Check if the CPU supports SSE.
/// @return true if the SSE feature bit is set.
inline bool has_sse() {
    return (cpuid(1).edx & CPUID_EDX1_SSE) != 0;
}
/// @brief Check if the CPU supports the RDRAND instruction.
/// @return true if the RDRAND feature bit is set.
inline bool has_rdrand() {
    return (cpuid(1).ecx & CPUID_ECX1_RDRAND) != 0;
}
/// @brief Check if the CPU supports process-context identifiers.
/// @return true if the PCID feature bit is set.
inline bool has_pcid() {
    return (cpuid(1).ecx & CPUID_ECX1_PCID) != 0;
}
/// @brief Check if the CPU supports the INVPCID instruction.
/// @return true if the INVPCID feature bit is set.
inline bool has_invpcid() {
    return (cpuid(7, 0).ebx & CPUID_EBX7_INVPCID) != 0;
}
/// @brief Check if the CPU supports the RDSEED instruction.
/// @return true if the RDSEED feature bit (leaf 7, subleaf 0) is set.
inline bool has_rdseed() {
    return (cpuid(7, 0).ebx & CPUID_EBX7_RDSEED) != 0;
}

} // namespace arch
// NOLINTEND(bugprone-easily-swappable-parameters)
