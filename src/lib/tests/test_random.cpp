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

/// @file test_random.cpp
/// @brief Random number generator tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/random.hpp>
#include <kernel/arch/rand.hpp>
#include <kernel/arch/cpuid.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Basic RNG smoke test — generate random bytes and check they aren't
// all zero Expect: random_fill produces non-zero output within a reasonable
// sample
JARVIS_TEST(random_basic_smoke, "PRE: none | POST: none") {
    uint8_t buf[256];
    random_fill(buf, sizeof(buf));

    bool all_zero = true;
    bool all_ff = true;
    for (size_t i = 0; i < sizeof(buf); ++i) {
        if (buf[i] != 0)
            all_zero = false;
        if (buf[i] != 0xFF)
            all_ff = false;
    }

    JARVIS_ASSERT_FMT(!all_zero, "random_fill returned %zu zero bytes",
                      sizeof(buf));
    JARVIS_ASSERT_FMT(!all_ff, "random_fill returned %zu 0xFF bytes",
                      sizeof(buf));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verify that consecutive random_fill calls produce different results
// Expect: Two consecutive 64-byte buffers should differ
JARVIS_TEST(random_not_repeating, "PRE: none | POST: none") {
    uint8_t a[64];
    uint8_t b[64];
    random_fill(a, sizeof(a));
    random_fill(b, sizeof(b));

    bool same = true;
    for (size_t i = 0; i < sizeof(a); ++i) {
        if (a[i] != b[i]) {
            same = false;
            break;
        }
    }

    JARVIS_ASSERT_FMT(
        !same, "Consecutive random_fill calls produced identical output");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: random_u64 should produce varied output
// Expect: Three consecutive random_u64 calls should not be all equal
JARVIS_TEST(random_u64_not_constant, "PRE: none | POST: none") {
    uint64_t a = random_u64();
    uint64_t b = random_u64();
    uint64_t c = random_u64();

    JARVIS_ASSERT_FMT(a != b || b != c,
                      "random_u64 produced constant value 0x%016llx", a);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Partial fills (odd lengths) should not crash or produce all-zero
// Expect: 1-byte, 3-byte, 7-byte, 33-byte fills return non-zero
JARVIS_TEST(random_partial_fills, "PRE: none | POST: none") {
    uint8_t buf[33];
    size_t lens[] = {1, 3, 7, 33};

    for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); ++li) {
        size_t len = lens[li];
        random_fill(buf, len);
        bool all_zero = true;
        for (size_t i = 0; i < len; ++i) {
            if (buf[i] != 0) {
                all_zero = false;
                break;
            }
        }
        JARVIS_ASSERT_FMT(!all_zero, "random_fill(%zu) returned all zeros",
                          len);
    }

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Large buffer fills should work without overflow or crash
// Expect: 8192-byte fill completes without error
JARVIS_TEST(random_large_buffer, "PRE: none | POST: none") {
    uint8_t buf[8192];
    random_fill(buf, sizeof(buf));

    bool all_zero = true;
    for (size_t i = 0; i < sizeof(buf); ++i) {
        if (buf[i] != 0) {
            all_zero = false;
            break;
        }
    }

    JARVIS_ASSERT_FMT(!all_zero, "random_fill(%zu) returned all zeros",
                      sizeof(buf));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Zero-length request should not crash or modify the buffer
// Expect: Buffer remains unchanged after zero-length fill
JARVIS_TEST(random_zero_length, "PRE: none | POST: none") {
    uint8_t buf[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    random_fill(buf, 0);
    JARVIS_ASSERT(buf[0] == 0xAA && buf[1] == 0xBB && buf[2] == 0xCC &&
                  buf[3] == 0xDD);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: CPUID-based RDRAND/RDSEED detection should not crash
// Expect: Returns true or false without faulting
JARVIS_TEST(random_cpuid_detection, "PRE: none | POST: none") {
    // Just ensure the functions don't crash
    bool has_rr = arch::has_rdrand();
    bool has_rs = arch::has_rdseed();
    Logger::info("RDRAND: %s, RDSEED: %s", has_rr ? "yes" : "no",
                 has_rs ? "yes" : "no");
    JARVIS_TEST_PASS();
}

void register_random_tests() {
    Logger::info("Registering random number generator tests");
    JARVIS_REGISTER_TEST(random_basic_smoke);
    JARVIS_REGISTER_TEST(random_not_repeating);
    JARVIS_REGISTER_TEST(random_u64_not_constant);
    JARVIS_REGISTER_TEST(random_partial_fills);
    JARVIS_REGISTER_TEST(random_large_buffer);
    JARVIS_REGISTER_TEST(random_zero_length);
    JARVIS_REGISTER_TEST(random_cpuid_detection);
}
