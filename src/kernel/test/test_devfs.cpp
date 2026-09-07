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

/// @file test_devfs.cpp
/// @brief devfs tests (milestone v0.4.10 issue #124): the /dev filesystem is
///        the single biggest coverage hole in kernel/vfs (14/57).  No test
///        ever opened a devfs node, so every device vnode operation
///        (open/close/read/write/ioctl/lseek/fstat/readdir/lookup) for
///        tty, null, console, kbd, random and the devfs root was dead code.
/// @note  Blocking character devices (tty, kbd) are only ever read with
///        count == 0 (documented early return) or after an O_NONBLOCK open,
///        so no test can ever wedge the scheduler in the blocking loop.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/devfs.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/arch/keyboard.hpp>
#include <string.hpp>

using namespace kernel;

namespace {

/// @brief Non-blocking open flag (mirrors Vfs OpenFlags::O_NONBLOCK).
constexpr uint64_t k_nonblock_flags = static_cast<uint64_t>(vfs::O_NONBLOCK);

/// @brief Devices published by devfs, in enumeration order.
struct DevExpectation {
    const char *name;
    uint64_t ino;
};

constexpr DevExpectation k_devices[5] = {
    {"tty", 1}, {"null", 2}, {"console", 3}, {"kbd", 4}, {"random", 5},
};

constexpr size_t k_device_count = sizeof(k_devices) / sizeof(k_devices[0]);

} // namespace

// Runmode: kernel
// Testidea: devfs publishes itself as a filesystem named "devfs" whose root
// vnode is stable across calls, is a directory (S_IFDIR, ino 0) and carries
// a complete ops table.
// Input: dev_fs.get_root() twice; inspect name/ino/mode/ops.
// Expect: Both calls return the same pointer; name == "devfs"; ino == 0;
//         mode has S_IFDIR; ops non-null.
// Depends: vfs::dev_fs
JARVIS_TEST(devfs_root_is_stable_directory, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *first = vfs::dev_fs.get_root();
    vfs::Vnode *second = vfs::dev_fs.get_root();

    bool name_ok = vfs::dev_fs.name != nullptr &&
                   strcmp(vfs::dev_fs.name, "devfs") == 0;

    JARVIS_ASSERT(first != nullptr);
    JARVIS_ASSERT_EQ(first, second);
    JARVIS_ASSERT(name_ok);
    JARVIS_ASSERT(first->ops != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), first->ino);
    JARVIS_ASSERT((first->mode & vfs::S_IFDIR) != 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The devfs root accepts open/close, reports S_IFDIR with size 0
// through fstat, and rejects every byte-oriented operation (read, write,
// lseek, ioctl) with the VFS_INVALID sentinel — a directory is neither
// readable nor writable nor seekable.
// Input: root open(0), fstat, read, write, lseek(SEEK_SET), ioctl, close.
// Expect: open == 0; fstat == 0 with S_IFDIR and size 0; read/write/lseek/
//         ioctl all == VFS_INVALID; close completes.
// Depends: vfs::dev_fs
JARVIS_TEST(devfs_root_rejects_byte_ops, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::dev_fs.get_root();
    JARVIS_ASSERT(root != nullptr);

    int open_ret = root->ops->open(*root, 0);
    vfs::VfsStat stat = {};
    int fstat_ret = root->ops->fstat(*root, stat);

    uint8_t scratch[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    int64_t read_ret = root->ops->read(*root, scratch, sizeof(scratch), 0);
    int64_t write_ret = root->ops->write(*root, scratch, sizeof(scratch), 0);

    uint64_t seek_pos = 0;
    int64_t lseek_ret = root->ops->lseek(*root, 0, vfs::SEEK_SET, &seek_pos);

    kernel::CheckedPtr<uint8_t> ioctl_arg;
    int ioctl_ret = root->ops->ioctl(*root, 0x1234, ioctl_arg);

    root->ops->close(*root);

    JARVIS_ASSERT_EQ(0, open_ret);
    JARVIS_ASSERT_EQ(0, fstat_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), stat.st_size);
    JARVIS_ASSERT((stat.st_mode & vfs::S_IFDIR) != 0);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, read_ret);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, write_ret);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, lseek_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), ioctl_ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: readdir enumerates exactly the five registered device nodes in
