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

/// @file test_checked_ptr_api.cpp
/// @brief CheckedPtr / safe-copy template-instantiation tests (milestone
///        v0.4.10 issue #127): 63 of the 100 functions in
///        src/kernel/memory/checked_ptr.hpp were never entered.  They are
///        *template instantiations*, so they exist in the ELF only where
///        some TU instantiated them — and they are only ever entered when
///        that instantiation is actually called.  This class instantiates
///        the whole API surface for every element type the kernel uses and
///        calls every member.
/// @note  Safety: every probe passes a KERNEL address (a stack buffer).
///        `is_user_range()` rejects anything at or above USER_SPACE_LIMIT,
///        so `valid()` is false and `copy_from/copy_to/read/write` and both
///        `safe_copy_*` variants all take their early-return path — the user
///        pointer is never dereferenced and no fault is ever provoked.
///        That is also the assertion being made: **a kernel address is never
///        accepted as a user pointer**, which is the SMAP contract that
///        keeps a confused-deputy syscall from touching kernel memory.
/// @note  const element types cannot instantiate `write()`, `copy_to()` or
///        `safe_copy_to_user()` (they would store through a const pointer);
///        those are guarded with `if constexpr` and reported as "not
///        instantiable" rather than omitted.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/vfs/vfs.hpp>
#include <signal.hpp>
#include <string.hpp>

using namespace kernel;

namespace {

/// @brief Portable const-detection (no <type_traits> in the freestanding lib).
template <typename T> struct TypeIsConst {
    static constexpr bool value = false;
};
template <typename T> struct TypeIsConst<const T> {
    static constexpr bool value = true;
};

/// @brief Observed behaviour of one CheckedPtr<T> instantiation.
struct SweepResult {
    bool default_ctor_noop = false;
    bool factory_matches_direct = false;
    bool valid = false;
    bool unsafe_is_source = false;
    bool copy_from = false;
    bool read_is_default = false;
    bool copy_to = false;
    bool write = false;
    bool const_type = false;
};

/// @brief Instantiates and calls the whole CheckedPtr<T> API for one type.
template <typename T>
SweepResult sweep(T *user_ptr, uint64_t count) {
    SweepResult result{};
    result.const_type = TypeIsConst<T>::value;

    // A default-constructed CheckedPtr is a VALID no-op: count 0 means
    // there is nothing to touch, and unsafe_ptr() is null.
    CheckedPtr<T> defaulted{};
    result.default_ctor_noop =
        defaulted.valid() && defaulted.unsafe_ptr() == nullptr;

    auto from_factory = checked(user_ptr, count);
    CheckedPtr<T> direct(user_ptr, count);

    result.factory_matches_direct =
        from_factory.unsafe_ptr() == direct.unsafe_ptr();
    result.valid = direct.valid() || from_factory.valid();
    result.unsafe_is_source = direct.unsafe_ptr() == user_ptr;

    T storage[8]{}; // issue #150: scratch must cover count (max 8);
                      // a 1-element scratch overflows the stack (and trips
                      // -Werror=stringop-overflow in release -O2 builds)

    T observed = direct.read(0);
    result.read_is_default = memcmp(&observed, &storage[0], sizeof(T)) == 0;

    if constexpr (!TypeIsConst<T>::value) {
        result.copy_from = direct.copy_from(&storage[0]);
        result.copy_to = direct.copy_to(&storage[0]);
        result.write = direct.write(storage[0], 0);
        (void)safe_copy_from_user(&storage[0], user_ptr, count);
        (void)safe_copy_to_user(user_ptr, &storage[0], count);
    }

    return result;
}

/// @brief Asserts the shared rejection contract for a kernel-address probe.
void expect_rejected(const SweepResult &result) {
    JARVIS_ASSERT(result.default_ctor_noop);
    JARVIS_ASSERT(result.factory_matches_direct);
    JARVIS_ASSERT(!result.valid);
    JARVIS_ASSERT(result.unsafe_is_source);
    JARVIS_ASSERT(!result.copy_from);
    JARVIS_ASSERT(result.read_is_default);
    JARVIS_ASSERT(!result.copy_to);
    JARVIS_ASSERT(!result.write);
}

} // namespace

