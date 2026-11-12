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

/// @file test_smp_census.cpp
/// @brief SMP AP census (issue #525): guards against the silent
/// single-CPU false green — a `-smp 2` run that never woke the AP must
/// fail here, not pass an empty multicpu class. Runs only inside the
/// `smp_multicpu` aggregate (the harness forces `-smp 2` for it);
/// never registered in single-CPU classes.

#include <test.hpp>
#include <logger.hpp>

#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/smp.hpp>
#elif defined(CONFIG_ARCH_AARCH64)
#include <kernel/arch/aarch64/hal/smp.hpp>
#elif defined(CONFIG_ARCH_RISCV64)
#include <kernel/arch/riscv64/hal/smp.hpp>
#endif

using namespace kernel;

// Runmode: kernel
// Testidea: Issue #525 false-green guard — under the harness-forced
// `-smp 2` topology exactly one AP must be up (BSP + 1 AP). A parked
// or refused bring-up (SBI HSM refusal, PSCI failure, DTB single-CPU
// walk) shows up as ap_count() == 0 and fails here instead of letting
// the multicpu class pass vacuously.
// Input: kernel::smp::ap_count().
// Expect: exactly 1 AP on every arch.
// Depends: arch bring_up (HSM hart_start / PSCI_CPU_ON / SIPI), g_aps_up
JARVIS_TEST(smp_census_two_cpus_up, "PRE: -smp 2 | POST: none") {
    // BSP + 1 AP: fewer means bring-up failed (or the harness lost the
    // -smp flag); more means the topology changed and the multicpu
    // expectations need a revisit.
    JARVIS_ASSERT_EQ(1ULL, static_cast<uint64_t>(smp::ap_count()));
    JARVIS_TEST_PASS();
}

void register_smp_census_tests() {
    Logger::info("Registering SMP census tests");
    JARVIS_REGISTER_TEST(smp_census_two_cpus_up);
}
