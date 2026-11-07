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

/// @file test_fstab.cpp
/// @brief Filesystem table (fstab) tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/vfs.hpp>
#include <string.hpp>

using namespace kernel;

// Replicate the init fstab parsing logic (from kernel.cpp) for unit testing
static int parse_fstab_line(const char *p, const char *end, char *fs_name_out,
                            size_t fs_max, char *mp_out, size_t mp_max) {
    while (p < end && (*p == ' ' || *p == '\t'))
        ++p;
    if (p >= end || *p == '#' || *p == '\n')
        return -1;

    size_t n = 0;
    while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && n < fs_max - 1)
        fs_name_out[n++] = *p++;
    fs_name_out[n] = '\0';

    while (p < end && (*p == ' ' || *p == '\t'))
        ++p;

    n = 0;
    while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && n < mp_max - 1)
        mp_out[n++] = *p++;
    mp_out[n] = '\0';

    if (!fs_name_out[0] || !mp_out[0])
        return -1;
    return 0;
}

JARVIS_TEST(fstab_parses_valid_line, "PRE: none | POST: none") {
    const char input[] = "tmpfs /tmp\n";
    char fs[32], mp[64];
    int ret = parse_fstab_line(input, input + sizeof(input) - 1, fs, sizeof(fs),
                               mp, sizeof(mp));
    JARVIS_ASSERT_EQ(0, ret);
    JARVIS_ASSERT_EQ(0, strcmp(fs, "tmpfs"));
    JARVIS_ASSERT_EQ(0, strcmp(mp, "/tmp"));
    JARVIS_TEST_PASS();
}

JARVIS_TEST(fstab_parses_with_extra_spaces, "PRE: none | POST: none") {
    const char input[] = "  tmpfs   /mnt/extra   \n";
    char fs[32], mp[64];
    int ret = parse_fstab_line(input, input + sizeof(input) - 1, fs, sizeof(fs),
                               mp, sizeof(mp));
    JARVIS_ASSERT_EQ(0, ret);
    JARVIS_ASSERT_EQ(0, strcmp(fs, "tmpfs"));
    JARVIS_ASSERT_EQ(0, strcmp(mp, "/mnt/extra"));
    JARVIS_TEST_PASS();
}

JARVIS_TEST(fstab_skips_comment_line, "PRE: none | POST: none") {
    const char input[] = "# this is a comment\n";
    char fs[32], mp[64];
    int ret = parse_fstab_line(input, input + sizeof(input) - 1, fs, sizeof(fs),
                               mp, sizeof(mp));
    JARVIS_ASSERT_EQ(-1, ret);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(fstab_skips_empty_line, "PRE: none | POST: none") {
    const char input[] = "\n";
    char fs[32], mp[64];
    int ret = parse_fstab_line(input, input + sizeof(input) - 1, fs, sizeof(fs),
                               mp, sizeof(mp));
    JARVIS_ASSERT_EQ(-1, ret);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(fstab_rejects_missing_mountpoint, "PRE: none | POST: none") {
    const char input[] = "tmpfs\n";
    char fs[32], mp[64];
    int ret = parse_fstab_line(input, input + sizeof(input) - 1, fs, sizeof(fs),
                               mp, sizeof(mp));
    JARVIS_ASSERT_EQ(-1, ret);
    JARVIS_TEST_PASS();
}

void register_fstab_tests() {
    kernel::Logger::info("Registering fstab tests");
    JARVIS_REGISTER_TEST(fstab_parses_valid_line);
    JARVIS_REGISTER_TEST(fstab_parses_with_extra_spaces);
    JARVIS_REGISTER_TEST(fstab_skips_comment_line);
    JARVIS_REGISTER_TEST(fstab_skips_empty_line);
    JARVIS_REGISTER_TEST(fstab_rejects_missing_mountpoint);
}