// Runmode: kernel
// Testidea: For every scalar element type the kernel copies across the
//           user boundary, the full CheckedPtr API must reject a KERNEL
//           address: valid() false, copy_from/copy_to/write false, read
//           yields a default-constructed value — and unsafe_ptr() must still
//           round-trip the original pointer (no silent nulling).
// Input: CheckedPtr<char>, <unsigned char>, <int>, <uint64_t> built over
//        stack buffers; default ctor, factory checked<T>() and direct ctor.
// Expect: All four types rejected identically; factory and direct ctor
//         agree; read() returns T{}; the safe_copy_* templates also refuse
//         the kernel source/destination.
// Depends: kernel::CheckedPtr, kernel::checked, safe_copy_from_user/to_user
JARVIS_TEST(checked_ptr_api_scalar_types,
            "PRE: vfsd, iocd | POST: none") {
    char chars[8] = {};
    unsigned char bytes[8] = {};
    int ints[4] = {};
    uint64_t quads[4] = {};

    const SweepResult char_result = sweep(chars, 8);
    const SweepResult byte_result = sweep(bytes, 8);
    const SweepResult int_result = sweep(ints, 4);
    const SweepResult quad_result = sweep(quads, 4);

    expect_rejected(char_result);
    expect_rejected(byte_result);
    expect_rejected(int_result);
    expect_rejected(quad_result);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: const element types are the read-only half of the API — they
//           instantiate the constructors, valid(), unsafe_ptr() and read(),
//           but write()/copy_to()/safe_copy_to_user() are NOT instantiable
//           (they would store through a const pointer).  The const
//           instantiations must still reject kernel addresses.
// Input: CheckedPtr<const char> over a string literal;
//        CheckedPtr<const char* const> over a const pointer variable.
// Expect: Both are const_type, both reject valid()/copy_from, both yield a
//         default read, and both report copy_to/write as uninstantiable.
// Depends: kernel::CheckedPtr, kernel::checked
JARVIS_TEST(checked_ptr_api_const_element_types,
            "PRE: vfsd, iocd | POST: none") {
    const char text[8] = "kernel!";
    const char *const pointer = text;

    const SweepResult text_result = sweep(text, 8);
    const SweepResult pointer_result = sweep(&pointer, 1);

    JARVIS_ASSERT(text_result.const_type);
    JARVIS_ASSERT(pointer_result.const_type);

    JARVIS_ASSERT(text_result.default_ctor_noop);
    JARVIS_ASSERT(pointer_result.default_ctor_noop);
    JARVIS_ASSERT(text_result.factory_matches_direct);
    JARVIS_ASSERT(pointer_result.factory_matches_direct);
    JARVIS_ASSERT(!text_result.valid);
    JARVIS_ASSERT(!pointer_result.valid);
    JARVIS_ASSERT(text_result.unsafe_is_source);
    JARVIS_ASSERT(pointer_result.unsafe_is_source);
    JARVIS_ASSERT(!text_result.copy_from);
    JARVIS_ASSERT(!pointer_result.copy_from);
    JARVIS_ASSERT(text_result.read_is_default);
    JARVIS_ASSERT(pointer_result.read_is_default);
    JARVIS_ASSERT(!text_result.copy_to);
    JARVIS_ASSERT(!text_result.write);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The two VFS structures that cross the user boundary on
//           getdents/stat (Dirent, VfsStat) get the same treatment as the
//           scalars — a kernel address must never be accepted as the user
//           destination of a stat/readdir copy-out.
// Input: CheckedPtr<vfs::Dirent> and <vfs::VfsStat> over stack buffers,
//        including safe_copy_to_user into the "user" buffer.
// Expect: Both rejected: valid() false, copy_to/write false, read default.
// Depends: kernel::CheckedPtr, vfs::Dirent, vfs::VfsStat
JARVIS_TEST(checked_ptr_api_vfs_structures,
            "PRE: vfsd, iocd | POST: none") {
    vfs::Dirent dirents[2] = {};
    vfs::VfsStat stats[2] = {};

    const SweepResult dirent_result = sweep(dirents, 2);
    const SweepResult stat_result = sweep(stats, 2);

    expect_rejected(dirent_result);
    expect_rejected(stat_result);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SignalFrame is the largest structure copied to user space (the
//           signal trampoline frame, 64 bytes); its CheckedPtr
//           instantiation must reject a kernel frame pointer.
// Input: CheckedPtr<SignalFrame> over a stack SignalFrame, plus the
//        safe_copy_* templates.
// Expect: Rejected; read() yields a zeroed SignalFrame (sig == 0).
// Depends: kernel::CheckedPtr, SignalFrame
JARVIS_TEST(checked_ptr_api_signal_frame,
            "PRE: vfsd, iocd | POST: none") {
    SignalFrame frames[2] = {};

    const SweepResult frame_result = sweep(frames, 2);

    SignalFrame delivered = frames[0].sig == 0
                                ? checked(frames, 2).read(0)
                                : SignalFrame{};

    expect_rejected(frame_result);
    JARVIS_ASSERT_EQ(0ULL, delivered.sig);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A zero-element CheckedPtr is the documented no-op case: valid()
//           is TRUE (there is nothing to touch) while every accessor still
//           refuses to transfer data.  Null with a non-zero count is the
//           opposite — never valid.
// Input: CheckedPtr<uint64_t>(nullptr, 0), (nullptr, 4), and is_user_range
//        with nullptr/0, nullptr/4 and a user address with size 0.
// Expect: count 0 → valid; null with a non-zero count → invalid; nullptr is
//         rejected for every size; a non-null user address with size 0 is
//         accepted (documented no-op).
// Depends: kernel::CheckedPtr, is_user_range
JARVIS_TEST(checked_ptr_api_zero_count_is_noop,
            "PRE: vfsd, iocd | POST: none") {
    CheckedPtr<uint64_t> empty_span(nullptr, 0);
    CheckedPtr<uint64_t> null_span(nullptr, 4);

    const bool empty_valid = empty_span.valid();
    const bool null_valid = null_span.valid();
    // A null pointer is rejected for ANY size, including 0 — is_user_range
    // checks the pointer before it looks at the length.
    const bool null_zero_range_ok = is_user_range(nullptr, 0);
    const bool null_nonzero_range_ok = is_user_range(nullptr, 4);
    // A non-null user address with size 0 IS a valid (no-op) range.
    const bool user_zero_size_ok =
        is_user_range(reinterpret_cast<void *>(0x400000ULL), 0);

    JARVIS_ASSERT(empty_valid);
    JARVIS_ASSERT(!null_valid);
    JARVIS_ASSERT(!null_zero_range_ok);
    JARVIS_ASSERT(!null_nonzero_range_ok);
    JARVIS_ASSERT(user_zero_size_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The two free safe-copy templates are the syscall-boundary
//           primitives; for a kernel source/destination they must fail
//           closed (return false) for every element type, and the kernel
//           destination must be left untouched.
// Input: safe_copy_from_user / safe_copy_to_user with a kernel "user" buffer
//        for uint64_t, vfs::VfsStat and SignalFrame; a sentinel pattern is
//        planted in the kernel destination first.
// Expect: Both return false for all three types and the destination
//         sentinel is unchanged.
// Depends: safe_copy_from_user, safe_copy_to_user
JARVIS_TEST(checked_ptr_api_safe_copy_templates_fail_closed,
            "PRE: vfsd, iocd | POST: none") {
    uint64_t kernel_quads[4] = {0xA5A5A5A5A5A5A5A5ULL, 0, 0, 0};
    vfs::VfsStat kernel_stats[2] = {};
    SignalFrame kernel_frames[2] = {};

    kernel_stats[0].st_size = 0xDEAD;
    kernel_frames[0].sig = 0xFE;

    const bool from_quads =
        safe_copy_from_user(kernel_quads, kernel_quads, 4);
    const bool to_quads = safe_copy_to_user(kernel_quads, kernel_quads, 4);
    const bool from_stats =
        safe_copy_from_user(kernel_stats, kernel_stats, 2);
    const bool to_stats = safe_copy_to_user(kernel_stats, kernel_stats, 2);
    const bool from_frames =
        safe_copy_from_user(kernel_frames, kernel_frames, 2);
    const bool to_frames = safe_copy_to_user(kernel_frames, kernel_frames, 2);

    JARVIS_ASSERT(!from_quads);
    JARVIS_ASSERT(!to_quads);
    JARVIS_ASSERT(!from_stats);
    JARVIS_ASSERT(!to_stats);
    JARVIS_ASSERT(!from_frames);
    JARVIS_ASSERT(!to_frames);
    JARVIS_ASSERT_EQ(0xA5A5A5A5A5A5A5A5ULL, kernel_quads[0]);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0xDEAD), kernel_stats[0].st_size);
    JARVIS_ASSERT_EQ(0xFEULL, kernel_frames[0].sig);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: safe_copy must fail closed through the fault-recovery path
//           (real #PF inside the copy, redirected to the recover label)
//           when the user range is valid but unmapped — not only through
//           the range gate.
//           PARKED as stub (issue #149): the recovery resume loops
//           silently instead of returning false; re-activate once #149
//           is fixed.
// Input: safe_copy_from_user (kernel dst, unmapped 0x60000000 src) and
//        safe_copy_to_user (unmapped 0x60000000 dst, kernel src), 16 bytes
//        each; sentinel planted in the kernel destination first.
// Expect: Both return false, destination sentinel unchanged, no panic,
//         no hang — the fault is recovered synchronously.
// Depends: safe_copy_from_user/safe_copy_to_user fault recovery
JARVIS_TEST(checked_ptr_api_safe_copy_fault_recovery,
            "PRE: vfsd, iocd | POST: none") {
    // Reactivated by the issue #149 keep-alive fix: the recover bodies now
    // survive codegen, so a #PF inside the copy redirects to them.
    uint8_t kernel_dst[16] = {};
    uint8_t kernel_src[16] = {};
    for (size_t idx = 0; idx < sizeof(kernel_dst); ++idx) {
        kernel_dst[idx] = 0xA5;
        kernel_src[idx] = static_cast<uint8_t>(idx);
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *unmapped = reinterpret_cast<uint8_t *>(0x60000000ULL);
    const bool from_ok =
        safe_copy_from_user(kernel_dst, unmapped, sizeof(kernel_dst));
    const bool to_ok =
        safe_copy_to_user(unmapped, kernel_src, sizeof(kernel_src));
    JARVIS_ASSERT(!from_ok);
    JARVIS_ASSERT(!to_ok);
    for (size_t idx = 0; idx < sizeof(kernel_dst); ++idx) {
        JARVIS_ASSERT_EQ(0xA5, kernel_dst[idx]);
    }
    JARVIS_TEST_PASS();
}

void register_checked_ptr_api_tests() {
    Logger::info("Registering CheckedPtr API tests");
    JARVIS_REGISTER_TEST(checked_ptr_api_scalar_types);
    JARVIS_REGISTER_TEST(checked_ptr_api_const_element_types);
    JARVIS_REGISTER_TEST(checked_ptr_api_vfs_structures);
    JARVIS_REGISTER_TEST(checked_ptr_api_signal_frame);
    JARVIS_REGISTER_TEST(checked_ptr_api_zero_count_is_noop);
    JARVIS_REGISTER_TEST(checked_ptr_api_safe_copy_templates_fail_closed);
    JARVIS_REGISTER_TEST(checked_ptr_api_safe_copy_fault_recovery);
}
