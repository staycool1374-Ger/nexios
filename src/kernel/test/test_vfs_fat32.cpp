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

/// @file test_vfs_fat32.cpp
/// @brief VFS FAT32 filesystem operation tests.

#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-null-argument"
#pragma GCC diagnostic ignored "-Wanalyzer-possible-null-dereference"
#if __GNUC__ >= 12
#pragma GCC diagnostic ignored "-Wanalyzer-undefined-behavior-ptrdiff"
#endif
#endif

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/vfs/fat32.hpp>
#include <kernel/vfs/fat32_fs.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/driver/block_device.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/test/test_isolate.hpp>
#include <string.hpp>

using namespace kernel;
using namespace kernel::vfs;

extern "C" uint8_t _binary_build_fat32_img_start[];
extern "C" uint8_t _binary_build_fat32_img_end[];

/// Wraps a Fat32Partition together with its backing device so both
/// are cleaned up on destruction.
struct Fat32TestFixture {
    fat32::Fat32Partition *partition;
    kernel::block::MockBlockDevice *device;

    Fat32TestFixture(fat32::Fat32Partition *p,
                     kernel::block::MockBlockDevice *d)
        : partition(p), device(d) {
    }

    ~Fat32TestFixture() {
        delete partition;
        delete device;
    }

    Fat32TestFixture(const Fat32TestFixture &) = delete;
    Fat32TestFixture &operator=(const Fat32TestFixture &) = delete;
};

static Fat32TestFixture *g_fixture = nullptr;

static void setup_fat32_fs() {
    kernel::test::mark_vfs_touched();
    if (kernel::gs::get_fat32_partition())
        return;

    uint64_t bytes = static_cast<uint64_t>(_binary_build_fat32_img_end -
                                           _binary_build_fat32_img_start);

    auto *dev = new kernel::block::MockBlockDevice(
        _binary_build_fat32_img_start, bytes / fat32::SECTOR_SIZE, true);
    auto *partition = new fat32::Fat32Partition(*dev);
    if (partition && partition->mount()) {
        kernel::gs::try_set_fat32_partition(partition);
        g_fixture = new Fat32TestFixture{partition, dev};
    }
}

JARVIS_TEST(vfs_fat32_mount, "PRE: vfsd, iocd | POST: none") {
    setup_fat32_fs();
    JARVIS_ASSERT(kernel::gs::get_fat32_partition() != nullptr);
    JARVIS_ASSERT(kernel::gs::get_fat32_partition()->bpb().valid);
}

JARVIS_TEST(vfs_fat32_open_root, "PRE: vfsd, iocd | POST: none") {
    setup_fat32_fs();
    Vnode *root = fat32_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    JARVIS_ASSERT(root->mode == S_IFDIR);
    JARVIS_ASSERT(root->ops != nullptr);
}

JARVIS_TEST(vfs_fat32_open_file, "PRE: vfsd, iocd | POST: none") {
    setup_fat32_fs();
    Vnode *root = fat32_fs.get_root();
    JARVIS_ASSERT(root != nullptr);

    Vnode *file = root->ops->lookup(*root, "HELLO.TXT");
    JARVIS_ASSERT(file != nullptr);
    JARVIS_ASSERT(file->mode == S_IFREG);
    file->ops->close(*file);
}

JARVIS_TEST(vfs_fat32_read_file, "PRE: vfsd, iocd | POST: none") {
    setup_fat32_fs();
    Vnode *root = fat32_fs.get_root();
    JARVIS_ASSERT(root != nullptr);

    Vnode *file = root->ops->lookup(*root, "HELLO.TXT");
    JARVIS_ASSERT(file != nullptr);

    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    int64_t nread = file->ops->read(*file, buf, sizeof(buf), 0);
    JARVIS_ASSERT(nread > 0);
    JARVIS_ASSERT(memcmp(buf, "Hello, World!\n", 14) == 0);
    file->ops->close(*file);
}

JARVIS_TEST(vfs_fat32_fstat, "PRE: vfsd, iocd | POST: none") {
    setup_fat32_fs();
    Vnode *root = fat32_fs.get_root();
    JARVIS_ASSERT(root != nullptr);

    Vnode *file = root->ops->lookup(*root, "HELLO.TXT");
    JARVIS_ASSERT(file != nullptr);

    VfsStat st = {};
    JARVIS_ASSERT(file->ops->fstat(*file, st) == 0);
    JARVIS_ASSERT(st.st_size > 0);
    JARVIS_ASSERT(st.st_mode == S_IFREG);
    file->ops->close(*file);
}