// the documented order with their documented inode numbers, advances the
// cursor on every call, and terminates with VFS_INVALID past the last node.
// Input: Five successive readdir calls from pos 0, then a sixth.
// Expect: Names/inodes match tty(1), null(2), console(3), kbd(4), random(5);
//         pos advances from 0 to 5; the sixth call returns VFS_INVALID.
// Depends: vfs::dev_fs
JARVIS_TEST(devfs_root_readdir_enumerates_devices,
            "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::dev_fs.get_root();
    JARVIS_ASSERT(root != nullptr);

    vfs::Dirent entries[k_device_count] = {};
    uint64_t pos = 0;
    int results[k_device_count] = {};
    for (size_t idx = 0; idx < k_device_count; ++idx) {
        results[idx] = root->ops->readdir(*root, pos, entries[idx]);
    }
    vfs::Dirent overflow = {};
    int overflow_ret = root->ops->readdir(*root, pos, overflow);

    for (size_t idx = 0; idx < k_device_count; ++idx) {
        JARVIS_ASSERT_EQ(0, results[idx]);
        JARVIS_ASSERT(memcmp(entries[idx].d_name, k_devices[idx].name,
                             strlen(k_devices[idx].name) + 1) == 0);
        JARVIS_ASSERT_EQ(k_devices[idx].ino, entries[idx].d_ino);
    }
    JARVIS_ASSERT_EQ(k_device_count, static_cast<size_t>(pos));
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), overflow_ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: dev_root_lookup resolves every registered device name to the
// matching vnode (same inode as reported by readdir) and rejects unknown and
// empty names with nullptr — path resolution under /dev never invents nodes.
// Input: lookup for all five device names, plus "nosuchdev", "" and "TTY".
// Expect: Known names resolve to non-null vnodes with the expected inode;
//         unknown/empty/case-mismatched names return nullptr.
// Depends: vfs::dev_fs
JARVIS_TEST(devfs_root_lookup_resolves_devices,
            "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::dev_fs.get_root();
    JARVIS_ASSERT(root != nullptr);

    vfs::Vnode *resolved[k_device_count] = {};
    for (size_t idx = 0; idx < k_device_count; ++idx) {
        resolved[idx] = root->ops->lookup(*root, k_devices[idx].name);
    }
    vfs::Vnode *unknown = root->ops->lookup(*root, "nosuchdev");
    vfs::Vnode *empty = root->ops->lookup(*root, "");
    vfs::Vnode *wrong_case = root->ops->lookup(*root, "TTY");

    for (size_t idx = 0; idx < k_device_count; ++idx) {
        JARVIS_ASSERT(resolved[idx] != nullptr);
        JARVIS_ASSERT_EQ(k_devices[idx].ino, resolved[idx]->ino);
        JARVIS_ASSERT(resolved[idx]->ops != nullptr);
    }
    JARVIS_ASSERT(unknown == nullptr);
    JARVIS_ASSERT(empty == nullptr);
    JARVIS_ASSERT(wrong_case == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: /dev/null is a character device that reads as EOF, swallows
// writes reporting the full count, is seekable (SEEK_SET/SEEK_CUR/SEEK_END
// all report and store the resulting position) and rejects ioctl/readdir/
// lookup.
// Input: open(0), read(4), write(4), fstat, lseek SET/CUR/END, ioctl,
//        readdir, lookup, close.
// Expect: read == 0; write == 4; fstat S_IFCHR size 0; SEEK_SET 16 -> 16;
//         SEEK_CUR +8 -> 24; SEEK_END 4 -> 4; ioctl/readdir VFS_INVALID;
//         lookup nullptr.
// Depends: vfs::dev_fs
JARVIS_TEST(devfs_null_sink_contract, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::dev_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    vfs::Vnode *null_dev = root->ops->lookup(*root, "null");
    JARVIS_ASSERT(null_dev != nullptr);

    int open_ret = null_dev->ops->open(*null_dev, 0);

    uint8_t scratch[4] = {1, 2, 3, 4};
    int64_t read_ret = null_dev->ops->read(*null_dev, scratch, sizeof(scratch), 0);
    int64_t write_ret =
        null_dev->ops->write(*null_dev, scratch, sizeof(scratch), 0);

    vfs::VfsStat stat = {};
    int fstat_ret = null_dev->ops->fstat(*null_dev, stat);

    uint64_t pos = 0;
    int64_t set_ret = null_dev->ops->lseek(*null_dev, 16, vfs::SEEK_SET, &pos);
    uint64_t after_set = pos;
    int64_t cur_ret = null_dev->ops->lseek(*null_dev, 8, vfs::SEEK_CUR, &pos);
    uint64_t after_cur = pos;
    int64_t end_ret = null_dev->ops->lseek(*null_dev, 4, vfs::SEEK_END, &pos);
    uint64_t after_end = pos;

    kernel::CheckedPtr<uint8_t> ioctl_arg;
    int ioctl_ret = null_dev->ops->ioctl(*null_dev, 0, ioctl_arg);
    uint64_t dir_pos = 0;
    vfs::Dirent dent = {};
    int readdir_ret = null_dev->ops->readdir(*null_dev, dir_pos, dent);
    vfs::Vnode *child = null_dev->ops->lookup(*null_dev, "sub");
    null_dev->ops->close(*null_dev);

    JARVIS_ASSERT_EQ(0, open_ret);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(0), read_ret);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(4), write_ret);
    JARVIS_ASSERT_EQ(0, fstat_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), stat.st_size);
    JARVIS_ASSERT((stat.st_mode & vfs::S_IFCHR) != 0);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(16), set_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(16), after_set);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(24), cur_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(24), after_cur);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(4), end_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(4), after_end);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), ioctl_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), readdir_ret);
    JARVIS_ASSERT(child == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: /dev/console is a write-only character device: it reports the
// full byte count on write, refuses reads, and rejects lseek/ioctl/readdir/
// lookup — it is a byte sink, not a seekable object.
// Input: open(0), read(4), write(3), fstat, lseek, ioctl, readdir, lookup,
//        close.
// Expect: open == 0; read == VFS_INVALID; write == 3; fstat S_IFCHR size 0;
//         lseek/ioctl/readdir == VFS_INVALID; lookup == nullptr.
// Depends: vfs::dev_fs
JARVIS_TEST(devfs_console_write_only_contract,
            "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::dev_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    vfs::Vnode *console = root->ops->lookup(*root, "console");
    JARVIS_ASSERT(console != nullptr);

    int open_ret = console->ops->open(*console, 0);

    uint8_t scratch[4] = {};
    int64_t read_ret = console->ops->read(*console, scratch, sizeof(scratch), 0);
    int64_t write_ret = console->ops->write(*console,
                                           reinterpret_cast<const uint8_t *>(
                                               "ok\n"),
                                           3, 0);

    vfs::VfsStat stat = {};
    int fstat_ret = console->ops->fstat(*console, stat);

    uint64_t pos = 0;
    int64_t lseek_ret = console->ops->lseek(*console, 0, vfs::SEEK_SET, &pos);

    kernel::CheckedPtr<uint8_t> ioctl_arg;
    int ioctl_ret = console->ops->ioctl(*console, 0x5401, ioctl_arg);
    uint64_t dir_pos = 0;
    vfs::Dirent dent = {};
    int readdir_ret = console->ops->readdir(*console, dir_pos, dent);
    vfs::Vnode *child = console->ops->lookup(*console, "sub");
    console->ops->close(*console);

    JARVIS_ASSERT_EQ(0, open_ret);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, read_ret);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(3), write_ret);
    JARVIS_ASSERT_EQ(0, fstat_ret);
    JARVIS_ASSERT((stat.st_mode & vfs::S_IFCHR) != 0);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), stat.st_size);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, lseek_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), ioctl_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), readdir_ret);
    JARVIS_ASSERT(child == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: /dev/tty is a serial/console character device.  With an
// O_NONBLOCK open the read path can never block: a zero-length read returns
// 0 without touching the device, and a one-byte read either delivers one
// pending input character or fails with VFS_INVALID — it never deschedules.
// Writes report the full count.  lseek/ioctl/readdir/lookup are rejected.
// Input: open(O_NONBLOCK); read(count=0); read(count=1); write(3); fstat;
//        lseek; ioctl; readdir; lookup; close; private_data restored.
// Expect: open == 0; read(0) == 0; read(1) in {1, VFS_INVALID}; write == 3;
//         fstat S_IFCHR; lseek/ioctl/readdir == VFS_INVALID; lookup nullptr;
//         private_data restored to null after close.
// Depends: vfs::dev_fs
JARVIS_TEST(devfs_tty_nonblock_contract, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::dev_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    vfs::Vnode *tty = root->ops->lookup(*root, "tty");
    JARVIS_ASSERT(tty != nullptr);

    int open_ret = tty->ops->open(*tty, k_nonblock_flags);
    bool nonblock_armed = tty->private_data != nullptr;

    uint8_t scratch[4] = {};
    int64_t zero_read = tty->ops->read(*tty, scratch, 0, 0);
    int64_t one_read = tty->ops->read(*tty, scratch, 1, 0);
    bool one_read_bounded = (one_read == 1) || (one_read == vfs::VFS_INVALID);

    int64_t write_ret =
        tty->ops->write(*tty, reinterpret_cast<const uint8_t *>("ok\n"), 3, 0);

    vfs::VfsStat stat = {};
    int fstat_ret = tty->ops->fstat(*tty, stat);

    uint64_t pos = 0;
    int64_t lseek_ret = tty->ops->lseek(*tty, 0, vfs::SEEK_SET, &pos);

    kernel::CheckedPtr<uint8_t> ioctl_arg;
    int ioctl_ret = tty->ops->ioctl(*tty, 0x5401, ioctl_arg);
    uint64_t dir_pos = 0;
    vfs::Dirent dent = {};
    int readdir_ret = tty->ops->readdir(*tty, dir_pos, dent);
    vfs::Vnode *child = tty->ops->lookup(*tty, "sub");
    tty->ops->close(*tty);
    tty->private_data = nullptr;

    JARVIS_ASSERT_EQ(0, open_ret);
    JARVIS_ASSERT(nonblock_armed);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(0), zero_read);
    JARVIS_ASSERT(one_read_bounded);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(3), write_ret);
    JARVIS_ASSERT_EQ(0, fstat_ret);
    JARVIS_ASSERT((stat.st_mode & vfs::S_IFCHR) != 0);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, lseek_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), ioctl_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), readdir_ret);
    JARVIS_ASSERT(child == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: /dev/kbd is a read-only keyboard character device.  With an
// O_NONBLOCK open a zero-length read returns 0 and a one-byte read either
// yields a decoded key or fails with VFS_INVALID; writes are rejected
// outright.  lseek/ioctl/readdir/lookup are rejected.
// Input: open(O_NONBLOCK); read(count=0); read(count=1); write(3); fstat;
//        lseek; ioctl; readdir; lookup; close; private_data restored.
// Expect: open == 0; read(0) == 0; read(1) in {1, VFS_INVALID};
//         write == VFS_INVALID; fstat S_IFCHR; lseek/ioctl/readdir
//         VFS_INVALID; lookup nullptr.
// Depends: vfs::dev_fs
JARVIS_TEST(devfs_kbd_read_only_contract, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::dev_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    vfs::Vnode *kbd = root->ops->lookup(*root, "kbd");
    JARVIS_ASSERT(kbd != nullptr);

    int open_ret = kbd->ops->open(*kbd, k_nonblock_flags);
    bool nonblock_armed = kbd->private_data != nullptr;

    uint8_t scratch[4] = {};
    int64_t zero_read = kbd->ops->read(*kbd, scratch, 0, 0);
    int64_t one_read = kbd->ops->read(*kbd, scratch, 1, 0);
    bool one_read_bounded = (one_read == 1) || (one_read == vfs::VFS_INVALID);

    int64_t write_ret =
        kbd->ops->write(*kbd, reinterpret_cast<const uint8_t *>("ok\n"), 3, 0);

    vfs::VfsStat stat = {};
    int fstat_ret = kbd->ops->fstat(*kbd, stat);

    uint64_t pos = 0;
    int64_t lseek_ret = kbd->ops->lseek(*kbd, 0, vfs::SEEK_SET, &pos);

    kernel::CheckedPtr<uint8_t> ioctl_arg;
    int ioctl_ret = kbd->ops->ioctl(*kbd, 0x5401, ioctl_arg);
    uint64_t dir_pos = 0;
    vfs::Dirent dent = {};
    int readdir_ret = kbd->ops->readdir(*kbd, dir_pos, dent);
    vfs::Vnode *child = kbd->ops->lookup(*kbd, "sub");
    kbd->ops->close(*kbd);
    kbd->private_data = nullptr;

    JARVIS_ASSERT_EQ(0, open_ret);
    JARVIS_ASSERT(nonblock_armed);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(0), zero_read);
    JARVIS_ASSERT(one_read_bounded);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, write_ret);
    JARVIS_ASSERT_EQ(0, fstat_ret);
    JARVIS_ASSERT((stat.st_mode & vfs::S_IFCHR) != 0);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, lseek_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), ioctl_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), readdir_ret);
    JARVIS_ASSERT(child == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: /dev/random delivers as many bytes as requested and its output
// is not a constant stream (two successive fills differ), accepts writes
// (re-seeding path, count reported), and rejects lseek/ioctl/readdir/lookup.
// Input: open(0); two 32-byte reads; write(4); fstat; lseek; ioctl;
//        readdir; lookup; close.
// Expect: Both reads return 32 and the two buffers differ; write == 4;
//         fstat S_IFCHR; lseek/ioctl/readdir VFS_INVALID; lookup nullptr.
// Depends: vfs::dev_fs, kernel::random_fill
JARVIS_TEST(devfs_random_fill_contract, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::dev_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    vfs::Vnode *random_dev = root->ops->lookup(*root, "random");
    JARVIS_ASSERT(random_dev != nullptr);

    int open_ret = random_dev->ops->open(*random_dev, 0);

    uint8_t first[32] = {};
    uint8_t second[32] = {};
    int64_t first_ret = random_dev->ops->read(*random_dev, first,
                                              sizeof(first), 0);
    int64_t second_ret = random_dev->ops->read(*random_dev, second,
                                               sizeof(second), 0);
    bool differ = memcmp(first, second, sizeof(first)) != 0;

    uint8_t seed[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    int64_t write_ret =
        random_dev->ops->write(*random_dev, seed, sizeof(seed), 0);

    vfs::VfsStat stat = {};
    int fstat_ret = random_dev->ops->fstat(*random_dev, stat);

    uint64_t pos = 0;
    int64_t lseek_ret = random_dev->ops->lseek(*random_dev, 0, vfs::SEEK_SET,
                                               &pos);

    kernel::CheckedPtr<uint8_t> ioctl_arg;
    int ioctl_ret = random_dev->ops->ioctl(*random_dev, 0, ioctl_arg);
    uint64_t dir_pos = 0;
    vfs::Dirent dent = {};
    int readdir_ret = random_dev->ops->readdir(*random_dev, dir_pos, dent);
    vfs::Vnode *child = random_dev->ops->lookup(*random_dev, "sub");
    random_dev->ops->close(*random_dev);

    JARVIS_ASSERT_EQ(0, open_ret);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(32), first_ret);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(32), second_ret);
    JARVIS_ASSERT(differ);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(4), write_ret);
    JARVIS_ASSERT_EQ(0, fstat_ret);
    JARVIS_ASSERT((stat.st_mode & vfs::S_IFCHR) != 0);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, lseek_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), ioctl_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), readdir_ret);
    JARVIS_ASSERT(child == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: devfs_init() flushes the keyboard buffer so a freshly
// initialised devfs never serves stale keystrokes: after the call the
// keyboard has no pending character.
// Input: devfs_init(); Keyboard::getchar probe.
// Expect: devfs_init completes; the keyboard reports no pending character
//         immediately afterwards.
// Depends: vfs::devfs_init, arch::Keyboard
JARVIS_TEST(devfs_init_flushes_keyboard, "PRE: vfsd, iocd | POST: none") {
    vfs::devfs_init();
    char pending = 0;
    bool has_pending = arch::Keyboard::getchar(pending);
    JARVIS_ASSERT(!has_pending);
    JARVIS_TEST_PASS();
}

void register_devfs_tests() {
    Logger::info("Registering devfs tests");
    JARVIS_REGISTER_TEST(devfs_root_is_stable_directory);
    JARVIS_REGISTER_TEST(devfs_root_rejects_byte_ops);
    JARVIS_REGISTER_TEST(devfs_root_readdir_enumerates_devices);
    JARVIS_REGISTER_TEST(devfs_root_lookup_resolves_devices);
    JARVIS_REGISTER_TEST(devfs_null_sink_contract);
    JARVIS_REGISTER_TEST(devfs_console_write_only_contract);
    JARVIS_REGISTER_TEST(devfs_tty_nonblock_contract);
    JARVIS_REGISTER_TEST(devfs_kbd_read_only_contract);
    JARVIS_REGISTER_TEST(devfs_random_fill_contract);
    JARVIS_REGISTER_TEST(devfs_init_flushes_keyboard);
}
