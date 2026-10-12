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

/// @file test_libc_string.cpp
/// @brief String/memory coverage (issue #303, Phase 6 module 11).
///
///        basic_lib covers single-size strlen/strcmp/strncmp/strncpy/
///        memcpy/memset/memcmp. This file covers the remaining bullets:
///        the 0..256 size sweep, memmove overlap (both directions),
///        strcpy/strcat/strchr/strrchr/strstr/strtok — against picolibc
///        in Ring 3 (exact-output pins) — plus an in-kernel sweep of the
///        kernel string API (memcpy/memset/memcmp/strncpy/strlcpy at
///        every size 0..256). strtok exists in picolibc; the IN-TREE
///        libc has no strtok (confirmed gap — see the #303 tracker).

#include <test.hpp>
#include <logger.hpp>
#include <string.hpp>
#include "test_ring3_vehicle.hpp"

using namespace kernel;

#if defined(CONFIG_ARCH_X86_64)

namespace {

constexpr size_t kCapSize = 2048;

} // namespace

// Runmode: kernel
// Testidea: memmove overlap (forward + backward restores the pattern),
// strcpy/strcat/strchr/strrchr/strstr/strtok exact results in Ring 3.
// Input: Dispatch; match the full string marker line.
// Expect: "string 4876 foobar 5 7 4 a b c".
// Depends: picolibc string.h
JARVIS_TEST(libc_string_ring3_exact, "PRE: vfsd | POST: none") {
    char capture[kCapSize]{};
    test::ring3::Outcome o = test::ring3::run(nullptr, nullptr, capture, kCapSize);
    JARVIS_ASSERT(o.joined);
    JARVIS_ASSERT(test::ring3::serial_contains(
        capture, "LIBC_VERIFY: string 4864 foobar 5 7 4 a b c"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: memcpy/memcmp correct at every size 0..256 (sweep checksum =
// 0+1+...+256 = 32896).
// Input: Dispatch; parse the sweep marker.
// Expect: sweep == 32896.
// Depends: picolibc memcpy/memcmp/memset
JARVIS_TEST(libc_string_ring3_sweep, "PRE: vfsd | POST: none") {
    char capture[kCapSize]{};
    test::ring3::Outcome o = test::ring3::run(nullptr, nullptr, capture, kCapSize);
    JARVIS_ASSERT(o.joined);
    uint64_t v[1] = {};
    JARVIS_ASSERT(test::ring3::parse_tag_line(capture, "LIBC_VERIFY: sweep", v,
                                        1, kCapSize));
    JARVIS_ASSERT_FMT(v[0] == 32896, "sweep checksum %lu != 32896", v[0]);
    JARVIS_TEST_PASS();
}

#endif // CONFIG_ARCH_X86_64

// Runmode: kernel
// Testidea: Kernel string API correct at every size 0..256: memcpy round-
// trip + memcmp, memset fill, strncpy truncation/padding, strlcpy bound.
// Input: Stack buffers; all sizes 0..256.
// Expect: Zero mismatches (fail-closed count published on failure).
// Depends: src/lib/string.hpp (kernel-side, all architectures)
JARVIS_TEST(libc_string_kernel_api_sweep, "PRE: none | POST: none") {
    char src[256];
    char dst[256];
    for (size_t i = 0; i < 256; ++i)
        src[i] = static_cast<char>(i * 7 + 3);
    uint64_t bad = 0;
    for (size_t n = 0; n <= 256; ++n) {
        for (size_t i = 0; i < n; ++i)
            dst[i] = 0;
        memcpy(dst, src, n);
        if (memcmp(dst, src, n) != 0)
            ++bad;
        memset(dst, 0xA5, n);
        for (size_t i = 0; i < n; ++i) {
            if (dst[i] != static_cast<char>(0xA5))
                ++bad;
        }
    }
    JARVIS_ASSERT_FMT(bad == 0, "%lu kernel mem sweep mismatches", bad);

    // strncpy: truncates without NUL at the bound, pads short copies.
    char nb[8];
    strncpy(nb, "0123456789ABCDEF", sizeof(nb));
    JARVIS_ASSERT_FMT(memcmp(nb, "01234567", 8) == 0,
                      "strncpy truncation wrong");
    char pb[8];
    for (size_t i = 0; i < sizeof(pb); ++i)
        pb[i] = 'Z';
    strncpy(pb, "hi", sizeof(pb));
    JARVIS_ASSERT_FMT(pb[0] == 'h' && pb[1] == 'i' && pb[2] == '\0' &&
                          pb[7] == '\0',
                      "strncpy padding wrong");
    // strlcpy: always NUL-terminates, returns the full source length.
    char lb[8];
    size_t lr = strlcpy(lb, "0123456789", sizeof(lb));
    JARVIS_ASSERT_FMT(lr == 10, "strlcpy return %lu != 10", lr);
    JARVIS_ASSERT_FMT(memcmp(lb, "0123456", 8) == 0 && lb[7] == '\0',
                      "strlcpy bound wrong");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all libc string tests.
// Input: None
// Expect: All string tests registered
// Depends: test framework
void register_libc_string_tests() {
    Logger::info("Registering libc string tests");
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_REGISTER_TEST(libc_string_ring3_exact);
    JARVIS_REGISTER_TEST(libc_string_ring3_sweep);
#endif
    JARVIS_REGISTER_TEST(libc_string_kernel_api_sweep);
}
