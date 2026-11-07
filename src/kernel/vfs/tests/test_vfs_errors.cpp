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

/// @file test_vfs_errors.cpp
/// @brief VFS error-code API tests (milestone v0.4.10 issue #124): the whole
///        `*_err` family in src/kernel/vfs/vfs.cpp (alloc_err, free_err,
///        get_err, resolve_err, mount_err, init_err, find_fs_err,
///        set_root_vnode_err, mkdir_err, create_err, unlink_err) was never
///        entered, so none of the VfsError codes defined in vfs_errors.hpp
///        were ever exercised — including the NOT_DIR / NOT_SUPPORTED /
///        NOT_FOUND / IO_ERROR discrimination rules.
/// @note  Any test that creates tmpfs entries unlinks them BEFORE asserting,
///        so an assertion early-return can never leave MemPool blocks behind.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/vfs/tmpfs.hpp>
#include <kernel/vfs/devfs.hpp>
#include <assert.hpp>
#include <string.hpp>

using namespace kernel;
using namespace kernel::errors;

namespace {

/// @brief A filesystem driver whose get_root() is absent — rejected before
/// the mount table is touched.
vfs::Filesystem g_no_getroot_fs = {"verr-no-getroot", nullptr};

/// @brief A filesystem driver whose get_root() legitimately fails.
vfs::Vnode *failing_get_root() {
    return nullptr;
}
vfs::Filesystem g_failing_fs = {"verr-failing", failing_get_root};

/// @brief A filesystem driver that hands out an existing static root.
vfs::Vnode *shim_get_root() {
    return vfs::tmpfs_fs.get_root();
}
vfs::Filesystem g_shim_fs = {"verr-shim", shim_get_root};

/// @brief Guarantees the standard mount set ("/", "/dev", "/proc", "/tmp")
/// is present — only "/" is mounted at boot; the rest appear after the first
/// VFS-touching snapshot restore.
void ensure_standard_mounts() {
    if (vfs::resolve("/tmp") == nullptr)
        vfs::reset_and_remount();
}

bool same_text(const char *lhs, const char *rhs) {
    return strcmp(lhs, rhs) == 0;
}

} // namespace

// Runmode: kernel
// Testidea: The FdTable error API allocates, looks up and releases slots with
// precise codes: success is VFS_ERR_OK, out-of-range and unused descriptors
// are VFS_ERR_INVALID_FD, and an exhausted table reports
// VFS_ERR_FD_TABLE_FULL instead of silently returning a sentinel.
// Input: alloc_err on a fresh table; get_err on the live fd, on -1, on
//        MAX_FDS and after free; free_err twice and out of range; finally
//        drain the table and over-allocate by one.
// Expect: alloc_err OK and fd in range; get_err OK for the live fd and
//         INVALID_FD for -1/MAX_FDS/released; free_err OK and idempotent,
//         INVALID_FD out of range; the (MAX_FDS+1)-th alloc_err returns
//         FD_TABLE_FULL; every allocated fd is released again.
// Depends: vfs::FdTable, ResourceTracker
JARVIS_TEST(vfs_fdtable_err_codes, "PRE: vfsd, iocd | POST: none") {
    vfs::FdTable table;
    int first_fd = -1;
    VfsError alloc_first = table.alloc_err(first_fd);

    vfs::FileDescription *entry = nullptr;
    VfsError get_live = table.get_err(first_fd, entry);
    // Capture while the slot is still live: `entry` points INTO the table,
    // so its `used` flag follows every later free.
    bool entry_live_at_get = (entry != nullptr) && entry->used;
    bool entry_is_slot =
        (entry != nullptr) && (entry == &table.fds[first_fd >= 0 ? first_fd
                                                                 : 0]);

    vfs::FileDescription *unused = nullptr;
    VfsError get_negative = table.get_err(-1, unused);
    VfsError get_too_big = table.get_err(static_cast<int>(vfs::MAX_FDS),
                                         unused);

    VfsError free_first = table.free_err(first_fd);
    VfsError free_again = table.free_err(first_fd);
    VfsError free_negative = table.free_err(-1);
    VfsError free_too_big = table.free_err(static_cast<int>(vfs::MAX_FDS));
    VfsError get_after_free = table.get_err(first_fd, unused);

    // Drain the whole table, then prove the next allocation is refused.
    int drain_fds[vfs::MAX_FDS] = {};
    VfsError drain_errors[vfs::MAX_FDS] = {};
    for (size_t idx = 0; idx < vfs::MAX_FDS; ++idx)
        drain_errors[idx] = table.alloc_err(drain_fds[idx]);
    int overflow_fd = -1;
    VfsError overflow = table.alloc_err(overflow_fd);
    for (size_t idx = 0; idx < vfs::MAX_FDS; ++idx)
        (void)table.free_err(drain_fds[idx]);

    bool fd_in_range = first_fd >= 0 &&
                       static_cast<size_t>(first_fd) < vfs::MAX_FDS;

    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_OK),
                     static_cast<uint64_t>(alloc_first));
    JARVIS_ASSERT(fd_in_range);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_OK),
                     static_cast<uint64_t>(get_live));
    JARVIS_ASSERT(entry_live_at_get);
    JARVIS_ASSERT(entry_is_slot);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_INVALID_FD),
                     static_cast<uint64_t>(get_negative));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_INVALID_FD),
                     static_cast<uint64_t>(get_too_big));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_OK),
                     static_cast<uint64_t>(free_first));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_OK),
                     static_cast<uint64_t>(free_again));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_INVALID_FD),
                     static_cast<uint64_t>(free_negative));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_INVALID_FD),
                     static_cast<uint64_t>(free_too_big));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_INVALID_FD),
                     static_cast<uint64_t>(get_after_free));
    for (size_t idx = 0; idx < vfs::MAX_FDS; ++idx)
        JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_OK),
                         static_cast<uint64_t>(drain_errors[idx]));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_FD_TABLE_FULL),
                     static_cast<uint64_t>(overflow));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: resolve_err distinguishes a resolvable mount root from every
