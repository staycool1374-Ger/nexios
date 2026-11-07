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

/// @file test_tmpfs_mount_unmount_failure.cpp
/// @brief TMPFS mount/unmount failure tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/tmpfs.hpp>
#include <kernel/vfs/vfs.hpp>

using namespace kernel;
using namespace kernel::vfs;

JARVIS_TEST(tmpfs_root_ops_consistent, "PRE: vfsd, iocd | POST: none") {
    Vnode *root = tmpfs_fs.get_root();
    JARVIS_ASSERT(root != nullptr);

    // Root should have directory ops
    JARVIS_ASSERT(root->ops != nullptr);
    JARVIS_ASSERT(root->ops->mkdir != nullptr);
    JARVIS_ASSERT(root->ops->lookup != nullptr);
    JARVIS_ASSERT(root->ops->readdir != nullptr);
    JARVIS_ASSERT(root->ops->unlink != nullptr);
    JARVIS_ASSERT(root->ops->fstat != nullptr);

    // Root should not have file-only ops
    JARVIS_ASSERT_EQ(nullptr, root->ops->read);
    JARVIS_ASSERT_EQ(nullptr, root->ops->write);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(tmpfs_unmount_unsupported, "PRE: vfsd, iocd | POST: none") {
    // Currently VFS has no unmount API — verify mount succeeds as expected
    int ret = vfs::mount(tmpfs_fs, "/tmp_nounmount_test");
    JARVIS_ASSERT_EQ(0, ret);

    Vnode *root = vfs::resolve("/tmp_nounmount_test");
    JARVIS_ASSERT(root != nullptr);
    JARVIS_ASSERT(root->mode & S_IFDIR);
    JARVIS_TEST_PASS();
}

void register_tmpfs_mount_unmount_failure_tests() {
    kernel::Logger::info("Registering tmpfs mount/unmount failure tests");
    JARVIS_REGISTER_TEST(tmpfs_root_ops_consistent);
    JARVIS_REGISTER_TEST(tmpfs_unmount_unsupported);
}