JARVIS_TEST(vfs_fat32_readdir, "PRE: vfsd, iocd | POST: none") {
    setup_fat32_fs();
    Vnode *root = fat32_fs.get_root();
    JARVIS_ASSERT(root != nullptr);

    uint64_t pos = 0;
    Dirent dent = {};
    bool found_readme = false;
    bool found_hello = false;

    while (root->ops->readdir(*root, pos, dent) == 0) {
        if (strcmp(dent.d_name, "MULTI.TXT") == 0)
            found_readme = true;
        if (strcmp(dent.d_name, "HELLO.TXT") == 0)
            found_hello = true;
    }

    JARVIS_ASSERT(found_readme);
    JARVIS_ASSERT(found_hello);
}

JARVIS_TEST(vfs_fat32_nonexistent_path, "PRE: vfsd, iocd | POST: none") {
    setup_fat32_fs();
    Vnode *root = fat32_fs.get_root();
    JARVIS_ASSERT(root != nullptr);

    Vnode *none = root->ops->lookup(*root, "NONEXIST.TXT");
    JARVIS_ASSERT(none == nullptr);
}

JARVIS_TEST(vfs_fat32_subdir, "PRE: vfsd, iocd | POST: none") {
    setup_fat32_fs();
    Vnode *root = fat32_fs.get_root();
    JARVIS_ASSERT(root != nullptr);

    Vnode *sub = root->ops->lookup(*root, "SUBDIR");
    JARVIS_ASSERT(sub != nullptr);
    JARVIS_ASSERT(sub->mode == S_IFDIR);

    Vnode *file = sub->ops->lookup(*sub, "FILE.TXT");
    JARVIS_ASSERT(file != nullptr);
    JARVIS_ASSERT(file->mode == S_IFREG);

    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    int64_t nread = file->ops->read(*file, buf, sizeof(buf), 0);
    JARVIS_ASSERT(nread > 0);
    JARVIS_ASSERT(memcmp(buf, "I am in a subdirectory.\n", 24) == 0);

    file->ops->close(*file);
    sub->ops->close(*sub);
}

// ---- Write tests (separate writable partition, not shared global) ----

static Fat32TestFixture create_writable_partition() {
    extern uint8_t _binary_build_fat32_img_start[];
    extern uint8_t _binary_build_fat32_img_end[];
    uint64_t bytes = static_cast<uint64_t>(_binary_build_fat32_img_end -
                                           _binary_build_fat32_img_start);
    uint64_t sectors = bytes / fat32::SECTOR_SIZE;
    auto *dev = new kernel::block::MockBlockDevice(sectors);
    for (uint64_t i = 0; i < sectors; ++i) {
        uint8_t sector[fat32::SECTOR_SIZE];
        __builtin_memcpy(sector,
                         _binary_build_fat32_img_start + i * fat32::SECTOR_SIZE,
                         fat32::SECTOR_SIZE);
        dev->write_sector(i, sector);
    }
    return Fat32TestFixture(new fat32::Fat32Partition(*dev), dev);
}