// failure shape: a missing path yields NOT_FOUND, and both a null pointer and
// an empty string are rejected as NOT_FOUND rather than resolving to the
// root.  The error string table maps the code to its documented text.
// Input: resolve_err("/"), resolve_err("/no/such/vfs-err-path"),
//        resolve_err(nullptr), resolve_err(""), error_string(NOT_FOUND).
// Expect: "/" -> OK with a non-null vnode; the other three -> NOT_FOUND
//         with out_vnode untouched; error_string matches the
//         vfs_errors.hpp text.
// Depends: vfs::resolve_err, errors::error_string
JARVIS_TEST(vfs_resolve_err_codes, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = nullptr;
    VfsError root_err = vfs::resolve_err("/", root);

    vfs::Vnode *missing = nullptr;
    VfsError missing_err = vfs::resolve_err("/no/such/vfs-err-path", missing);

    vfs::Vnode *from_null = nullptr;
    VfsError null_err = vfs::resolve_err(nullptr, from_null);

    vfs::Vnode *from_empty = nullptr;
    VfsError empty_err = vfs::resolve_err("", from_empty);

    const char *not_found_text =
        kernel::errors::error_string(VFS_ERR_NOT_FOUND);
    bool text_ok = same_text(not_found_text, "Path or vnode not found");

    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_OK),
                     static_cast<uint64_t>(root_err));
    JARVIS_ASSERT(root != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_NOT_FOUND),
                     static_cast<uint64_t>(missing_err));
    JARVIS_ASSERT(missing == nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_NOT_FOUND),
                     static_cast<uint64_t>(null_err));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_NOT_FOUND),
                     static_cast<uint64_t>(empty_err));
    JARVIS_ASSERT(text_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: find_fs_err resolves mounted filesystems by name, rejects a null
// name with INVALID_ARGS before touching the table, and reports NO_SUCH_FS
// for an unknown driver instead of nullptr-with-OK.
// Input: find_fs_err("devfs"), find_fs_err("tmpfs"),
//        find_fs_err("no-such-fs"), find_fs_err(nullptr).
// Expect: devfs -> OK with &dev_fs; tmpfs -> OK; unknown -> NO_SUCH_FS;
//         null -> INVALID_ARGS.
// Depends: vfs::find_fs_err
JARVIS_TEST(vfs_find_fs_err_codes, "PRE: vfsd, iocd | POST: none") {
    ensure_standard_mounts();

    vfs::Filesystem *devfs = nullptr;
    VfsError devfs_err = vfs::find_fs_err("devfs", devfs);
    vfs::Filesystem *tmpfs = nullptr;
    VfsError tmpfs_err = vfs::find_fs_err("tmpfs", tmpfs);
    vfs::Filesystem *unknown = nullptr;
    VfsError unknown_err = vfs::find_fs_err("no-such-fs", unknown);
    vfs::Filesystem *from_null = nullptr;
    VfsError null_err = vfs::find_fs_err(nullptr, from_null);

    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_OK),
                     static_cast<uint64_t>(devfs_err));
    JARVIS_ASSERT_EQ(&vfs::dev_fs, devfs);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_OK),
                     static_cast<uint64_t>(tmpfs_err));
    JARVIS_ASSERT(tmpfs != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_NO_SUCH_FS),
                     static_cast<uint64_t>(unknown_err));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_INVALID_ARGS),
                     static_cast<uint64_t>(null_err));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: mount_err validates before mutating the mount table: a driver
