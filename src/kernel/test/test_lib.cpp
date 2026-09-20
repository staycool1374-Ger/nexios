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

/// @file test_lib.cpp
/// @brief Kernel library utility tests.

#include <test.hpp>
#include <string.hpp>
#include <utils.hpp>
#include <error.hpp>
#include <version.hpp>
#include <crc32.hpp>
#include <fdt/fdt.h>
#include <fdt/libfdt.h>
#include <fdt/libfdt_internal.h>

using namespace kernel;

// Runmode: kernel
// Testidea: Test strlen with empty string, typical strings, and a single
// character.
// Input: strlen(""), strlen("hello"), strlen("Hello, World!"), strlen("x").
// Expect: Correct lengths returned; ternary expression returns expected value.
// Depends: string
JARVIS_TEST(string_strlen, "PRE: none | POST: none") {
    JARVIS_ASSERT_EQ(0, strlen(""));
    JARVIS_ASSERT_EQ(5, strlen("hello"));
    JARVIS_ASSERT_EQ(13, strlen("Hello, World!"));
    JARVIS_ASSERT_EQ(1, strlen("x"));
    JARVIS_ASSERT_HEX_EQ(0, strlen("") ? 1 : 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Test strcmp with identical, different-length, and
// lexicographically ordered strings.
// Input: strcmp on pairs of "", "abc", "abd", "ab", "identical".
// Expect: 0 for exact matches, negative for less-than, positive for
// greater-than, non-zero for different lengths.
// Depends: string
JARVIS_TEST(string_strcmp, "PRE: none | POST: none") {
    JARVIS_ASSERT_EQ(0, strcmp("", ""));
    JARVIS_ASSERT_EQ(0, strcmp("abc", "abc"));
    JARVIS_ASSERT(strcmp("abc", "abd") < 0);
    JARVIS_ASSERT(strcmp("abd", "abc") > 0);
    JARVIS_ASSERT(strcmp("abc", "ab") != 0);
    JARVIS_ASSERT_EQ(0, strcmp("identical", "identical"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Test strncmp with limited-length comparisons including prefix
// matches and boundary cases.
// Input: strncmp with lengths 0–6 on various string pairs including
// prefix-only matches.
// Expect: 0 when limited prefix matches, correct ordering for mismatches, 0
// for zero-length compare.
// Depends: string
JARVIS_TEST(string_strncmp, "PRE: none | POST: none") {
    JARVIS_ASSERT_EQ(0, strncmp("abcde", "abcde", 5));
    JARVIS_ASSERT_EQ(0, strncmp("abcde", "abcxx", 3));
    JARVIS_ASSERT(strncmp("abcde", "abdde", 3) < 0);
    JARVIS_ASSERT_EQ(0, strncmp("", "", 0));
    JARVIS_ASSERT_EQ(0, strncmp("abcdef", "abc", 3));
    JARVIS_ASSERT(strncmp("abc", "abcdef", 6) < 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Test strncpy copies strings correctly into a buffer, including
// empty and short strings.
// Input: strncpy with "hello", "", "short" into a 32-byte buffer pre-filled
// with 0xAA.
// Expect: Buffer matches source string after each copy.
// Depends: string
JARVIS_TEST(string_strncpy, "PRE: none | POST: none") {
    char buf[32];
    memset(buf, 0xAA, sizeof(buf));
    strncpy(buf, "hello", sizeof(buf));
    JARVIS_ASSERT_EQ(0, strcmp(buf, "hello"));
    strncpy(buf, "", sizeof(buf));
    JARVIS_ASSERT_EQ(0, strcmp(buf, ""));
    strncpy(buf, "short", sizeof(buf));
    JARVIS_ASSERT_EQ(0, strcmp(buf, "short"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Test memcpy copies memory correctly between non-overlapping
// buffers.
// Input: memcpy from a string literal to a 64-byte zeroed destination buffer.
// Expect: Destination matches source string.
// Depends: string
JARVIS_TEST(string_memcpy, "PRE: none | POST: none") {
    const char src[] = "memory test data";
    char dst[64];
    memset(dst, 0, sizeof(dst));
    memcpy(dst, src, sizeof(src));
    JARVIS_ASSERT_EQ(0, strcmp(dst, src));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Test memset fills memory with a byte value and then zeroes it
// correctly.
// Input: memset(buf, 0xFF, 32) then memset(buf, 0, 32).
// Expect: All 32 bytes equal 0xFF after first fill, all bytes equal 0 after
// zero fill.
// Depends: string
JARVIS_TEST(string_memset, "PRE: none | POST: none") {
    char buf[32];
    memset(buf, 0xFF, sizeof(buf));
    for (size_t i = 0; i < sizeof(buf); ++i)
        JARVIS_ASSERT(buf[i] == static_cast<char>(0xFF));
    memset(buf, 0, sizeof(buf));
    for (size_t i = 0; i < sizeof(buf); ++i)
        JARVIS_ASSERT(buf[i] == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Test memcmp with identical and differing buffers.
// Input: memcmp on "abc" vs "abc", "abc" vs "abd", "abd" vs "abc", "" vs "".
// Expect: 0 for matching buffers, negative/positive for lexicographic
// ordering, 0 for empty buffers.
// Depends: string
JARVIS_TEST(string_memcmp, "PRE: none | POST: none") {
    JARVIS_ASSERT_EQ(0, memcmp("abc", "abc", 3));
    JARVIS_ASSERT(memcmp("abc", "abd", 3) < 0);
    JARVIS_ASSERT(memcmp("abd", "abc", 3) > 0);
    JARVIS_ASSERT_EQ(0, memcmp("", "", 0));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Test align_up rounds values up to the next alignment boundary,
// including edge cases at boundaries and zero.
// Input: Values 0x1, 0x1000, 0x1001, 0xFFF, 0x3, 0x0 with alignments 0x1000
// or 0x4.
// Expect: Correctly rounded-up addresses.
// Depends: utils
JARVIS_TEST(utils_align_up, "PRE: none | POST: none") {
    JARVIS_ASSERT_HEX_EQ(0x1000, align_up(0x1, 0x1000));
    JARVIS_ASSERT_HEX_EQ(0x1000, align_up(0x1000, 0x1000));
    JARVIS_ASSERT_HEX_EQ(0x2000, align_up(0x1001, 0x1000));
    JARVIS_ASSERT_HEX_EQ(0x1000, align_up(0xFFF, 0x1000));
    JARVIS_ASSERT_HEX_EQ(0x4, align_up(0x3, 0x4));
    JARVIS_ASSERT_HEX_EQ(0x0, align_up(0x0, 0x4));
    JARVIS_ASSERT_HEX_EQ(0x1000, align_up(1, 0x1000));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Test align_down rounds values down to the previous alignment
// boundary, including edge cases.
// Input: Values 0x1, 0x1000, 0x1001, 0xFFF, 0x0, 0xFFF with alignments
// 0x1000 or 0x4.
// Expect: Correctly rounded-down addresses.
// Depends: utils
JARVIS_TEST(utils_align_down, "PRE: none | POST: none") {
    JARVIS_ASSERT_HEX_EQ(0x0000, align_down(0x1, 0x1000));
    JARVIS_ASSERT_HEX_EQ(0x1000, align_down(0x1000, 0x1000));
    JARVIS_ASSERT_HEX_EQ(0x1000, align_down(0x1001, 0x1000));
    JARVIS_ASSERT_HEX_EQ(0x0000, align_down(0xFFF, 0x1000));
    JARVIS_ASSERT_HEX_EQ(0x0000, align_down(0x0, 0x4));
    JARVIS_ASSERT_HEX_EQ(0x0FFC, align_down(0x0FFF, 0x4));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Test compile-time type traits is_same, is_integral, and is_pod
// for various types.
// Input: Type checks on int, unsigned int, uint64_t, double.
// Expect: Boolean trait values match expected type properties.
// Depends: utils
JARVIS_TEST(utils_type_traits, "PRE: none | POST: none") {
    JARVIS_ASSERT((is_same<int, int>::value));
    JARVIS_ASSERT((!is_same<int, unsigned int>::value));
    JARVIS_ASSERT((is_same<uint64_t, unsigned long long>::value));
    JARVIS_ASSERT((is_integral<int>::value));
    JARVIS_ASSERT((is_integral<uint64_t>::value));
    JARVIS_ASSERT((!is_integral<double>::value));
    JARVIS_ASSERT((is_pod<int>::value));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Test ErrorOr basic construction with values and errors,
// including the void specialization.
// Input: ErrorOr<int> constructed with value 42 or Error::OOM; ErrorOr<void>
// with success or Error::INVALID_ARG.
// Expect: ok() returns correct state, dereference returns stored value,
// error field matches ErrorOr<void> state.
// Depends: error
JARVIS_TEST(error_or_basic, "PRE: none | POST: none") {
    ErrorOr<int> e0(42);
    JARVIS_ASSERT(e0.ok());
    JARVIS_ASSERT_EQ(42, *e0);
    ErrorOr<int> e1(Error::OOM);
    JARVIS_ASSERT(!e1.ok());
    ErrorOr<void> v0;
    JARVIS_ASSERT(v0.ok());
    ErrorOr<void> v1(Error::INVALID_ARG);
    JARVIS_ASSERT(!v1.ok());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Test ErrorOr stores and reports different error codes correctly.
// Input: ErrorOr<int> constructed with Error::OOM, Error::INVALID_ARG,
// Error::NOT_FOUND.
// Expect: ok() returns false, error field matches the original error code.
// Depends: error
JARVIS_TEST(error_or_errors, "PRE: none | POST: none") {
    ErrorOr<int> e_oom(Error::OOM);
    JARVIS_ASSERT(!e_oom.ok());
    JARVIS_ASSERT(e_oom.error == Error::OOM);
    ErrorOr<int> e_inv(Error::INVALID_ARG);
    JARVIS_ASSERT(e_inv.error == Error::INVALID_ARG);
    ErrorOr<int> e_nf(Error::NOT_FOUND);
    JARVIS_ASSERT(e_nf.error == Error::NOT_FOUND);
    JARVIS_ASSERT(e_nf.ok() == false);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Test Version::string() returns a non-null, non-empty version
// identifier string.
// Input: None (reads global version state).
// Expect: Non-null pointer with strlen > 0.
// Depends: version
JARVIS_TEST(version_string_not_empty, "PRE: none | POST: none") {
    const char *sv = Version::string();
    JARVIS_ASSERT(sv != nullptr);
    JARVIS_ASSERT(strlen(sv) > 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Test Version::full_string() returns a non-null, non-empty full
// version string.
// Input: None (reads global version state).
// Expect: Non-null pointer with strlen > 0.
// Depends: version
JARVIS_TEST(version_full_string_not_empty, "PRE: none | POST: none") {
    const char *fv = Version::full_string();
    JARVIS_ASSERT(fv != nullptr);
    JARVIS_ASSERT(strlen(fv) > 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Test Version::build_date() returns a non-null, non-empty build
// date string.
// Input: None (reads global version state).
// Expect: Non-null pointer with strlen > 0.
// Depends: version
JARVIS_TEST(version_build_date_not_empty, "PRE: none | POST: none") {
    const char *bd = Version::build_date();
    JARVIS_ASSERT(bd != nullptr);
    JARVIS_ASSERT(strlen(bd) > 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: CRC32 matches the standard check vector — init builds the
//           table, update over "123456789" then finalize yields 0xCBF43926.
//           Re-init is idempotent (guarded) and reproduces the vector.
// Input: crc32("123456789"); init() twice.
// Expect: 0xCBF43926 both times (issue #126).
// Depends: kernel::CRC32
JARVIS_TEST(lib_crc32_known_vector, "PRE: none | POST: none") {
    CRC32::init();
    const uint8_t msg[] = {'1', '2', '3', '4', '5',
                           '6', '7', '8', '9'};
    const uint32_t first =
        CRC32::finalize(CRC32::update(CRC32::INITIAL, msg, sizeof(msg)));
    CRC32::init();
    const uint32_t second =
        CRC32::finalize(CRC32::update(CRC32::INITIAL, msg, sizeof(msg)));
    JARVIS_ASSERT_EQ(0xCBF43926UL, first);
    JARVIS_ASSERT_EQ(0xCBF43926UL, second);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Empty input finalizes to zero and split updates equal one-shot
//           updates (table-driven chaining is order-faithful).
// Input: finalize(INITIAL); update("12")+update("3456789") vs full update.
// Expect: Empty gives 0; incremental equals one-shot (issue #126).
// Depends: kernel::CRC32
JARVIS_TEST(lib_crc32_empty_and_incremental, "PRE: none | POST: none") {
    CRC32::init();
    JARVIS_ASSERT_EQ(0UL, CRC32::finalize(CRC32::INITIAL));
    const uint8_t msg[] = {'1', '2', '3', '4', '5',
                           '6', '7', '8', '9'};
    const uint32_t oneshot =
        CRC32::update(CRC32::INITIAL, msg, sizeof(msg));
    uint32_t chained = CRC32::update(CRC32::INITIAL, msg, 2);
    chained = CRC32::update(chained, msg + 2, sizeof(msg) - 2);
    JARVIS_ASSERT_EQ(oneshot, chained);
    JARVIS_ASSERT_EQ(0xCBF43926UL, CRC32::finalize(chained));
    JARVIS_TEST_PASS();
}

// Static device-tree blob for the lib_fdt_static_* tests (issue #183).
// The FDT library is reachable in production only via the AARCH64/RISCV64
// boot-DTB consumer, so x86 test boots can never exercise it live. These
// tests parse a hand-built blob instead, covering every public fdt_ro
// entry point without any boot/QEMU change.
//
// Blob layout (all integers big-endian):
//   header(40) + rsvmap(32: one entry + terminator) + struct(132) + strings(25)
// Struct block (offsets relative to its start):
//   root BEGIN@0 -> memory@8 (device_type + reg props) -> chosen@84
//   (bootargs prop) -> root END -> FDT_END.
// Strings: "device_type"@0, "reg"@12, "bootargs"@16.
namespace {

constexpr size_t kFdtStaticSize = 256U;

void fdt_static_put32(uint8_t *blob, size_t &pos, uint32_t val) {
    blob[pos] = static_cast<uint8_t>(val >> 24);
    blob[pos + 1] = static_cast<uint8_t>(val >> 16);
    blob[pos + 2] = static_cast<uint8_t>(val >> 8);
    blob[pos + 3] = static_cast<uint8_t>(val);
    pos += 4;
}

void fdt_static_put64(uint8_t *blob, size_t &pos, uint64_t val) {
    fdt_static_put32(blob, pos, static_cast<uint32_t>(val >> 32));
    fdt_static_put32(blob, pos, static_cast<uint32_t>(val));
}

void fdt_static_put_str(uint8_t *blob, size_t &pos, const char *str) {
    while (*str) {
        blob[pos++] = static_cast<uint8_t>(*str++);
    }
    blob[pos++] = 0;
    while (pos % 4 != 0) {
        blob[pos++] = 0;
    }
}

void fdt_static_put_prop(uint8_t *blob, size_t &pos, uint32_t nameoff,
                         const uint8_t *data, size_t len) {
    fdt_static_put32(blob, pos, FDT_PROP);
    fdt_static_put32(blob, pos, static_cast<uint32_t>(len));
    fdt_static_put32(blob, pos, nameoff);
    for (size_t i = 0; i < len; ++i) {
        blob[pos++] = data[i];
    }
    while (pos % 4 != 0) {
        blob[pos++] = 0;
    }
}

// Builds the blob into a static buffer, recording the real struct-relative
// offsets so tests never hardcode layout arithmetic.
struct FdtStaticLayout {
    size_t total;
    int mem_node;
    int chosen_node;
    int reg_prop;
};

size_t fdt_static_build(uint8_t *blob, FdtStaticLayout &layout) {
    for (size_t i = 0; i < kFdtStaticSize; ++i) {
        blob[i] = 0;
    }
    // Header with placeholder offsets/sizes; patched at the end.
    size_t pos = 0;
    fdt_static_put32(blob, pos, FDT_MAGIC);
    size_t totalsize_at = pos;
    fdt_static_put32(blob, pos, 0);
    size_t struct_at = pos;
    fdt_static_put32(blob, pos, 0);
    size_t strings_at = pos;
    fdt_static_put32(blob, pos, 0);
    fdt_static_put32(blob, pos, 40);  // off_mem_rsvmap
    fdt_static_put32(blob, pos, 17);  // version
    fdt_static_put32(blob, pos, 16);  // last_comp_version
    fdt_static_put32(blob, pos, 0);   // boot_cpuid_phys
    size_t strsize_at = pos;
    fdt_static_put32(blob, pos, 0);
    size_t structsize_at = pos;
    fdt_static_put32(blob, pos, 0);
    // Reserve map: one entry + terminator.
    fdt_static_put64(blob, pos, 0x1000);
    fdt_static_put64(blob, pos, 0x100);
    fdt_static_put64(blob, pos, 0);
    fdt_static_put64(blob, pos, 0);
    // Struct block: root with memory + chosen children.
    size_t struct_base = pos;
    auto rel = [&pos, struct_base]() -> int {
        return static_cast<int>(pos - struct_base);
    };
    auto patch32 = [&blob](size_t at, uint32_t val) {
        blob[at] = static_cast<uint8_t>(val >> 24);
        blob[at + 1] = static_cast<uint8_t>(val >> 16);
        blob[at + 2] = static_cast<uint8_t>(val >> 8);
        blob[at + 3] = static_cast<uint8_t>(val);
    };
    fdt_static_put32(blob, pos, FDT_BEGIN_NODE);
    fdt_static_put_str(blob, pos, "");
    layout.mem_node = rel();
    fdt_static_put32(blob, pos, FDT_BEGIN_NODE);
    fdt_static_put_str(blob, pos, "memory@40000000");
    const uint8_t kDeviceType[] = {'m', 'e', 'm', 'o', 'r', 'y', 0};
    fdt_static_put_prop(blob, pos, 0, kDeviceType, sizeof(kDeviceType));
    layout.reg_prop = rel();
    uint8_t reg[16] = {0, 0, 0, 0, 0x40, 0, 0, 0,
                       0, 0, 0, 0, 0x10, 0, 0, 0};
    fdt_static_put_prop(blob, pos, 12, reg, sizeof(reg));
    fdt_static_put32(blob, pos, FDT_END_NODE);
    layout.chosen_node = rel();
    fdt_static_put32(blob, pos, FDT_BEGIN_NODE);
    fdt_static_put_str(blob, pos, "chosen");
    const uint8_t kBootargs[] = {'t', 'e', 's', 't', '-', 'b',
                                 'o', 'o', 't', 0};
    fdt_static_put_prop(blob, pos, 16, kBootargs, sizeof(kBootargs));
    fdt_static_put32(blob, pos, FDT_END_NODE);
    fdt_static_put32(blob, pos, FDT_END_NODE);
    fdt_static_put32(blob, pos, FDT_END);
    size_t struct_size = pos - struct_base;
    // Strings block.
    size_t strings_base = pos;
    fdt_static_put_str(blob, pos, "device_type");
    fdt_static_put_str(blob, pos, "reg");
    fdt_static_put_str(blob, pos, "bootargs");
    // Patch header fields (big-endian).
    patch32(totalsize_at, static_cast<uint32_t>(pos));
    patch32(struct_at, static_cast<uint32_t>(struct_base));
    patch32(strings_at, static_cast<uint32_t>(strings_base));
    patch32(strsize_at, static_cast<uint32_t>(pos - strings_base));
    patch32(structsize_at, static_cast<uint32_t>(struct_size));
    layout.total = pos;
    return pos;
}

}  // namespace

// Runmode: kernel
// Testidea: A hand-built DTB passes header validation and exposes its
// reserve-map entry (issue #183: boot-independent fdt_* measurement).
// Input: static blob (magic 0xD00DFEED, v17, one reserve entry).
// Expect: fdt_check_header == 0, one entry at 0x1000 size 0x100.
// Depends: src/lib/fdt/fdt_ro.cpp
JARVIS_TEST(lib_fdt_static_header_and_reserve, "PRE: none | POST: none") {
    static uint8_t blob[kFdtStaticSize];
    FdtStaticLayout layout{};
    size_t total = fdt_static_build(blob, layout);
    JARVIS_ASSERT_EQ(0, fdt_check_header(blob));
    JARVIS_ASSERT_EQ(1, fdt_num_mem_rsv(blob));
    uint64_t addr = 0;
    uint64_t size = 0;
    JARVIS_ASSERT_EQ(0, fdt_get_mem_rsv(blob, 0, &addr, &size));
    JARVIS_ASSERT_EQ(0x1000ULL, addr);
    JARVIS_ASSERT_EQ(0x100ULL, size);
    JARVIS_ASSERT(total < kFdtStaticSize);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Walk the static blob through every public node/property query
// (issue #183).
// Input: static blob with memory@40000000 (device_type + reg) and chosen
// (bootargs) children.
// Expect: prop-value search reports the outermost match (root, 0); child,
// sibling, name, subnode and property queries resolve to the builder-recorded
// layout; reg decodes to base 0x40000000 size 0x10000000; bootargs reads back
// "test-boot".
// Depends: src/lib/fdt/fdt_ro.cpp
JARVIS_TEST(lib_fdt_static_tree_walk, "PRE: none | POST: none") {
    static uint8_t blob[kFdtStaticSize];
    FdtStaticLayout layout{};
    fdt_static_build(blob, layout);

    int mem =
        fdt_node_offset_by_prop_value(blob, -1, "device_type", "memory", 7);
    // Returns the OUTERMOST node whose content scan hits the property: the
    // root (offset 0) has no direct props, so the scan starting at its
    // content finds memory's device_type first and reports the root.
    JARVIS_ASSERT_EQ(0, mem);
    JARVIS_ASSERT_EQ(layout.mem_node, fdt_first_child(blob, 0));
    JARVIS_ASSERT_EQ(layout.chosen_node, fdt_next_sibling(blob, layout.mem_node));
    JARVIS_ASSERT_EQ(layout.chosen_node, fdt_next_subnode(blob, layout.mem_node));
    // Last child and root have no next sibling (fail-closed NOTFOUND, not a
    // bogus offset — issue #183 found the old depth-0 walk returned the
    // parent's end instead).
    JARVIS_ASSERT(fdt_next_sibling(blob, layout.chosen_node) < 0);
    JARVIS_ASSERT(fdt_next_sibling(blob, 0) < 0);

    int name_len = 0;
    JARVIS_ASSERT_EQ(0, strcmp(fdt_get_name(blob, layout.mem_node, &name_len),
                               "memory@40000000"));
    JARVIS_ASSERT_EQ(15, name_len);
    JARVIS_ASSERT_EQ(0, strcmp(fdt_get_name(blob, layout.chosen_node, nullptr),
                               "chosen"));

    JARVIS_ASSERT_EQ(layout.mem_node, fdt_subnode_offset_namelen(
                                          blob, 0, "memory@40000000", 15));
    JARVIS_ASSERT_EQ(layout.chosen_node,
                     fdt_subnode_offset_namelen(blob, 0, "chosen", 6));

    int reg_len = 0;
    const auto *reg = static_cast<const uint32_t *>(
        fdt_getprop_namelen(blob, layout.mem_node, "reg", &reg_len));
    JARVIS_ASSERT(reg != nullptr);
    JARVIS_ASSERT_EQ(16, reg_len);
    uint64_t base =
        (static_cast<uint64_t>(fdt32_to_cpu(reg[0])) << 32) |
        fdt32_to_cpu(reg[1]);
    uint64_t size =
        (static_cast<uint64_t>(fdt32_to_cpu(reg[2])) << 32) |
        fdt32_to_cpu(reg[3]);
    JARVIS_ASSERT_EQ(0x40000000ULL, base);
    JARVIS_ASSERT_EQ(0x10000000ULL, size);

    const char *outname = nullptr;
    int by_off_len = 0;
    const auto *by_off = static_cast<const uint32_t *>(
        fdt_getprop_by_offset(blob, layout.reg_prop, &outname, &by_off_len));
    JARVIS_ASSERT(by_off != nullptr);
    JARVIS_ASSERT_EQ(0, strcmp(outname, "reg"));
    JARVIS_ASSERT_EQ(16, by_off_len);

    int args_len = 0;
    const auto *args = static_cast<const char *>(
        fdt_getprop_namelen(blob, layout.chosen_node, "bootargs", &args_len));
    JARVIS_ASSERT(args != nullptr);
    JARVIS_ASSERT_EQ(10, args_len);
    JARVIS_ASSERT_EQ(0, strcmp(args, "test-boot"));

    JARVIS_ASSERT_EQ(0, strcmp(fdt_string(blob, 0), "device_type"));
    JARVIS_ASSERT_EQ(0, strcmp(fdt_string(blob, 12), "reg"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A corrupted magic fail-closes with BADMAGIC and every query
// rejects the blob (issue #183).
// Input: static blob with magic zeroed.
// Expect: fdt_check_header == -FDT_ERR_BADMAGIC; node/prop lookups fail;
// fdt_strerror describes the code.
// Depends: src/lib/fdt/fdt_ro.cpp
JARVIS_TEST(lib_fdt_static_bad_header, "PRE: none | POST: none") {
    static uint8_t blob[kFdtStaticSize];
    FdtStaticLayout bad_layout{};
    fdt_static_build(blob, bad_layout);
    blob[0] = 0;
    blob[1] = 0;
    blob[2] = 0;
    blob[3] = 0;
    JARVIS_ASSERT_EQ(-FDT_ERR_BADMAGIC, fdt_check_header(blob));
    JARVIS_ASSERT(fdt_node_offset_by_prop_value(blob, -1, "device_type",
                                                "memory", 7) < 0);
    JARVIS_ASSERT(fdt_subnode_offset_namelen(blob, 0, "chosen", 6) < 0);
    JARVIS_ASSERT(fdt_getprop_namelen(blob, bad_layout.mem_node, "reg",
                                        nullptr) == nullptr);
    JARVIS_ASSERT(fdt_string(blob, 0) == nullptr);
    JARVIS_ASSERT(fdt_strerror(FDT_ERR_BADMAGIC) != nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Register all lib subsystem tests (string, utils, ErrorOr,
// version) with the test framework.
// Depends: string, utils, error, version
void register_lib_tests() {
    Logger::info("Registering lib tests");

    JARVIS_REGISTER_RELEASE_TEST(string_strlen);
    JARVIS_REGISTER_RELEASE_TEST(string_strcmp);
    JARVIS_REGISTER_RELEASE_TEST(string_strncmp);
    JARVIS_REGISTER_RELEASE_TEST(string_strncpy);
    JARVIS_REGISTER_RELEASE_TEST(string_memcpy);
    JARVIS_REGISTER_RELEASE_TEST(string_memset);
    JARVIS_REGISTER_RELEASE_TEST(string_memcmp);

    JARVIS_REGISTER_RELEASE_TEST(utils_align_up);
    JARVIS_REGISTER_RELEASE_TEST(utils_align_down);
    JARVIS_REGISTER_RELEASE_TEST(utils_type_traits);

    JARVIS_REGISTER_RELEASE_TEST(error_or_basic);
    JARVIS_REGISTER_RELEASE_TEST(error_or_errors);

    JARVIS_REGISTER_RELEASE_TEST(version_string_not_empty);
    JARVIS_REGISTER_RELEASE_TEST(version_full_string_not_empty);
    JARVIS_REGISTER_RELEASE_TEST(version_build_date_not_empty);

    JARVIS_REGISTER_TEST(lib_crc32_known_vector);
    JARVIS_REGISTER_TEST(lib_crc32_empty_and_incremental);

    JARVIS_REGISTER_TEST(lib_fdt_static_header_and_reserve);
    JARVIS_REGISTER_TEST(lib_fdt_static_tree_walk);
    JARVIS_REGISTER_TEST(lib_fdt_static_bad_header);
}
