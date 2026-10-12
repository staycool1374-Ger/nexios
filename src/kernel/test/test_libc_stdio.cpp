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

/// @file test_libc_stdio.cpp
/// @brief printf/scanf coverage (issue #303, Phase 6 module 9).
///
///        libc_verify covers printf-write (%d-only) + scanf-read (1234).
///        This file covers the remaining bullets against the picolibc
///        Ring-3 image (LIBC_VERIFY markers, 2 KiB capture): full format
///        specifiers, snprintf bounds, fprintf-to-fd (byte-verified
///        kernel-side), scanf hex/string + sscanf mismatch. The in-tree
///        libc has no scanf/sscanf/fprintf at all (confirmed gap — see
///        the #303 tracker); %lu vectors stay manual (printf-probe.c).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/test/test_isolate.hpp>
#include "test_ring3_vehicle.hpp"

using namespace kernel;

#if defined(CONFIG_ARCH_X86_64)

namespace {

constexpr const char *kFprintfPath = "/tmp/libc_fprintf.txt";
constexpr size_t kCapSize = 2048;

void stage_fprintf_file() {
    test::ring3::write_data_file(kFprintfPath, nullptr, 0);
}

// NOTE: the fprintf file is NOT unlinked by run(): the test reads it
// back after run() returns, then unlinks it explicitly (run() only owns
// image/stdin/stdout).

} // namespace

// Runmode: kernel
// Testidea: %d/%x/%s/%c/%p render the exact expected output in Ring 3.
// Input: Dispatch the verify image; parse the fmt marker.
// Expect: Joined clean; "fmt -42 abcd hi Q " + "400000" present.
// Depends: picolibc printf -> WRITE (issues #71/#73), vehicle header
JARVIS_TEST(libc_stdio_format_specifiers, "PRE: vfsd | POST: none") {
    char capture[kCapSize]{};
    test::ring3::Outcome o = test::ring3::run(nullptr, nullptr, capture, kCapSize);
    JARVIS_ASSERT(o.joined);
    JARVIS_ASSERT(test::ring3::serial_contains(capture,
                                         "LIBC_VERIFY: fmt -42 abcd hi Q "));
    JARVIS_ASSERT(test::ring3::serial_contains(capture, "400000"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: snprintf truncates at the bound, NUL-terminates, and reports
// the would-be length.
// Input: Same dispatch; parse the snprintf marker (20-char input, 16 B
// buffer).
// Expect: r=20, len=15, content "0123456789ABCDE".
// Depends: picolibc snprintf
JARVIS_TEST(libc_stdio_snprintf_bounds, "PRE: vfsd | POST: none") {
    char capture[kCapSize]{};
    test::ring3::Outcome o = test::ring3::run(nullptr, nullptr, capture, kCapSize);
    JARVIS_ASSERT(o.joined);
    uint64_t v[2] = {};
    JARVIS_ASSERT(test::ring3::parse_tag_line(capture, "LIBC_VERIFY: snprintf",
                                        v, 2, kCapSize));
    JARVIS_ASSERT_FMT(v[0] == 20, "snprintf return %lu != 20", v[0]);
    JARVIS_ASSERT_FMT(v[1] == 15, "snprintf len %lu != 15", v[1]);
    JARVIS_ASSERT(test::ring3::serial_contains(capture, "0123456789ABCDE"));
    JARVIS_ASSERT_FMT(!test::ring3::serial_contains(capture, "0123456789ABCDEF"),
                      "snprintf wrote past the bound");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: sscanf mismatch yields 0 matches; hex + capped-%s convert.
// Input: Same dispatch; parse the sscanf marker.
// Expect: "sscanf 0 2 16 hello".
// Depends: picolibc sscanf (in-tree libc has none — #303 gap)
JARVIS_TEST(libc_stdio_sscanf, "PRE: vfsd | POST: none") {
    char capture[kCapSize]{};
    test::ring3::Outcome o = test::ring3::run(nullptr, nullptr, capture, kCapSize);
    JARVIS_ASSERT(o.joined);
    JARVIS_ASSERT(test::ring3::serial_contains(capture,
                                         "LIBC_VERIFY: sscanf 0 2 16 hello"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: fprintf to an explicit fd lands byte-exact in the file.
// Input: Pre-create /tmp/libc_fprintf.txt; image fdopens+fprintf+fclose;
// read the file back kernel-side.
// Expect: fd >= 0; file content exactly "LIBC_VERIFY file 7\n".
// Depends: picolibc fdopen/fprintf + _open/_write glue, tmpfs
JARVIS_TEST(libc_stdio_fprintf_to_fd, "PRE: vfsd | POST: none") {
    char capture[kCapSize]{};
    test::ring3::Outcome o =
        test::ring3::run(stage_fprintf_file, nullptr, capture, kCapSize);
    JARVIS_ASSERT(o.joined);
    uint64_t v[1] = {};
    JARVIS_ASSERT(test::ring3::parse_tag_line(capture, "LIBC_VERIFY: fprintf_fd",
                                        v, 1, kCapSize));
    JARVIS_ASSERT_FMT(static_cast<int64_t>(v[0]) >= 0,
                      "fprintf open failed (fd marker %lu)", v[0]);
    char content[64]{};
    int64_t n = test::ring3::read_file(kFprintfPath, content, sizeof(content));
    JARVIS_ASSERT_FMT(n == 19, "fprintf file is %ld bytes, want 19", n);
    const char *want = "LIBC_VERIFY file 7\n";
    for (int64_t i = 0; i < n; ++i)
        JARVIS_ASSERT_FMT(content[static_cast<size_t>(i)] == want[i],
                          "fprintf byte %ld mismatch", i);
    test::ring3::unlink_path(kFprintfPath);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The program survives every stdio edge section (fdopen,
// truncation, mismatch scans) to clean EXIT(0).
// Input: Same dispatch.
// Expect: Joined clean.
// Depends: full image run incl. #303 sections
JARVIS_TEST(libc_stdio_full_run_clean, "PRE: vfsd | POST: none") {
    char capture[kCapSize]{};
    test::ring3::Outcome o = test::ring3::run(nullptr, nullptr, capture, kCapSize);
    JARVIS_ASSERT(o.joined);
    JARVIS_ASSERT(o.clean_exit);
    JARVIS_TEST_PASS();
}

#endif // CONFIG_ARCH_X86_64

// Runmode: kernel
// Testidea: Registers all libc stdio tests.
// Input: None
// Expect: All stdio tests registered
// Depends: test framework
void register_libc_stdio_tests() {
    Logger::info("Registering libc stdio tests");
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_REGISTER_TEST(libc_stdio_format_specifiers);
    JARVIS_REGISTER_TEST(libc_stdio_snprintf_bounds);
    JARVIS_REGISTER_TEST(libc_stdio_sscanf);
    JARVIS_REGISTER_TEST(libc_stdio_fprintf_to_fd);
    JARVIS_REGISTER_TEST(libc_stdio_full_run_clean);
#endif
}
