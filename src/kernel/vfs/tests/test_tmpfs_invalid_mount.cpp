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

/// @file test_tmpfs_invalid_mount.cpp
/// @brief TMPFS invalid mount handling tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/tmpfs.hpp>
#include <kernel/vfs/vfs.hpp>
#include <string.hpp>

using namespace kernel;
using namespace kernel::vfs;

JARVIS_TEST(tmpfs_filesystem_properties, "PRE: vfsd, iocd | POST: none") {
    JARVIS_ASSERT(tmpfs_fs.get_root != nullptr);
    JARVIS_ASSERT(tmpfs_fs.name != nullptr);
    JARVIS_ASSERT_EQ(0, strcmp(tmpfs_fs.name, "tmpfs"));

    Vnode *root = tmpfs_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    JARVIS_ASSERT(root->mode & S_IFDIR);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(tmpfs_mount_at_invalid_resolve_fails,
            "PRE: vfsd, iocd | POST: none") {
    Vnode *vn = vfs::resolve("/nonexistent_tmpfs_mount");
    JARVIS_ASSERT_EQ(nullptr, vn);
    JARVIS_TEST_PASS();
}

void register_tmpfs_invalid_mount_tests() {
    kernel::Logger::info("Registering tmpfs invalid mount tests");
    JARVIS_REGISTER_TEST(tmpfs_filesystem_properties);
    JARVIS_REGISTER_TEST(tmpfs_mount_at_invalid_resolve_fails);
}
