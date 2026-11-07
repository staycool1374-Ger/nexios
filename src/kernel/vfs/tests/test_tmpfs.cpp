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

/// @file test_tmpfs.cpp
/// @brief TMPFS (temporary in-memory filesystem) tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/tmpfs.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/test/test_isolate.hpp>
#include <string.hpp>

using namespace kernel;
using namespace kernel::vfs;

JARVIS_TEST(tmpfs_mount, "PRE: vfsd, iocd | POST: none") {
    int ret = vfs::mount(tmpfs_fs, "/tmp_mount_test");
    JARVIS_ASSERT_EQ(0, ret);

    Vnode *root = vfs::resolve("/tmp_mount_test");
    JARVIS_ASSERT(root != nullptr);
    JARVIS_ASSERT(root->mode & S_IFDIR);

    JARVIS_TEST_PASS();
}

JARVIS_TEST(tmpfs_mkdir_ls, "PRE: vfsd, iocd | POST: none") {
    int ret = vfs::mount(tmpfs_fs, "/tmp_mkdir_test");
    JARVIS_ASSERT_EQ(0, ret);

    Vnode *root = vfs::resolve("/tmp_mkdir_test");
    JARVIS_ASSERT(root != nullptr);

    ret = vfs::mkdir("/tmp_mkdir_test/subdir", 0);
    JARVIS_ASSERT_EQ(0, ret);

    Vnode *sub = vfs::resolve("/tmp_mkdir_test/subdir");
    JARVIS_ASSERT(sub != nullptr);
    JARVIS_ASSERT(sub->mode & S_IFDIR);

    // Readdir should show "subdir"
    uint64_t pos = 0;
    Dirent dent;
    bool found = false;
    while (root->ops->readdir(*root, pos, dent) == 0) {
        if (strcmp(dent.d_name, "subdir") == 0) {
            found = true;
            break;
        }
    }
    JARVIS_ASSERT(found);

    ret = vfs::unlink("/tmp_mkdir_test/subdir");
    JARVIS_ASSERT_EQ(0, ret);

    JARVIS_TEST_PASS();
}

JARVIS_TEST(tmpfs_create_file, "PRE: vfsd, iocd | POST: none") {
    int ret = vfs::mount(tmpfs_fs, "/tmp_file_test");
    JARVIS_ASSERT_EQ(0, ret);

    ret = vfs::create("/tmp_file_test/test.txt", 0);
    JARVIS_ASSERT_EQ(0, ret);

    Vnode *file = vfs::resolve("/tmp_file_test/test.txt");
    JARVIS_ASSERT(file != nullptr);
    JARVIS_ASSERT(file->mode & S_IFREG);

    // Write data
    const char *data = "Hello tmpfs!";
    uint64_t len = 13;
    int64_t written = file->ops->write(
        *file, reinterpret_cast<const uint8_t *>(data), len, 0);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(len), written);
    JARVIS_ASSERT_EQ(len, file->size);

    // Read back
    uint8_t buf[64];
    __builtin_memset(buf, 0, sizeof(buf));
    int64_t read = file->ops->read(*file, buf, sizeof(buf) - 1, 0);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(len), read);
    JARVIS_ASSERT_EQ(0, strcmp(data, reinterpret_cast<const char *>(buf)));

    // fstat
    VfsStat st;
    ret = file->ops->fstat(*file, st);
    JARVIS_ASSERT_EQ(0, ret);
    JARVIS_ASSERT_EQ(len, st.st_size);
    JARVIS_ASSERT(st.st_mode & S_IFREG);

    ret = vfs::unlink("/tmp_file_test/test.txt");
    JARVIS_ASSERT_EQ(0, ret);

    JARVIS_TEST_PASS();
}

JARVIS_TEST(tmpfs_unlink_file, "PRE: vfsd, iocd | POST: none") {
    int ret = vfs::mount(tmpfs_fs, "/tmp_unlink_test");
    JARVIS_ASSERT_EQ(0, ret);

    ret = vfs::create("/tmp_unlink_test/delfile.txt", 0);
    JARVIS_ASSERT_EQ(0, ret);

    Vnode *file = vfs::resolve("/tmp_unlink_test/delfile.txt");
    JARVIS_ASSERT(file != nullptr);

    ret = vfs::unlink("/tmp_unlink_test/delfile.txt");
    JARVIS_ASSERT_EQ(0, ret);

    file = vfs::resolve("/tmp_unlink_test/delfile.txt");
    JARVIS_ASSERT_EQ(nullptr, file);

    JARVIS_TEST_PASS();
}

JARVIS_TEST(tmpfs_unlink_dir, "PRE: vfsd, iocd | POST: none") {
    int ret = vfs::mount(tmpfs_fs, "/tmp_unlinkdir_test");
    JARVIS_ASSERT_EQ(0, ret);

    ret = vfs::mkdir("/tmp_unlinkdir_test/subdir", 0);
    JARVIS_ASSERT_EQ(0, ret);

    ret = vfs::unlink("/tmp_unlinkdir_test/subdir");
    JARVIS_ASSERT_EQ(0, ret);

    Vnode *sub = vfs::resolve("/tmp_unlinkdir_test/subdir");
    JARVIS_ASSERT_EQ(nullptr, sub);

    JARVIS_TEST_PASS();
}

JARVIS_TEST(tmpfs_nonempty_dir_unlink_fails, "PRE: vfsd, iocd | POST: none") {
    int ret = vfs::mount(tmpfs_fs, "/tmp_nonempty_test");
    JARVIS_ASSERT_EQ(0, ret);

    ret = vfs::mkdir("/tmp_nonempty_test/parent", 0);
    JARVIS_ASSERT_EQ(0, ret);

    ret = vfs::mkdir("/tmp_nonempty_test/parent/child", 0);
    JARVIS_ASSERT_EQ(0, ret);

    ret = vfs::unlink("/tmp_nonempty_test/parent");
    JARVIS_ASSERT_EQ(VFS_INVALID, ret);

    ret = vfs::unlink("/tmp_nonempty_test/parent/child");
    JARVIS_ASSERT_EQ(0, ret);
    ret = vfs::unlink("/tmp_nonempty_test/parent");
    JARVIS_ASSERT_EQ(0, ret);

    JARVIS_TEST_PASS();
}

void register_tmpfs_tests() {
    kernel::Logger::info("Registering tmpfs tests");
    JARVIS_REGISTER_TEST(tmpfs_mount);
    JARVIS_REGISTER_TEST(tmpfs_mkdir_ls);
    JARVIS_REGISTER_TEST(tmpfs_create_file);
    JARVIS_REGISTER_TEST(tmpfs_unlink_file);
    JARVIS_REGISTER_TEST(tmpfs_unlink_dir);
    JARVIS_REGISTER_TEST(tmpfs_nonempty_dir_unlink_fails);
}
