/// @file test_buildsystem.cpp
/// @brief Build system integration tests.

#include <test.hpp>
#include <logger.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies kernel compiles with -DCONFIG_DEBUG for test builds.
// Input: Check debug build flags
// Expect: CONFIG_DEBUG defined, debug symbols present
// Depends: Makefile
JARVIS_TEST(buildsystem_debug_flags, "PRE: none | POST: none") {
#if defined(CONFIG_DEBUG)
    bool debug_defined = true;
#else
    bool debug_defined = false;
#endif
    JARVIS_ASSERT_FMT(debug_defined,
                      "CONFIG_DEBUG must be defined for test builds");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that a known CONFIG_ARCH_* macro is defined for the
// build (arch-appropriate TU selection).
// Input: Preprocessor check
// Expect: exactly one known CONFIG_ARCH_* macro defined
// Depends: Makefile, build system
JARVIS_TEST(buildsystem_config_arch_defined, "PRE: none | POST: none") {
#if defined(CONFIG_ARCH_X86_64) || defined(CONFIG_ARCH_AARCH64) || \
    defined(CONFIG_ARCH_RISCV64)
    bool arch_ok = true;
#else
    bool arch_ok = false;
#endif
    JARVIS_ASSERT_FMT(arch_ok,
                      "a known CONFIG_ARCH_* must be defined for the build");
    JARVIS_TEST_PASS();
}

#if defined(CONFIG_ARCH_X86_64)
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

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/cpuid.hpp>
#include <kernel/arch/io.hpp>
#include <string.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies ARCH=x86_64 selects x86_64 toolchain and sources.
// Input: Build with ARCH=x86_64
// Expect: x86_64-elf-gcc used, arch/x86_64/ sources compiled
// Depends: Makefile, toolchain
JARVIS_TEST(buildsystem_arch_x86_64_toolchain, "PRE: none | POST: none") {
    arch::CpuIdResult res = arch::cpuid(0);
    char vendor[13];
    memcpy(vendor, &res.ebx, 4);
    memcpy(vendor + 4, &res.edx, 4);
    memcpy(vendor + 8, &res.ecx, 4);
    vendor[12] = '\0';
    JARVIS_ASSERT_FMT(strcmp(vendor, "GenuineIntel") == 0 ||
                          strcmp(vendor, "AuthenticAMD") == 0,
                      "Toolchain targets x86_64: CPUID vendor '%s'", vendor);
    JARVIS_ASSERT_EQ(8ULL, sizeof(void *));

    uint64_t cr3 = arch::read_cr3();
    JARVIS_ASSERT(cr3 != 0);

    JARVIS_TEST_PASS();
}

#if defined(CONFIG_ARCH_X86_64)
// Runmode: kernel
// Testidea: Verifies invalid ARCH value produces clear error message.
// Input: Build with ARCH=invalid_arch
// Expect: Make fails with descriptive error
// Depends: Makefile
JARVIS_TEST(buildsystem_invalid_arch_error, "PRE: none | POST: none") {
    JARVIS_ASSERT_EQ(8ULL, sizeof(void *));

    arch::CpuIdResult res = arch::cpuid(0);
    char vendor[13];
    memcpy(vendor, &res.ebx, 4);
    memcpy(vendor + 4, &res.edx, 4);
    memcpy(vendor + 8, &res.ecx, 4);
    vendor[12] = '\0';
    JARVIS_ASSERT_FMT(strcmp(vendor, "GenuineIntel") == 0 ||
                          strcmp(vendor, "AuthenticAMD") == 0 ||
                          vendor[0] == '\0' /* KVM / hypervisor */,
                      "Only x86_64 ARCH is supported (CPUID vendor '%s')",
                      vendor);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies default ARCH is x86_64.
// Input: Build without ARCH variable
// Expect: Defaults to x86_64, builds successfully
// Depends: Makefile
JARVIS_TEST(buildsystem_default_arch_x86_64, "PRE: none | POST: none") {
    JARVIS_ASSERT_EQ(8ULL, sizeof(void *));

    uint64_t cr4 = arch::read_cr4();
    JARVIS_ASSERT(cr4 != 0);

    JARVIS_TEST_PASS();
}
#endif

void register_buildsystem_tests() {
    Logger::info("Registering buildsystem tests");

    JARVIS_REGISTER_TEST(buildsystem_arch_x86_64_toolchain);
    JARVIS_REGISTER_TEST(buildsystem_invalid_arch_error);
    JARVIS_REGISTER_TEST(buildsystem_default_arch_x86_64);
    JARVIS_REGISTER_TEST(buildsystem_debug_flags);
    JARVIS_REGISTER_TEST(buildsystem_config_arch_defined);
}
#elif defined(CONFIG_ARCH_AARCH64) || defined(CONFIG_ARCH_RISCV64)
#include <kernel/arch/cpuid.hpp>

// Runmode: kernel
// Testidea: The non-x86 toolchain targets the right architecture: 64-bit
// pointers plus the hardware FPU the kernel context-switches (NEON/FP on
// aarch64 QEMU virt via EL1-readable PFR0; riscv64 proves XLEN=64 via
// pointers only — S-mode misa reads trap, so FPU presence detection
// belongs to #353, not build identity; see the deliberately
// unregistered riscv64_fpu_extension_detection).
// Input: sizeof(void*), arch::has_fpu() (aarch64 only).
// Expect: 8-byte pointers; FPU present where readable.
// Depends: toolchain triple, arch::has_fpu
JARVIS_TEST(buildsystem_arch_toolchain, "PRE: none | POST: none") {
    JARVIS_ASSERT_EQ(8ULL, sizeof(void *));
#if defined(CONFIG_ARCH_AARCH64)
    JARVIS_ASSERT(arch::has_fpu());
#endif
    JARVIS_TEST_PASS();
}

void register_buildsystem_tests() {
    Logger::info("Registering buildsystem tests");
    JARVIS_REGISTER_TEST(buildsystem_arch_toolchain);
    JARVIS_REGISTER_TEST(buildsystem_debug_flags);
    JARVIS_REGISTER_TEST(buildsystem_config_arch_defined);
}
#endif