// without get_root is INVALID_ARGS, a driver whose get_root fails is
// NO_DEVICE, and a driver with a live root mounts successfully.  The table
// is restored afterwards so the extra mount never leaks into later tests.
// Input: mount_err(no-getroot fs), mount_err(failing fs),
//        mount_err(shim fs, "/mnt/vfs-err"), find_fs_err("verr-shim"),
//        then reset_and_remount().
// Expect: INVALID_ARGS, NO_DEVICE, OK, and the shim is findable; after the
//         reset the shim is gone again.
// Depends: vfs::mount_err, vfs::find_fs_err, vfs::reset_and_remount
JARVIS_TEST(vfs_mount_err_codes, "PRE: vfsd, iocd | POST: none") {
    VfsError no_getroot = vfs::mount_err(g_no_getroot_fs, "/mnt/vfs-err-none");
    VfsError no_device = vfs::mount_err(g_failing_fs, "/mnt/vfs-err-fail");
    VfsError mounted = vfs::mount_err(g_shim_fs, "/mnt/vfs-err");

    vfs::Filesystem *found = nullptr;
    VfsError found_err = vfs::find_fs_err("verr-shim", found);

    vfs::reset_and_remount();
    vfs::Filesystem *after_reset = nullptr;
    VfsError after_reset_err = vfs::find_fs_err("verr-shim", after_reset);

    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_INVALID_ARGS),
                     static_cast<uint64_t>(no_getroot));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_NO_DEVICE),
                     static_cast<uint64_t>(no_device));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_OK),
                     static_cast<uint64_t>(mounted));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_OK),
                     static_cast<uint64_t>(found_err));
    JARVIS_ASSERT_EQ(&g_shim_fs, found);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_NO_SUCH_FS),
                     static_cast<uint64_t>(after_reset_err));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: mkdir_err / create_err / unlink_err map every failure shape to a
// distinct code: OK on success, IO_ERROR when the backend refuses (duplicate
// or missing entry), NOT_FOUND when the parent does not resolve, NOT_DIR when
// the parent is not a directory, and NOT_SUPPORTED when the parent's ops
// table has no such operation (devfs is a device namespace).
// Input: On tmpfs: mkdir twice, create twice, unlink file twice, unlink dir.
//        On a non-existent parent: mkdir/create/unlink.  On the /dev/tty
//        char device: mkdir/create/unlink.  On the /dev root: mkdir/create/
//        unlink.  All entries are removed before any assertion.
// Expect: OK, IO_ERROR, IO_ERROR, OK, IO_ERROR, OK for the tmpfs sequence;
//         NOT_FOUND x3; NOT_DIR x3; NOT_SUPPORTED x3.
// Depends: vfs::mkdir_err, vfs::create_err, vfs::unlink_err, tmpfs
JARVIS_TEST(vfs_mkdir_create_unlink_err_codes,
            "PRE: vfsd, iocd | POST: none") {
    ensure_standard_mounts();
    JARVIS_ASSERT(vfs::resolve("/tmp") != nullptr);

    VfsError mkdir_ok = vfs::mkdir_err("/tmp/verr_dir", 0);
    VfsError mkdir_dup = vfs::mkdir_err("/tmp/verr_dir", 0);
    VfsError create_ok = vfs::create_err("/tmp/verr_file", 0);
    VfsError create_dup = vfs::create_err("/tmp/verr_file", 0);
    VfsError unlink_ok = vfs::unlink_err("/tmp/verr_file");
    VfsError unlink_gone = vfs::unlink_err("/tmp/verr_file");
    VfsError unlink_dir = vfs::unlink_err("/tmp/verr_dir");

    VfsError mkdir_orphan = vfs::mkdir_err("/no/such/verr_dir", 0);
    VfsError create_orphan = vfs::create_err("/no/such/verr_file", 0);
    VfsError unlink_orphan = vfs::unlink_err("/no/such/verr_file");

    VfsError mkdir_notdir = vfs::mkdir_err("/dev/tty/verr_dir", 0);
    VfsError create_notdir = vfs::create_err("/dev/tty/verr_file", 0);
    VfsError unlink_notdir = vfs::unlink_err("/dev/tty/verr_file");

    VfsError mkdir_unsupported = vfs::mkdir_err("/dev/verr_dir", 0);
    VfsError create_unsupported = vfs::create_err("/dev/verr_file", 0);
    VfsError unlink_unsupported = vfs::unlink_err("/dev/verr_file");

    // Cleanup before any assertion: an early return must not leak entries.
    (void)vfs::unlink_err("/tmp/verr_dir");
    (void)vfs::unlink_err("/tmp/verr_file");

    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_OK),
                     static_cast<uint64_t>(mkdir_ok));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_IO_ERROR),
                     static_cast<uint64_t>(mkdir_dup));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_OK),
                     static_cast<uint64_t>(create_ok));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_IO_ERROR),
                     static_cast<uint64_t>(create_dup));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_OK),
                     static_cast<uint64_t>(unlink_ok));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_IO_ERROR),
                     static_cast<uint64_t>(unlink_gone));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_OK),
                     static_cast<uint64_t>(unlink_dir));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_NOT_FOUND),
                     static_cast<uint64_t>(mkdir_orphan));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_NOT_FOUND),
                     static_cast<uint64_t>(create_orphan));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_NOT_FOUND),
                     static_cast<uint64_t>(unlink_orphan));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_NOT_DIR),
                     static_cast<uint64_t>(mkdir_notdir));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_NOT_DIR),
                     static_cast<uint64_t>(create_notdir));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_NOT_DIR),
                     static_cast<uint64_t>(unlink_notdir));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_NOT_SUPPORTED),
                     static_cast<uint64_t>(mkdir_unsupported));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_NOT_SUPPORTED),
                     static_cast<uint64_t>(create_unsupported));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_NOT_SUPPORTED),
                     static_cast<uint64_t>(unlink_unsupported));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: init_err tears the mount table down to a known-empty state