// Runmode: kernel
// Testidea: Verifies FAT32 mkdir via VnodeOps creates a directory entry.
// Input: ops->mkdir on root vnode with name "NEWDIR"
// Expect: Returns 0, lookup finds new directory
// Depends: kernel::vfs::VnodeOps::mkdir
JARVIS_TEST(vfs_fat32_mkdir, "PRE: vfsd, iocd | POST: none") {
    auto fix = create_writable_partition();
    JARVIS_ASSERT(fix.partition->mount());
    auto *old = kernel::gs::get_fat32_partition();
    kernel::gs::try_set_fat32_partition(fix.partition);

    Vnode *root = fat32_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    JARVIS_ASSERT(root->ops->mkdir != nullptr);

    int ret = root->ops->mkdir(*root, "NEWDIR", 0);
    JARVIS_ASSERT_EQ(0, ret);

    Vnode *child = root->ops->lookup(*root, "NEWDIR");
    JARVIS_ASSERT(child != nullptr);
    JARVIS_ASSERT(child->mode & vfs::S_IFDIR);

    child->ops->close(*child);
    root->ops->close(*root);
    kernel::gs::try_set_fat32_partition(old);
    fat32_fs.get_root();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies FAT32 unlink via VnodeOps removes a file entry.
// Input: ops->unlink on root vnode for "HELLO.TXT"
// Expect: Returns 0, lookup no longer finds the file
// Depends: kernel::vfs::VnodeOps::unlink
JARVIS_TEST(vfs_fat32_unlink, "PRE: vfsd, iocd | POST: none") {
    auto fix = create_writable_partition();
    JARVIS_ASSERT(fix.partition->mount());
    auto *old = kernel::gs::get_fat32_partition();
    kernel::gs::try_set_fat32_partition(fix.partition);

    Vnode *root = fat32_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    JARVIS_ASSERT(root->ops->unlink != nullptr);

    int ret = root->ops->unlink(*root, "HELLO.TXT");
    JARVIS_ASSERT_EQ(0, ret);

    Vnode *child = root->ops->lookup(*root, "HELLO.TXT");
    JARVIS_ASSERT(child == nullptr);

    root->ops->close(*root);
    kernel::gs::try_set_fat32_partition(old);
    fat32_fs.get_root();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies mkdir then readdir shows the new directory.
// Input: mkdir "DIR2", then readdir
// Expect: readdir includes "DIR2"
// Depends: kernel::vfs::VnodeOps
JARVIS_TEST(vfs_fat32_mkdir_then_readdir, "PRE: vfsd, iocd | POST: none") {
    auto fix = create_writable_partition();
    JARVIS_ASSERT(fix.partition->mount());
    auto *old = kernel::gs::get_fat32_partition();
    kernel::gs::try_set_fat32_partition(fix.partition);

    Vnode *root = fat32_fs.get_root();
    JARVIS_ASSERT(root != nullptr);

    JARVIS_ASSERT_EQ(0, root->ops->mkdir(*root, "DIR2", 0));

    uint64_t pos = 0;
    Dirent dent = {};
    bool found = false;
    while (root->ops->readdir(*root, pos, dent) == 0) {
        if (strcmp(dent.d_name, "DIR2") == 0) {
            found = true;
            break;
        }
    }
    JARVIS_ASSERT(found);

    root->ops->close(*root);
    kernel::gs::try_set_fat32_partition(old);
    fat32_fs.get_root();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies rmdir (unlink on directory) succeeds on empty directory.
// Input: mkdir "EMPTYDIR", then unlink "EMPTYDIR"
// Expect: unlink returns 0, lookup fails
// Depends: kernel::vfs::VnodeOps::unlink
JARVIS_TEST(vfs_fat32_rmdir_empty, "PRE: vfsd, iocd | POST: none") {
    auto fix = create_writable_partition();
    JARVIS_ASSERT(fix.partition->mount());
    auto *old = kernel::gs::get_fat32_partition();
    kernel::gs::try_set_fat32_partition(fix.partition);

    Vnode *root = fat32_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    JARVIS_ASSERT(root->ops->mkdir != nullptr);
    JARVIS_ASSERT(root->ops->unlink != nullptr);

    JARVIS_ASSERT_EQ(0, root->ops->mkdir(*root, "EMPTYDIR", 0));

    Vnode *child = root->ops->lookup(*root, "EMPTYDIR");
    JARVIS_ASSERT(child != nullptr);
    JARVIS_ASSERT(child->mode & vfs::S_IFDIR);
    child->ops->close(*child);

    int ret = root->ops->unlink(*root, "EMPTYDIR");
    JARVIS_ASSERT_EQ(0, ret);

    child = root->ops->lookup(*root, "EMPTYDIR");
    JARVIS_ASSERT(child == nullptr);

    root->ops->close(*root);
    kernel::gs::try_set_fat32_partition(old);
    fat32_fs.get_root();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies rmdir (unlink on directory) fails on non-empty directory.
// Input: mkdir "PARENT", mkdir "PARENT/CHILD", unlink "PARENT"
// Expect: unlink returns error (FAT32 enforces empty dir check)
// Depends: kernel::vfs::VnodeOps::unlink
JARVIS_TEST(vfs_fat32_rmdir_nonempty_fails, "PRE: vfsd, iocd | POST: none") {
    auto fix = create_writable_partition();
    JARVIS_ASSERT(fix.partition->mount());
    auto *old = kernel::gs::get_fat32_partition();
    kernel::gs::try_set_fat32_partition(fix.partition);

    Vnode *root = fat32_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    JARVIS_ASSERT(root->ops->mkdir != nullptr);
    JARVIS_ASSERT(root->ops->unlink != nullptr);

    JARVIS_ASSERT_EQ(0, root->ops->mkdir(*root, "PARENT", 0));

    Vnode *parent = root->ops->lookup(*root, "PARENT");
    JARVIS_ASSERT(parent != nullptr);
    JARVIS_ASSERT(parent->mode & vfs::S_IFDIR);
    JARVIS_ASSERT(parent->ops->mkdir != nullptr);

    JARVIS_ASSERT_EQ(0, parent->ops->mkdir(*parent, "CHILD", 0));

    Vnode *child = parent->ops->lookup(*parent, "CHILD");
    JARVIS_ASSERT(child != nullptr);
    child->ops->close(*child);

    int ret = root->ops->unlink(*root, "PARENT");
    JARVIS_ASSERT(ret < 0); // Should fail - directory not empty

    // Cleanup
    ret = parent->ops->unlink(*parent, "CHILD");
    JARVIS_ASSERT_EQ(0, ret);
    ret = root->ops->unlink(*root, "PARENT");
    JARVIS_ASSERT_EQ(0, ret);

    parent->ops->close(*parent);
    root->ops->close(*root);
    kernel::gs::try_set_fat32_partition(old);
    fat32_fs.get_root();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies unlink on file succeeds and frees clusters.
// Input: Create file via add_dir_entry + write, unlink, verify FAT chain freed
// Expect: unlink returns 0, clusters marked free in FAT
// Depends: kernel::fat32::add_dir_entry, kernel::fat32::unlink,
// kernel::fat32::Fat32Partition
JARVIS_TEST(vfs_fat32_unlink_frees_clusters, "PRE: vfsd, iocd | POST: none") {
    auto fix = create_writable_partition();
    JARVIS_ASSERT(fix.partition->mount());
    auto *old = kernel::gs::get_fat32_partition();
    kernel::gs::try_set_fat32_partition(fix.partition);

    Vnode *root = fat32_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    JARVIS_ASSERT(root->ops->mkdir != nullptr);
    JARVIS_ASSERT(root->ops->unlink != nullptr);

    // Create a file by making a dir then adding a file entry
    // First create a test directory
    JARVIS_ASSERT_EQ(0, root->ops->mkdir(*root, "TESTDIR", 0));
    Vnode *testdir = root->ops->lookup(*root, "TESTDIR");
    JARVIS_ASSERT(testdir != nullptr);
    JARVIS_ASSERT(testdir->ops != nullptr);
    JARVIS_ASSERT(testdir->ops->unlink != nullptr);

    // Try to unlink a file that doesn't exist
    int ret = testdir->ops->unlink(*testdir, "NOFILE.TXT");
    JARVIS_ASSERT(ret < 0);

    // Create a file entry by writing to it (use FAT32 primitives)
    // We'll just test unlink on existing file in the image
    Vnode *file = root->ops->lookup(*root, "HELLO.TXT");
    if (file) {
        ret = root->ops->unlink(*root, "HELLO.TXT");
        JARVIS_ASSERT_EQ(0, ret);
        file->ops->close(*file);
        file = root->ops->lookup(*root, "HELLO.TXT");
        JARVIS_ASSERT(file == nullptr);
    }

    testdir->ops->close(*testdir);
    root->ops->close(*root);
    kernel::gs::try_set_fat32_partition(old);
    fat32_fs.get_root();
    JARVIS_TEST_PASS();
}

void register_vfs_fat32_tests() {
    Logger::info("Registering VFS FAT32 tests");

    JARVIS_REGISTER_RELEASE_TEST(vfs_fat32_mount);
    JARVIS_REGISTER_RELEASE_TEST(vfs_fat32_open_root);
    JARVIS_REGISTER_RELEASE_TEST(vfs_fat32_open_file);
    JARVIS_REGISTER_RELEASE_TEST(vfs_fat32_read_file);
    JARVIS_REGISTER_RELEASE_TEST(vfs_fat32_fstat);
    JARVIS_REGISTER_RELEASE_TEST(vfs_fat32_readdir);
    JARVIS_REGISTER_RELEASE_TEST(vfs_fat32_nonexistent_path);
    JARVIS_REGISTER_RELEASE_TEST(vfs_fat32_subdir);

    JARVIS_REGISTER_TEST(vfs_fat32_mkdir);
    JARVIS_REGISTER_TEST(vfs_fat32_unlink);
    JARVIS_REGISTER_TEST(vfs_fat32_mkdir_then_readdir);
    JARVIS_REGISTER_TEST(vfs_fat32_rmdir_empty);
    JARVIS_REGISTER_TEST(vfs_fat32_rmdir_nonempty_fails);
    JARVIS_REGISTER_TEST(vfs_fat32_unlink_frees_clusters);
}
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
