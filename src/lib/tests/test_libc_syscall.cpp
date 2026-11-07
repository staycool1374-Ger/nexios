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

/// @file test_libc_syscall.cpp
/// @brief Libc-integration coverage (issue #303, Phase 6 module 13).
///
///        printf->WRITE and malloc->BRK paths are covered by libc_verify.
///        This file pins what a kernel test CAN observe: the embedded
///        image stays within the tmpfs file cap (binary-size bullet,
///        measured — not estimated), the Ring-3 getpid loop publishes
///        the syscall entry/exit distribution (also serving Phase 6
///        module 4), the single-write crossing publishes the
///        kernel<->user round-trip cost, and a kernel canary buffer
///        proves the user program corrupts no kernel memory
///        (no-corruption bullet). "Links unmodified" is a build rule
///        (mk/rules.mk picolibc pattern rule) — evidenced by the build
///        itself, see the #303 tracker.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/memory/mempool.hpp>
#include "test_ring3_vehicle.hpp"

using namespace kernel;

#if defined(CONFIG_ARCH_X86_64)

extern "C" {
extern const uint8_t _binary_libc_verify_img_start[];
extern const uint8_t _binary_libc_verify_img_end[];
}

namespace {

constexpr size_t kCapSize = 2048;
// tmpfs caps a single file at 64 KiB (mk/rules.mk, issue #75); the image
// must stage through it with headroom to spare.
constexpr size_t kTmpfsFileCap = 64 * 1024;

} // namespace

// Runmode: kernel
// Testidea: The stripped, embedded user image fits the tmpfs staging cap
// with headroom (binary-size bullet, measured from the linked symbols).
// Input: _binary_libc_verify_img_{start,end} distance.
// Expect: 0 < size <= 48 KiB (cap 64 KiB minus staging slack).
// Depends: mk/rules.mk VERIFY_IMG_* rules
JARVIS_TEST(libc_syscall_image_size_bounded, "PRE: none | POST: none") {
    size_t size = static_cast<size_t>(_binary_libc_verify_img_end -
                                      _binary_libc_verify_img_start);
    Logger::info("LIBC image: %u bytes (tmpfs cap %u)", size,
                 kTmpfsFileCap);
    JARVIS_ASSERT_FMT(size > 0, "embedded image is empty");
    JARVIS_ASSERT_FMT(size <= 48 * 1024,
                      "image %u bytes exceeds 48 KiB staging budget", size);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Syscall entry/exit distribution from Ring 3 (2000 getpid
// round-trips timed in-image by rdtsc).
// Input: Dispatch; parse the cs_getpid marker (min/max/avg cycles).
// Expect: Joined; avg < 5M cycles sanity bound; publish the triple.
// Depends: _getpid stub -> SYS_GETPID (issue #71 stubs)
JARVIS_TEST(libc_syscall_getpid_entry_exit, "PRE: vfsd | POST: none") {
    char capture[kCapSize]{};
    test::ring3::Outcome o = test::ring3::run(nullptr, nullptr, capture, kCapSize);
    JARVIS_ASSERT(o.joined);
    uint64_t v[3] = {};
    JARVIS_ASSERT(test::ring3::parse_tag_line(capture, "LIBC_VERIFY: cs_getpid",
                                        v, 3, kCapSize));
    Logger::info("SYSCALL getpid: min=%lu max=%lu avg=%lu cycles", v[0],
                 v[1], v[2]);
    // NOTE: min may read 0 under TCG (sub-resolution fast path) — avg>0
    // is the liveness guard, not min.
    JARVIS_ASSERT_FMT(v[2] > 0, "getpid avg is 0 (timer dead?)");
    JARVIS_ASSERT_FMT(v[0] <= v[2] && v[2] <= v[1],
                      "getpid avg outside [min,max]");
    JARVIS_ASSERT_FMT(v[2] < 5000000,
                      "getpid avg %lu above sanity bound", v[2]);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Kernel<->user crossing cost: one single-byte WRITE per sample
// (user -> syscall -> vfsd send_sync -> reply -> user), 200 samples.
// Input: Dispatch; parse the cs_writecross marker.
// Expect: Joined; avg < 50M cycles sanity bound; publish the triple.
// Depends: WRITE path incl. vfsd send_sync (issue #317 contract)
JARVIS_TEST(libc_syscall_write_crossing, "PRE: vfsd | POST: none") {
    char capture[kCapSize]{};
    test::ring3::Outcome o = test::ring3::run(nullptr, nullptr, capture, kCapSize);
    JARVIS_ASSERT(o.joined);
    uint64_t v[3] = {};
    JARVIS_ASSERT(test::ring3::parse_tag_line(
        capture, "LIBC_VERIFY: cs_writecross", v, 3, kCapSize));
    Logger::info("SYSCALL write-cross: min=%lu max=%lu avg=%lu cycles",
                 v[0], v[1], v[2]);
    JARVIS_ASSERT_FMT(v[2] > 0, "write-cross avg is 0 (timer dead?)");
    JARVIS_ASSERT_FMT(v[0] <= v[2] && v[2] <= v[1],
                      "write-cross avg outside [min,max]");
    JARVIS_ASSERT_FMT(v[2] < 50000000,
                      "write-cross avg %lu above sanity bound", v[2]);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A hostile user program cannot corrupt kernel memory: a
// canary-filled MemPool block is bit-identical after the full image run
// (which performs file I/O, 2 GiB sbrk, realloc and brk churn).
// Input: 256 B canary block; dispatch; memcmp after teardown.
// Expect: Joined; zero mismatches.
// Depends: MemPool alloc/free, vehicle teardown discipline
JARVIS_TEST(libc_syscall_no_kernel_corruption, "PRE: vfsd | POST: none") {
    constexpr size_t k_size = 256;
    auto *canary = static_cast<uint8_t *>(MemPool::alloc(k_size));
    JARVIS_ASSERT(canary != nullptr);
    for (size_t i = 0; i < k_size; ++i)
        canary[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);

    char capture[kCapSize]{};
    test::ring3::Outcome o = test::ring3::run(nullptr, nullptr, capture, kCapSize);
    JARVIS_ASSERT(o.joined);

    uint64_t bad = 0;
    for (size_t i = 0; i < k_size; ++i) {
        if (canary[i] != static_cast<uint8_t>((i * 31 + 7) & 0xFF))
            ++bad;
    }
    MemPool::free(canary);
    JARVIS_ASSERT_FMT(bad == 0, "%lu canary bytes corrupted", bad);
    JARVIS_TEST_PASS();
}

#endif // CONFIG_ARCH_X86_64

// Runmode: kernel
// Testidea: Registers all libc-integration tests.
// Input: None
// Expect: All integration tests registered
// Depends: test framework
void register_libc_syscall_tests() {
    Logger::info("Registering libc syscall-integration tests");
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_REGISTER_TEST(libc_syscall_image_size_bounded);
    JARVIS_REGISTER_TEST(libc_syscall_getpid_entry_exit);
    JARVIS_REGISTER_TEST(libc_syscall_write_crossing);
    JARVIS_REGISTER_TEST(libc_syscall_no_kernel_corruption);
#endif
}