// (root vnode null, no mounted filesystems) and set_root_vnode_err installs
// a root explicitly — the pair is what makes VFS state re-constructible
// after a reset.  The standard mounts are restored before asserting so the
// global VFS is never left empty.
// Input: init_err(); get_root_vnode(); find_fs_err("devfs");
//        reset_and_remount(); get_root_vnode(); set_root_vnode_err(root).
// Expect: init_err OK; root null and devfs NO_SUCH_FS right after it; after
//         the reset devfs is back; set_root_vnode_err returns OK and
//         get_root_vnode() then equals the installed vnode.
// Depends: vfs::init_err, vfs::set_root_vnode_err, vfs::reset_and_remount
JARVIS_TEST(vfs_init_err_and_root_vnode_err, "PRE: vfsd, iocd | POST: none") {
    ensure_standard_mounts();

    VfsError init_err_code = vfs::init_err();
    vfs::Vnode *emptied_root = vfs::get_root_vnode();
    vfs::Filesystem *emptied_devfs = nullptr;
    VfsError emptied_devfs_err = vfs::find_fs_err("devfs", emptied_devfs);

    vfs::reset_and_remount();
    vfs::Vnode *restored_root = vfs::get_root_vnode();
    vfs::Filesystem *restored_devfs = nullptr;
    VfsError restored_devfs_err = vfs::find_fs_err("devfs", restored_devfs);

    vfs::Vnode *selected = vfs::dev_fs.get_root();
    VfsError set_err = vfs::set_root_vnode_err(*selected);
    vfs::Vnode *after_set = vfs::get_root_vnode();

    // Leave the global root on the standard initrd root.
    vfs::reset_and_remount();

    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_OK),
                     static_cast<uint64_t>(init_err_code));
    JARVIS_ASSERT(emptied_root == nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_NO_SUCH_FS),
                     static_cast<uint64_t>(emptied_devfs_err));
    JARVIS_ASSERT(restored_root != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_OK),
                     static_cast<uint64_t>(restored_devfs_err));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(VFS_ERR_OK),
                     static_cast<uint64_t>(set_err));
    JARVIS_ASSERT_EQ(selected, after_set);
    JARVIS_TEST_PASS();
}

void register_vfs_errors_tests() {
    Logger::info("Registering vfs error-code tests");
    JARVIS_REGISTER_TEST(vfs_fdtable_err_codes);
    JARVIS_REGISTER_TEST(vfs_resolve_err_codes);
    JARVIS_REGISTER_TEST(vfs_find_fs_err_codes);
    JARVIS_REGISTER_TEST(vfs_mount_err_codes);
    JARVIS_REGISTER_TEST(vfs_mkdir_create_unlink_err_codes);
    JARVIS_REGISTER_TEST(vfs_init_err_and_root_vnode_err);
}
