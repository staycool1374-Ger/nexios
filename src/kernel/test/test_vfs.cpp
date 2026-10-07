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

/// @file test_vfs.cpp
/// @brief VFS (Virtual Filesystem) core tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/vfs/pipe.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/syscall/syscall_helpers.hpp>

using namespace kernel;
using namespace kernel::vfs;

// Runmode: kernel
// Testidea: Tests allocating a single fd from FdTable, verifying the slot is
// marked used, then freeing it and confirming the slot is released.
// Input: Alloc one fd from a fresh FdTable, then free that fd.
// Expect: JARVIS_ASSERT checks fd >= 0, the returned entry is non-null and
// used; after free, entry is null or not used.
// Depends: kernel::vfs::FdTable
JARVIS_TEST(vfs_fdtable_alloc_free, "PRE: vfsd, iocd | POST: none") {
    vfs::FdTable ft;
    int fd1 = ft.alloc();
    JARVIS_ASSERT(fd1 >= 0);
    JARVIS_ASSERT(ft.get(fd1) != nullptr);
    JARVIS_ASSERT(ft.get(fd1)->used);
    ft.free(fd1);
    JARVIS_ASSERT(ft.get(fd1) == nullptr || !ft.get(fd1)->used);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests allocating and freeing multiple fds from FdTable, then
// verifying that a recycled fd can be re-allocated.
// Input: Alloc 5 fds, free all 5, then alloc one more fd.
// Expect: JARVIS_ASSERT checks all allocations return non-negative fd; final
// alloc also succeeds.
// Depends: kernel::vfs::FdTable
JARVIS_TEST(vfs_fdtable_multiple, "PRE: vfsd, iocd | POST: none") {
    vfs::FdTable ft;
    int fds[5];
    for (int i = 0; i < 5; ++i) {
        fds[i] = ft.alloc();
        JARVIS_ASSERT(fds[i] >= 0);
    }
    for (int i = 0; i < 5; ++i)
        ft.free(fds[i]);
    int fd = ft.alloc();
    JARVIS_ASSERT(fd >= 0);
    ft.free(fd);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests that resolving the root path "/" returns a valid vnode
// with directory mode.
// Input: vfs::resolve("/")
// Expect: JARVIS_ASSERT checks vnode is non-null and has S_IFDIR set.
// Depends: kernel::vfs::resolve
JARVIS_TEST(vfs_resolve_root, "PRE: vfsd, iocd | POST: none") {
    auto *vn = vfs::resolve("/");
    JARVIS_ASSERT(vn != nullptr);
    JARVIS_ASSERT(vn->mode & vfs::S_IFDIR);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests that resolving "/dev" returns a valid directory vnode.
// Input: vfs::resolve("/dev")
// Expect: JARVIS_ASSERT checks vnode is non-null and has S_IFDIR set.
// Depends: kernel::vfs::resolve
JARVIS_TEST(vfs_resolve_dev, "PRE: vfsd, iocd | POST: none") {
    auto *vn = vfs::resolve("/dev");
    JARVIS_ASSERT(vn != nullptr);
    JARVIS_ASSERT(vn->mode & vfs::S_IFDIR);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests that resolving "/dev/tty" returns a valid character-device
// vnode.
// Input: vfs::resolve("/dev/tty")
// Expect: JARVIS_ASSERT checks vnode is non-null and has S_IFCHR set.
// Depends: kernel::vfs::resolve
JARVIS_TEST(vfs_resolve_tty, "PRE: vfsd, iocd | POST: none") {
    auto *vn = vfs::resolve("/dev/tty");
    JARVIS_ASSERT(vn != nullptr);
    JARVIS_ASSERT(vn->mode & vfs::S_IFCHR);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests that resolving "/dev/null" returns a valid
// character-device vnode.
// Input: vfs::resolve("/dev/null")
// Expect: JARVIS_ASSERT checks vnode is non-null and has S_IFCHR set.
// Depends: kernel::vfs::resolve
JARVIS_TEST(vfs_resolve_null, "PRE: vfsd, iocd | POST: none") {
    auto *vn = vfs::resolve("/dev/null");
    JARVIS_ASSERT(vn != nullptr);
    JARVIS_ASSERT(vn->mode & vfs::S_IFCHR);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests that resolving "/proc" returns a valid directory vnode.
// Input: vfs::resolve("/proc")
// Expect: JARVIS_ASSERT checks vnode is non-null and has S_IFDIR set.
// Depends: kernel::vfs::resolve
JARVIS_TEST(vfs_resolve_proc, "PRE: vfsd, iocd | POST: none") {
    auto *vn = vfs::resolve("/proc");
    JARVIS_ASSERT(vn != nullptr);
    JARVIS_ASSERT(vn->mode & vfs::S_IFDIR);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Tests that resolving a nonexistent path returns nullptr.
// Input: vfs::resolve("/nonexistent_path_xyz")
// Expect: JARVIS_ASSERT checks result is nullptr.
// Depends: kernel::vfs::resolve
JARVIS_TEST(vfs_resolve_nonexistent, "PRE: vfsd, iocd | POST: none") {
    auto *vn = vfs::resolve("/nonexistent_path_xyz");
    JARVIS_ASSERT(vn == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies resolution of relative paths "." and ".." works.
// Input: resolve("/dev/./tty"), resolve("/dev/../dev/tty")
// Expect: Both return valid character-device vnodes
// Depends: kernel::vfs::resolve
JARVIS_TEST(vfs_resolve_relative_path, "PRE: vfsd, iocd | POST: none") {
    Vnode *vn = vfs::resolve("/dev/./tty");
    JARVIS_ASSERT(vn != nullptr);
    JARVIS_ASSERT(vn->mode & vfs::S_IFCHR);
    vn = vfs::resolve("/dev/../dev/tty");
    JARVIS_ASSERT(vn != nullptr);
    JARVIS_ASSERT(vn->mode & vfs::S_IFCHR);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies ".." resolves to the parent directory.
// Input: resolve("/dev/..")
// Expect: Returns the root directory vnode
// Depends: kernel::vfs::resolve
JARVIS_TEST(vfs_resolve_dotdot, "PRE: vfsd, iocd | POST: none") {
    Vnode *dev = vfs::resolve("/dev");
    JARVIS_ASSERT(dev != nullptr);
    Vnode *root = vfs::resolve("/dev/..");
    JARVIS_ASSERT(root != nullptr);
    JARVIS_ASSERT(root->mode & vfs::S_IFDIR);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies mounting a filesystem registers it in the mount table
//           and resolve can find root.
// Input: mount(fake_fs, "/mnt/testfs")
// Expect: Mount succeeds, resolve("/mnt/testfs") returns root vnode
// Depends: kernel::vfs::mount, kernel::vfs::resolve, kernel::vfs::Filesystem
static Vnode fake_root_vnode;

static Vnode *fake_get_root() {
    return &fake_root_vnode;
}

static kernel::vfs::Filesystem fake_fs = {.name = "fakefs",
                                          .get_root = fake_get_root};

JARVIS_TEST(vfs_mount_unmount, "PRE: vfsd, iocd | POST: none") {
    fake_root_vnode = Vnode{};
    fake_root_vnode.mode = vfs::S_IFDIR;
    fake_root_vnode.ino = 1;

    int ret = vfs::mount(fake_fs, "/mnt/testfs");
    JARVIS_ASSERT_EQ(0, ret);

    Vnode *vn = vfs::resolve("/mnt/testfs");
    JARVIS_ASSERT(vn != nullptr);
    JARVIS_ASSERT(vn->mode & vfs::S_IFDIR);
    JARVIS_ASSERT(vn == &fake_root_vnode);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies vfs::mkdir creates a directory at a valid path.
// Input: vfs::mkdir("/mnt/NEWDIR_TEST", 0)
// Expect: Returns 0, vfs::resolve finds the new directory with S_IFDIR
// Depends: kernel::vfs::mkdir, kernel::vfs::resolve
JARVIS_TEST(vfs_mkdir_valid, "PRE: vfsd, iocd | POST: none") {
    int ret = vfs::mkdir("/tmp/VFS_MKDIR_TEST", 0);
    JARVIS_ASSERT_EQ(0, ret);

    Vnode *vn = vfs::resolve("/tmp/VFS_MKDIR_TEST");
    JARVIS_ASSERT(vn != nullptr);
    JARVIS_ASSERT(vn->mode & vfs::S_IFDIR);

    ret = vfs::unlink("/tmp/VFS_MKDIR_TEST");
    JARVIS_ASSERT_EQ(0, ret);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies vfs::mkdir fails for nonexistent parent.
// Input: vfs::mkdir("/nonexistent/DIR", 0)
// Expect: Returns VFS_INVALID
// Depends: kernel::vfs::mkdir
JARVIS_TEST(vfs_mkdir_nonexistent_parent, "PRE: vfsd, iocd | POST: none") {
    int ret = vfs::mkdir("/nonexistent/DIR", 0);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies vfs::mkdir fails when parent is not a directory.
// Input: vfs::mkdir("/dev/null/DIR", 0) where null is a char device
// Expect: Returns VFS_INVALID
// Depends: kernel::vfs::mkdir
JARVIS_TEST(vfs_mkdir_parent_not_dir, "PRE: vfsd, iocd | POST: none") {
    int ret = vfs::mkdir("/dev/null/DIR", 0);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies vfs::unlink removes a file/directory.
// Input: Create /tmp/UNLINK_ME, then vfs::unlink
// Expect: Returns 0, vfs::resolve no longer finds the path
// Depends: kernel::vfs::unlink, kernel::vfs::resolve
JARVIS_TEST(vfs_unlink_file, "PRE: vfsd, iocd | POST: none") {
    int ret = vfs::mkdir("/tmp/UNLINK_ME", 0);
    JARVIS_ASSERT_EQ(0, ret);

    Vnode *vn = vfs::resolve("/tmp/UNLINK_ME");
    JARVIS_ASSERT(vn != nullptr);

    ret = vfs::unlink("/tmp/UNLINK_ME");
    JARVIS_ASSERT_EQ(0, ret);

    vn = vfs::resolve("/tmp/UNLINK_ME");
    JARVIS_ASSERT(vn == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies vfs::unlink fails for nonexistent path.
// Input: vfs::unlink("/tmp/NONEXISTENT")
// Expect: Returns VFS_INVALID
// Depends: kernel::vfs::unlink
JARVIS_TEST(vfs_unlink_nonexistent, "PRE: vfsd, iocd | POST: none") {
    int ret = vfs::unlink("/tmp/NONEXISTENT");
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies vfs::unlink fails for non-empty directory.
// Input: vfs::mkdir("/tmp/DIR"), create subdir inside, vfs::unlink("/tmp/DIR")
// Expect: Returns VFS_INVALID (tmpfs enforces empty dir check)
// Depends: kernel::vfs::unlink, kernel::vfs::mkdir
JARVIS_TEST(vfs_unlink_nonempty_dir_fails, "PRE: vfsd, iocd | POST: none") {
    int ret = vfs::mkdir("/tmp/NONEMPTYDIR", 0);
    JARVIS_ASSERT_EQ(0, ret);

    ret = vfs::mkdir("/tmp/NONEMPTYDIR/CHILD", 0);
    JARVIS_ASSERT_EQ(0, ret);

    ret = vfs::unlink("/tmp/NONEMPTYDIR");
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, ret);

    ret = vfs::unlink("/tmp/NONEMPTYDIR/CHILD");
    JARVIS_ASSERT_EQ(0, ret);
    ret = vfs::unlink("/tmp/NONEMPTYDIR");
    JARVIS_ASSERT_EQ(0, ret);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Opens /dev/null via vnode ops, reads 0 bytes (EOF), then closes.
// Input: Resolve /dev/null, alloc fd, call ops->read, ops->close.
// Expect: read returns 0; close succeeds.
// Depends: kernel::vfs, kernel::Scheduler
JARVIS_TEST(vfs_open_read_null, "PRE: vfsd, iocd | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    Vnode *vn = vfs::resolve("/dev/null");
    JARVIS_ASSERT(vn != nullptr);

    int fd = cur->fd_table.alloc();
    JARVIS_ASSERT(fd >= 0);

    int ret = vn->ops->open(*vn, 0);
    JARVIS_ASSERT_EQ(0, ret);
    cur->fd_table.get(fd)->vnode = vn;
    cur->fd_table.get(fd)->offset = 0;
    cur->fd_table.get(fd)->flags = 0;

    char buf[32];
    int64_t nread = vn->ops->read(*vn, reinterpret_cast<uint8_t *>(buf), 10, 0);
    JARVIS_ASSERT_EQ(0LL, nread);

    vn->ops->close(*vn);
    cur->fd_table.free(fd);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Opens /dev/tty via vnode ops, writes a string, retrieves stat,
// then closes.
// Input: Resolve /dev/tty, alloc fd, call ops->write, ops->fstat, ops->close.
// Expect: write returns 4; fstat returns 0; close succeeds.
// Depends: kernel::vfs, kernel::Scheduler
JARVIS_TEST(vfs_write_fstat, "PRE: vfsd, iocd | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    Vnode *vn = vfs::resolve("/dev/tty");
    JARVIS_ASSERT(vn != nullptr);

    int fd = cur->fd_table.alloc();
    JARVIS_ASSERT(fd >= 0);
    cur->fd_table.get(fd)->vnode = vn;
    cur->fd_table.get(fd)->offset = 0;
    cur->fd_table.get(fd)->flags = 0;

    const char *msg = "test";
    int64_t nwritten =
        vn->ops->write(*vn, reinterpret_cast<const uint8_t *>(msg), 4, 0);
    JARVIS_ASSERT_EQ(4LL, nwritten);

    VfsStat st{};
    int ret = vn->ops->fstat(*vn, st);
    JARVIS_ASSERT_EQ(0, ret);

    vn->ops->close(*vn);
    cur->fd_table.free(fd);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Creates a pipe, writes data to the write end, reads it back from
// the read end, and validates the content.
// Input: vfs::create_pipe, vnode->ops->write/read.
// Expect: create_pipe returns 0; write returns 4; read returns 4 with
// matching content.
// Depends: kernel::vfs::create_pipe, kernel::Scheduler
JARVIS_TEST(vfs_pipe_read_write, "PRE: vfsd, iocd | POST: none") {
    int fds[2];
    int ret = vfs::create_pipe(fds);
    JARVIS_ASSERT_EQ(0, ret);
    JARVIS_ASSERT(fds[0] >= 0);
    JARVIS_ASSERT(fds[1] >= 0);
    JARVIS_ASSERT(fds[0] != fds[1]);

    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    auto *rnode = cur->fd_table.get(fds[0])->vnode;
    auto *wnode = cur->fd_table.get(fds[1])->vnode;
    JARVIS_ASSERT(rnode != nullptr);
    JARVIS_ASSERT(wnode != nullptr);

    const char *msg = "pipe";
    int64_t nwritten =
        wnode->ops->write(*wnode, reinterpret_cast<const uint8_t *>(msg), 4, 0);
    JARVIS_ASSERT_EQ(4LL, nwritten);

    char buf[32];
    int64_t nread =
        rnode->ops->read(*rnode, reinterpret_cast<uint8_t *>(buf), 10, 0);
    JARVIS_ASSERT_EQ(4LL, nread);
    JARVIS_ASSERT_EQ('p', buf[0]);
    JARVIS_ASSERT_EQ('i', buf[1]);
    JARVIS_ASSERT_EQ('p', buf[2]);
    JARVIS_ASSERT_EQ('e', buf[3]);

    // Single-close FdTable path: FdTable::free() ref-decs the vnode and
    // calls ops->close only when the last reference is released.  Manual
    // ops->close on both ends would free the vnodes, then fd_table.free
    // would ref-dec the freed blocks (use-after-free, vfs.md §6).
    cur->fd_table.free(fds[0]);
    cur->fd_table.free(fds[1]);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all VFS test cases into the test framework.
// Input: None
// Expect: Each JARVIS_REGISTER_TEST call adds the test to the suite.
// Depends: kernel::test

// Runmode: kernel
// Testidea: issue #268 — repeated initrd resolve+release leaves the vnode
// counters flat (pre-fix: +1 vnode per resolve, draining the 32-block
// pool3 until every lookup fails).
// Input: 40x resolve("/hey.c.elf") + release (initrd-resident demo ELF).
// Expect: ResourceTracker vnodes identical before/after.
// Depends: vfs::resolve OWNED convention + vfs::release.
JARVIS_TEST(vfs_initrd_lookup_release_flat, "PRE: vfsd, iocd | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);
    for (int i = 0; i < 40; ++i) {
        auto *vn = vfs::resolve("/hey.c.elf");
        JARVIS_ASSERT(vn != nullptr);
        vfs::release(vn);
    }
    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.vnodes, after.vnodes);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: initrd /bin hierarchy resolves (issue #274): a command ELF
// under /bin resolves S_IFREG, /bin itself resolves S_IFDIR, a missing
// /bin child returns null, and the /hey.c.elf root canary still works.
// Input: resolve("/bin/cat.c.elf"), resolve("/bin"),
// resolve("/bin/nope-274"), resolve("/hey.c.elf") with vnode counters
// captured around the cycle.
// Expect: bin ELF non-null S_IFREG; /bin non-null S_IFDIR; missing child
// null; root canary non-null; vnode counters flat; every owned vnode
// released exactly once.
// Depends: vfs::resolve OWNED convention + initrd dir-vnode lookup.
JARVIS_TEST(vfs_initrd_bin_resolve, "PRE: vfsd, iocd | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);
    auto *bin_elf = vfs::resolve("/bin/cat.c.elf");
    JARVIS_ASSERT(bin_elf != nullptr);
    JARVIS_ASSERT(bin_elf->mode & vfs::S_IFREG);
    auto *bin_dir = vfs::resolve("/bin");
    JARVIS_ASSERT(bin_dir != nullptr);
    JARVIS_ASSERT(bin_dir->mode & vfs::S_IFDIR);
    auto *missing = vfs::resolve("/bin/nope-274");
    JARVIS_ASSERT(missing == nullptr);
    auto *root_canary = vfs::resolve("/hey.c.elf");
    JARVIS_ASSERT(root_canary != nullptr);
    vfs::release(bin_elf);
    vfs::release(bin_dir);
    vfs::release(root_canary);
    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.vnodes, after.vnodes);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: issue #268 regression guard — tmpfs cached vnodes are
// borrowed (refcount 0), so resolve+release cycles must neither leak nor
// double-free the cache entries (release is a no-op on them).
// Input: mkdir + create nested tmpfs file, 20x resolve+release, then
// resolve once more (cache entry must be alive and correct).
// Expect: vnodes flat; final resolve non-null with S_IFREG.
JARVIS_TEST(vfs_tmpfs_nested_resolve_balanced,
            "PRE: vfsd, iocd | POST: none") {
    JARVIS_ASSERT(vfs::mkdir("/tmp/d268", 0755) == 0);
    JARVIS_ASSERT(vfs::create("/tmp/d268/f", vfs::S_IFREG) == 0);
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);
    for (int i = 0; i < 20; ++i) {
        auto *vn = vfs::resolve("/tmp/d268/f");
        JARVIS_ASSERT(vn != nullptr);
        vfs::release(vn);
    }
    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.vnodes, after.vnodes);
    auto *check = vfs::resolve("/tmp/d268/f");
    JARVIS_ASSERT(check != nullptr);
    JARVIS_ASSERT(check->mode & vfs::S_IFREG);
    vfs::release(check);
    JARVIS_ASSERT(vfs::unlink("/tmp/d268/f") == 0);
    JARVIS_ASSERT(vfs::unlink("/tmp/d268") == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: issue #268 — mirror the shell `ls` coloring loop
// (readdir + per-entry lookup) with releases: 3 full passes over the
// initrd root must not move the vnode counters (pre-fix: +entries/pass).
JARVIS_TEST(vfs_ls_lookup_loop_no_leak, "PRE: vfsd, iocd | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);
    auto *root = vfs::resolve("/");
    JARVIS_ASSERT(root != nullptr);
    for (int pass = 0; pass < 3; ++pass) {
        uint64_t pos = 0;
        vfs::Dirent dent{};
        while (root->ops->readdir(*root, pos, dent) == 0) {
            if (dent.d_name[0] == '\0')
                continue;
            auto *child = root->ops->lookup(*root, dent.d_name);
            vfs::release(child);
        }
    }
    vfs::release(root);
    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.vnodes, after.vnodes);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: issue #268 — fd open/close cycles balance vnode + fd counters
// on initrd (owned lookup vnodes) and tmpfs (borrowed cache vnodes).
JARVIS_TEST(vfs_fd_open_close_balanced, "PRE: vfsd, iocd | POST: none") {
    auto *cur = kernel::Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    JARVIS_ASSERT(vfs::create("/tmp/fd268", vfs::S_IFREG) == 0);
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);
    for (int i = 0; i < 10; ++i) {
        auto *vn = vfs::resolve("/hey.c.elf");
        JARVIS_ASSERT(vn != nullptr);
        int fd = kernel::syscall_task_open(vn, vfs::O_RDONLY);
        JARVIS_ASSERT(fd >= 0);
        cur->fd_table.free(fd);
    }
    for (int i = 0; i < 10; ++i) {
        auto *vn = vfs::resolve("/tmp/fd268");
        JARVIS_ASSERT(vn != nullptr);
        int fd = kernel::syscall_task_open(vn, vfs::O_RDONLY);
        JARVIS_ASSERT(fd >= 0);
        cur->fd_table.free(fd);
    }
    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.vnodes, after.vnodes);
    JARVIS_ASSERT_EQ(before.open_fds, after.open_fds);
    JARVIS_ASSERT(vfs::unlink("/tmp/fd268") == 0);
    JARVIS_TEST_PASS();
}

void register_vfs_tests() {
    Logger::info("Registering VFS tests");

    JARVIS_REGISTER_TEST(vfs_fdtable_alloc_free);
    JARVIS_REGISTER_TEST(vfs_fdtable_multiple);

    JARVIS_REGISTER_TEST(vfs_resolve_root);
    JARVIS_REGISTER_TEST(vfs_resolve_dev);
    JARVIS_REGISTER_TEST(vfs_resolve_tty);
    JARVIS_REGISTER_TEST(vfs_resolve_null);
    JARVIS_REGISTER_TEST(vfs_resolve_proc);
    JARVIS_REGISTER_TEST(vfs_resolve_nonexistent);

    JARVIS_REGISTER_TEST(vfs_resolve_relative_path);
    JARVIS_REGISTER_TEST(vfs_resolve_dotdot);
    JARVIS_REGISTER_TEST(vfs_mount_unmount);
    JARVIS_REGISTER_TEST(vfs_mkdir_valid);
    JARVIS_REGISTER_TEST(vfs_mkdir_nonexistent_parent);
    JARVIS_REGISTER_TEST(vfs_mkdir_parent_not_dir);
    JARVIS_REGISTER_TEST(vfs_unlink_file);
    JARVIS_REGISTER_TEST(vfs_unlink_nonexistent);
    JARVIS_REGISTER_TEST(vfs_unlink_nonempty_dir_fails);

    JARVIS_REGISTER_TEST(vfs_open_read_null);
    JARVIS_REGISTER_TEST(vfs_write_fstat);
    JARVIS_REGISTER_TEST(vfs_pipe_read_write);
    JARVIS_REGISTER_TEST(vfs_initrd_lookup_release_flat); // #268
    JARVIS_REGISTER_TEST(vfs_initrd_bin_resolve); // #274
    JARVIS_REGISTER_TEST(vfs_tmpfs_nested_resolve_balanced); // #268
    JARVIS_REGISTER_TEST(vfs_ls_lookup_loop_no_leak); // #268
    JARVIS_REGISTER_TEST(vfs_fd_open_close_balanced); // #268
}
