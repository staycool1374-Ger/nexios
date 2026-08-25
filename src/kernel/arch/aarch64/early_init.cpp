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

/// @file early_init.cpp
/// @brief Early AArch64 init not covered by boot.S assembly: PAN (FEAT_PAN)
///        detection + enablement for MP-4.4 (issue #5).

// The bulk of early AArch64 init (UART, page tables, MMU, higher-half
// transition) is performed in boot.S.  This TU hosts arch::g_pan_supported
// and arch::pan_init() — the only arch-level init that needs C++ (ID register
// field parsing) rather than assembly.

#include <kernel/nexios_config.h>
#include <kernel/arch/hal/io.hpp>

namespace arch {

bool g_pan_supported = false;

/// @brief Detect + enable aarch64 PAN (FEAT_PAN), MP-4.4.
/// Sets SCTLR_EL1.PAN (bit 23) when ID_AA64MMFR1_EL1.PAN[23:20] != 0 and
/// CONFIG_PAN is on, then defaults PSTATE.PAN to deny (clac).  Every PAN
/// sysreg/SCTLR access is gated on this detection — an access on a CPU
/// without FEAT_PAN raises an UNDEFINED exception at EL1.
/// @return true if PAN was enabled.
bool pan_init() {
#if CONFIG_PAN
    uint64_t mmfr1{};
    asm volatile("mrs %0, id_aa64mmfr1_el1" : "=r"(mmfr1));
    if (((mmfr1 >> 20) & 0xF) == 0)
        return false;
    uint64_t sctlr{};
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 23); // SCTLR_EL1.PAN
    asm volatile("msr sctlr_el1, %0" : : "r"(sctlr) : "memory");
    asm volatile("isb");
    g_pan_supported = true;
    clac(); // default-deny: PSTATE.PAN = 1
    return true;
#else
    return false;
#endif
}

} // namespace arch

struct EarlyInitStub {
    EarlyInitStub() {
    }
};
static EarlyInitStub stub{};
