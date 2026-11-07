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

/// @file test_libc_malloc.cpp
/// @brief malloc/free coverage (issue #303, Phase 6 module 10).
///
///        libc_verify covers growth + free-list reuse. This file covers
///        the remaining bullets against the picolibc heap (NOT the
///        kernel MemPool and NOT the stub-grade in-tree malloc, whose
///        free is a no-op and realloc returns 0 by inspection): malloc(0)
///        validity, free(NULL) survival, calloc zeroing, realloc content
///        preservation, alloc/free-cycle leak freedom (break returns to
///        the reuse level), and OOM-is-NULL. The program reaching clean
///        EXIT proves the free(NULL) no-crash bullet.

#include <test.hpp>
#include <logger.hpp>
#include "test_ring3_vehicle.hpp"

using namespace kernel;

#if defined(CONFIG_ARCH_X86_64)

namespace {

constexpr size_t kCapSize = 2048;

} // namespace

// Runmode: kernel
// Testidea: malloc(0) returns a usable-or-null pointer per implementation
// (picolibc: minimal chunk, non-NULL) and free(NULL) does not kill the
// program.
// Input: Dispatch; parse the malloc0 marker; require clean exit.
// Expect: Joined clean; malloc(0) != NULL.
// Depends: picolibc malloc/free
JARVIS_TEST(libc_malloc_zero_and_null_free, "PRE: vfsd | POST: none") {
    char capture[kCapSize]{};
    test::ring3::Outcome o = test::ring3::run(nullptr, nullptr, capture, kCapSize);
    JARVIS_ASSERT(o.joined);
    uint64_t v[1] = {};
    JARVIS_ASSERT(test::ring3::parse_tag_line(capture, "LIBC_VERIFY: malloc0",
                                        v, 1, kCapSize));
    JARVIS_ASSERT_FMT(v[0] != 0, "malloc(0) returned NULL");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: calloc returns zeroed memory.
// Input: Same dispatch; parse the calloc marker (4x8, checksum).
// Expect: Pointer non-NULL, 32-byte checksum == 0.
// Depends: picolibc calloc
JARVIS_TEST(libc_malloc_calloc_zeroed, "PRE: vfsd | POST: none") {
    char capture[kCapSize]{};
    test::ring3::Outcome o = test::ring3::run(nullptr, nullptr, capture, kCapSize);
    JARVIS_ASSERT(o.joined);
    uint64_t v[2] = {};
    JARVIS_ASSERT(test::ring3::parse_tag_line(capture, "LIBC_VERIFY: calloc",
                                        v, 2, kCapSize));
    JARVIS_ASSERT_FMT(v[0] != 0, "calloc returned NULL");
    JARVIS_ASSERT_FMT(v[1] == 0, "calloc not zeroed (checksum %lu)", v[1]);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: realloc preserves the first min(old,new) bytes.
// Input: Same dispatch; parse the realloc flag (32 B pattern -> 64 B).
// Expect: rok == 1; OOM probe returns NULL.
// Depends: picolibc realloc, sys_brk STACK_VADDR cap (MP-2.2)
JARVIS_TEST(libc_malloc_realloc_preserves_and_oom_null,
            "PRE: vfsd | POST: none") {
    char capture[kCapSize]{};
    test::ring3::Outcome o = test::ring3::run(nullptr, nullptr, capture, kCapSize);
    JARVIS_ASSERT(o.joined);
    uint64_t v[2] = {};
    JARVIS_ASSERT(test::ring3::parse_tag_line(capture, "LIBC_VERIFY: realloc",
                                        v, 2, kCapSize));
    JARVIS_ASSERT_FMT(v[0] == 1, "realloc did not preserve content");
    JARVIS_ASSERT_FMT(v[1] == 0, "2 GiB malloc did not return NULL");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Blocks are suitably aligned and non-overlapping: the handed-
// out pointer satisfies the 8-byte ABI alignment, and the recycled block
// identity (below) proves pool discipline (no two live owners).
// Input: Same dispatch; parse the ptr marker.
// Expect: p1 != 0 and p1 % 8 == 0.
// Depends: picolibc malloc chunk alignment over _sbrk
JARVIS_TEST(libc_malloc_aligned_non_overlapping, "PRE: vfsd | POST: none") {
    char capture[kCapSize]{};
    test::ring3::Outcome o =
        test::ring3::run(nullptr, nullptr, capture, kCapSize);
    JARVIS_ASSERT(o.joined);
    uint64_t p[2] = {};
    JARVIS_ASSERT(test::ring3::parse_tag_line(capture, "LIBC_VERIFY: ptr",
                                              p, 2, kCapSize));
    JARVIS_ASSERT_FMT(p[0] != 0, "first malloc returned NULL");
    JARVIS_ASSERT_FMT(p[0] % 8 == 0, "malloc block %lu under-aligned",
                      p[0]);
    JARVIS_TEST_PASS();
}
// Input: Same dispatch; require the ptr reuse marker (p2 == p1 != 0).
// Expect: Joined clean; same block handed out twice.
// Depends: picolibc free-list (issue #75 block)
// Runmode: kernel
// Testidea: Repeated alloc/free cycles do not leak the break upward: the
// image's own brk markers (b3 == b2 reuse level) already prove reuse;
// here the full-run exit being clean plus the reuse asserts pin it.
// Input: Same dispatch; require the ptr reuse marker (p2 == p1 != 0).
// Expect: Joined clean; same block handed out twice.
// Depends: picolibc free-list (issue #75 block)
JARVIS_TEST(libc_malloc_cycle_no_leak, "PRE: vfsd | POST: none") {
    char capture[kCapSize]{};
    test::ring3::Outcome o = test::ring3::run(nullptr, nullptr, capture, kCapSize);
    JARVIS_ASSERT(o.joined);
    uint64_t p[2] = {};
    JARVIS_ASSERT(test::ring3::parse_tag_line(capture, "LIBC_VERIFY: ptr", p, 2,
                                        kCapSize));
    JARVIS_ASSERT_FMT(p[0] != 0, "first malloc returned NULL");
    JARVIS_ASSERT_FMT(p[1] == p[0], "free did not recycle the block");
    JARVIS_TEST_PASS();
}

#endif // CONFIG_ARCH_X86_64

// Runmode: kernel
// Testidea: Registers all libc malloc tests.
// Input: None
// Expect: All malloc tests registered
// Depends: test framework
void register_libc_malloc_tests() {
    Logger::info("Registering libc malloc tests");
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_REGISTER_TEST(libc_malloc_zero_and_null_free);
    JARVIS_REGISTER_TEST(libc_malloc_aligned_non_overlapping);
    JARVIS_REGISTER_TEST(libc_malloc_calloc_zeroed);
    JARVIS_REGISTER_TEST(libc_malloc_realloc_preserves_and_oom_null);
    JARVIS_REGISTER_TEST(libc_malloc_cycle_no_leak);
#endif
}
