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

/// @file test_random_seed.cpp
/// @brief Random seed initialisation tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/random.hpp>
#include <kernel/arch/rand.hpp>
#include <kernel/arch/cpuid.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verify the CSPRNG works regardless of hardware RNG availability
// Input: Check CPUID RDRAND/RDSEED, then generate random data
// Expect: random_fill produces non-zero output even without hardware RNG
// Depends: kernel::arch::has_rdrand, kernel::arch::has_rdseed,
// kernel::random_fill
JARVIS_TEST(random_fallback_independent, "PRE: none | POST: none") {
    bool has_rr = arch::has_rdrand();
    bool has_rs = arch::has_rdseed();
    Logger::info("RNG sources: RDSEED=%s RDRAND=%s", has_rs ? "yes" : "no",
                 has_rr ? "yes" : "no");

    uint8_t buf[256];
    random_fill(buf, sizeof(buf));

    bool any_nonzero = false;
    for (size_t i = 0; i < sizeof(buf); ++i) {
        if (buf[i] != 0) {
            any_nonzero = true;
            break;
        }
    }
    JARVIS_ASSERT_FMT(
        any_nonzero,
        "random_fill returned all zeros (no HW RNG fallback failed)");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verify the CSPRNG correctly crosses ChaCha20 block boundaries
// Input: generate 256 bytes (4 ChaCha20 blocks), verify all non-zero
// Expect: All blocks produce non-zero output
// Depends: kernel::random_fill
JARVIS_TEST(random_multi_block, "PRE: none | POST: none") {
    uint8_t buf[256];
    random_fill(buf, sizeof(buf));

    // Verify each 64-byte ChaCha20 block has non-zero content
    for (int block = 0; block < 4; ++block) {
        bool block_nonzero = false;
        for (int i = 0; i < 64; ++i) {
            if (buf[block * 64 + i] != 0) {
                block_nonzero = true;
                break;
            }
        }
        JARVIS_ASSERT_FMT(block_nonzero, "ChaCha20 block %d returned all zeros",
                          block);
    }
    JARVIS_TEST_PASS();
}

void register_random_seed_tests() {
    Logger::info("Registering RNG seed/reseed tests");
    JARVIS_REGISTER_TEST(random_fallback_independent);
    JARVIS_REGISTER_TEST(random_multi_block);
}
