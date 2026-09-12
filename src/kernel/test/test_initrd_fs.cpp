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

/// @file test_initrd_fs.cpp
/// @brief initrd filesystem tests (milestone v0.4.10 issue #124):
///        src/kernel/vfs/initrd_fs.cpp was 1/19 covered — the whole
///        file-vnode layer (read/write/open/close/lseek/fstat/ioctl/
///        readdir/lookup) and the root readdir/lookup paths were dead.
///        Synthetic cpio-newc archives drive the real initrd_fs vnodes.
/// @note  Every test installs a synthetic archive, captures its results into
///        locals, RESTORES the boot initrd and only then asserts — an
///        assertion early-return can never leave a poisoned initrd behind.
///        Every lookup()-allocated vnode is released with ops->close() so the
///        ResourceTracker MemPool/vnode deltas stay at zero.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/initrd_fs.hpp>
#include <kernel/vfs/vfs.hpp>
#include <initrd/initrd.hpp>
#include <string.hpp>

using namespace kernel;

namespace {

/// @brief Boot-time initrd boundaries (linker symbols from the embedded
/// cpio object) — used to restore the real archive after each test.
extern "C" uint8_t _binary_initrd_cpio_start[];
extern "C" uint8_t _binary_initrd_cpio_end[];

constexpr size_t k_synth_cap = 4096;
uint8_t g_synth_archive[k_synth_cap];

/// @brief Minimal cpio-newc writer used to build synthetic archives.
struct CpioWriter {
    size_t pos = 0;
    bool overflowed = false;

    static void hex8(char *dst, uint32_t value) {
        static const char digits[] = "0123456789ABCDEF";
        for (int i = 7; i >= 0; --i) {
            dst[i] = digits[value & 0xF];
            value >>= 4;
        }
    }

    static uint32_t align4(uint32_t v) { return (v + 3) & ~3U; }

    void add(const char *name, const void *data, size_t size, uint32_t mode) {
        const size_t name_len = strlen(name) + 1;
        const size_t name_off = pos + 110;
        const size_t data_off =
            align4(static_cast<uint32_t>(name_off + name_len));
        const size_t end = align4(static_cast<uint32_t>(data_off + size));
        if (end > k_synth_cap) {
            overflowed = true;
            return;
        }

        char hdr[110];
        memset(hdr, '0', sizeof(hdr));
        memcpy(hdr, "070701", 6);
        hex8(hdr + 6, 0);
        hex8(hdr + 14, mode);
        hex8(hdr + 22, 0);
        hex8(hdr + 30, 0);
        hex8(hdr + 38, 1);
        hex8(hdr + 46, 0);
        hex8(hdr + 54, static_cast<uint32_t>(size));
        hex8(hdr + 62, 0);
        hex8(hdr + 70, 0);
        hex8(hdr + 78, 0);
        hex8(hdr + 86, 0);
        hex8(hdr + 94, static_cast<uint32_t>(name_len));
        hex8(hdr + 102, 0);
        memcpy(&g_synth_archive[pos], hdr, 110);
        memcpy(&g_synth_archive[name_off], name, name_len);
        for (size_t i = name_off + name_len; i < data_off; ++i)
            g_synth_archive[i] = 0;
        if (size && data)
            memcpy(&g_synth_archive[data_off], data, size);
        for (size_t i = data_off + size; i < end; ++i)
            g_synth_archive[i] = 0;
        pos = end;
    }

    void add_trailer() { add("TRAILER!!!", nullptr, 0, 0); }
};

void install_synth(const CpioWriter &writer) {
    JARVIS_ASSERT_FMT(!writer.overflowed, "synthetic archive overflow");
    initrd::init(g_synth_archive, g_synth_archive + writer.pos);
}

void restore_boot_initrd() {
    initrd::init(_binary_initrd_cpio_start, _binary_initrd_cpio_end);
}

} // namespace

// Runmode: kernel
// Testidea: The initrd root is a lazily initialised, stable directory vnode:
// repeated get_root() calls yield the same address, it is S_IFDIR with ino 0
// and no parent, it opens/closes cleanly and rejects read/write/ioctl —
// directories are not byte streams.
// Input: initrd_fs.get_root() twice; open(0); fstat; read; write; ioctl;
//        close.
// Expect: Same pointer; ino 0; S_IFDIR; open == 0; fstat size 0;
//         read/write/ioctl == VFS_INVALID.
// Depends: vfs::initrd_fs
JARVIS_TEST(initrd_fs_root_directory_contract, "PRE: iocd | POST: none") {
    vfs::Vnode *first = vfs::initrd_fs.get_root();
    vfs::Vnode *second = vfs::initrd_fs.get_root();

    bool name_ok = vfs::initrd_fs.name != nullptr &&
                   strcmp(vfs::initrd_fs.name, "initrd") == 0;

    JARVIS_ASSERT(first != nullptr);
    JARVIS_ASSERT_EQ(first, second);
    JARVIS_ASSERT(name_ok);
    JARVIS_ASSERT(first->ops != nullptr);

    int open_ret = first->ops->open(*first, 0);
    vfs::VfsStat stat = {};
    int fstat_ret = first->ops->fstat(*first, stat);

    uint8_t scratch[4] = {1, 2, 3, 4};
    int64_t read_ret = first->ops->read(*first, scratch, sizeof(scratch), 0);
    int64_t write_ret = first->ops->write(*first, scratch, sizeof(scratch), 0);

    kernel::CheckedPtr<uint8_t> ioctl_arg;
    int ioctl_ret = first->ops->ioctl(*first, 0x1234, ioctl_arg);

    first->ops->close(*first);

    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), first->ino);
    JARVIS_ASSERT((first->mode & vfs::S_IFDIR) != 0);
    JARVIS_ASSERT(first->parent == nullptr);
    JARVIS_ASSERT_EQ(0, open_ret);
    JARVIS_ASSERT_EQ(0, fstat_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), stat.st_size);
    JARVIS_ASSERT((stat.st_mode & vfs::S_IFDIR) != 0);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, read_ret);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, write_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), ioctl_ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The initrd root's lseek delegates to the file lseek, which has
// no per-file private data on the directory vnode — the delegation must
// fail closed with VFS_INVALID instead of dereferencing a null finfo.
// Input: lseek(root, 0, SEEK_SET, &pos) and lseek(root, 8, SEEK_END, &pos).
// Expect: Both return VFS_INVALID; the caller's position is untouched.
// Depends: vfs::initrd_fs
JARVIS_TEST(initrd_fs_root_lseek_fails_closed, "PRE: iocd | POST: none") {
    vfs::Vnode *root = vfs::initrd_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    JARVIS_ASSERT(root->private_data == nullptr);

    uint64_t pos = 42;
    int64_t set_ret = root->ops->lseek(*root, 0, vfs::SEEK_SET, &pos);
    int64_t end_ret = root->ops->lseek(*root, 8, vfs::SEEK_END, &pos);
    uint64_t untouched = pos;

    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, set_ret);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, end_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(42), untouched);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: initrd root readdir mirrors the archive enumeration verbatim —
// the archive-stored names are copied into the dirent with the fixed inode
// number 2, and enumeration stops (VFS_INVALID) once the archive is drained.
// Input: Synthetic archive with "./alpha" and "./beta" plus a trailer;
//        three successive readdir calls from pos 0.
// Expect: First two calls return 0 with names "./alpha"/"./beta" and
//         d_ino 2; the third returns VFS_INVALID.
// Depends: vfs::initrd_fs, initrd::readdir
JARVIS_TEST(initrd_fs_root_readdir_lists_archive,
            "PRE: iocd | POST: none") {
    CpioWriter writer;
    writer.add("./alpha", "AAA", 3, 0100644);
    writer.add("./beta", "BBBB", 4, 0100644);
    writer.add_trailer();
    install_synth(writer);

    vfs::Vnode *root = vfs::initrd_fs.get_root();
    vfs::Dirent first = {};
    vfs::Dirent second = {};
    vfs::Dirent third = {};
    uint64_t pos = 0;
    int first_ret = root->ops->readdir(*root, pos, first);
    int second_ret = root->ops->readdir(*root, pos, second);
    int third_ret = root->ops->readdir(*root, pos, third);
    uint64_t advanced = pos;
    restore_boot_initrd();

    JARVIS_ASSERT_EQ(0, first_ret);
    JARVIS_ASSERT_EQ(0, second_ret);
    JARVIS_ASSERT(memcmp(first.d_name, "./alpha", 8) == 0);
    JARVIS_ASSERT(memcmp(second.d_name, "./beta", 7) == 0);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2), first.d_ino);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2), second.d_ino);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), third_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2), advanced);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Root lookup turns an archive entry into a fully initialised file
// vnode — refcount 1, ino 1, S_IFREG, size equal to the archive entry size,
// the file ops table, and the initrd root as parent — while a missing name
// resolves to nullptr without allocating anything.
// Input: Synthetic archive with "./hello.txt" (5 bytes); lookup("hello.txt")
//        and lookup("absent.bin").
// Expect: hello.txt vnode has ino 1, S_IFREG, size 5, refcount 1,
//         parent == root, ops != nullptr; absent.bin == nullptr.
// Depends: vfs::initrd_fs, initrd::find, MemPool
JARVIS_TEST(initrd_fs_lookup_builds_file_vnode, "PRE: iocd | POST: none") {
    CpioWriter writer;
    writer.add("./hello.txt", "HELLO", 5, 0100644);
    writer.add_trailer();
    install_synth(writer);

    vfs::Vnode *root = vfs::initrd_fs.get_root();
    vfs::Vnode *file = root->ops->lookup(*root, "hello.txt");
    vfs::Vnode *absent = root->ops->lookup(*root, "absent.bin");

    uint64_t file_ino = 0;
    uint16_t file_mode = 0;
    uint64_t file_size = 0;
    uint64_t file_refs = 0;
    bool parented = false;
    bool has_ops = false;
    if (file) {
        file_ino = file->ino;
        file_mode = file->mode;
        file_size = file->size;
        file_refs = file->refcount;
        parented = (file->parent == root);
        has_ops = (file->ops != nullptr);
    }
    if (file)
        file->ops->close(*file);
    restore_boot_initrd();

    JARVIS_ASSERT(file != nullptr);
    JARVIS_ASSERT(absent == nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), file_ino);
    JARVIS_ASSERT((file_mode & vfs::S_IFREG) != 0);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(5), file_size);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), file_refs);
    JARVIS_ASSERT(parented);
    JARVIS_ASSERT(has_ops);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: An initrd file is a read-only byte source: open/close succeed,
// a full read returns the exact archive content, an oversized read is
// clamped to the remaining bytes, a read at or past EOF returns 0, and
// write/ioctl/readdir/lookup are rejected.
// Input: Archive "./hello.txt" = "HELLO"; open(0); read(0, 16); read(3, 16);
//        read(5, 16); write; fstat; ioctl; readdir; lookup; close.
// Expect: open == 0; read(0,16) == 5 with content "HELLO"; read(3,16) == 2
//         with content "LO"; read(5,16) == 0; write/ioctl/readdir
//         == VFS_INVALID; lookup == nullptr; fstat size 5 and S_IFREG.
// Depends: vfs::initrd_fs, MemPool
JARVIS_TEST(initrd_fs_file_readonly_contract, "PRE: iocd | POST: none") {
    CpioWriter writer;
    writer.add("./hello.txt", "HELLO", 5, 0100644);
    writer.add_trailer();
    install_synth(writer);

    vfs::Vnode *root = vfs::initrd_fs.get_root();
    vfs::Vnode *file = root->ops->lookup(*root, "hello.txt");
    JARVIS_ASSERT(file != nullptr);

    int open_ret = file->ops->open(*file, 0);

    uint8_t full[16] = {};
    int64_t full_ret = file->ops->read(*file, full, sizeof(full), 0);
    uint8_t tail[16] = {};
    int64_t tail_ret = file->ops->read(*file, tail, sizeof(tail), 3);
    uint8_t eof[16] = {};
    int64_t eof_ret = file->ops->read(*file, eof, sizeof(eof), 5);

    const uint8_t payload[4] = {'W', 'R', 'I', 'T'};
    int64_t write_ret = file->ops->write(*file, payload, sizeof(payload), 0);

    vfs::VfsStat stat = {};
    int fstat_ret = file->ops->fstat(*file, stat);

    kernel::CheckedPtr<uint8_t> ioctl_arg;
    int ioctl_ret = file->ops->ioctl(*file, 0x1234, ioctl_arg);

    uint64_t dir_pos = 0;
    vfs::Dirent dent = {};
    int readdir_ret = file->ops->readdir(*file, dir_pos, dent);
    vfs::Vnode *child = file->ops->lookup(*file, "sub");
    bool has_private = file->private_data != nullptr;

    file->ops->close(*file);
    restore_boot_initrd();

    JARVIS_ASSERT_EQ(0, open_ret);
    JARVIS_ASSERT(has_private);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(5), full_ret);
    JARVIS_ASSERT(memcmp(full, "HELLO", 5) == 0);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(2), tail_ret);
    JARVIS_ASSERT(memcmp(tail, "LO", 2) == 0);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(0), eof_ret);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, write_ret);
    JARVIS_ASSERT_EQ(0, fstat_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(5), stat.st_size);
    JARVIS_ASSERT((stat.st_mode & vfs::S_IFREG) != 0);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), ioctl_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), readdir_ret);
    JARVIS_ASSERT(child == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: initrd file lseek implements SEEK_SET, SEEK_CUR and SEEK_END,
// clamps any result beyond EOF to the file size, and rejects an unknown
// whence with VFS_INVALID.
// Input: Archive "./hello.txt" = "HELLO" (size 5); lseek SET 2, CUR +2 from
//        2, END 0, END 1 (past EOF), SET 99 (past EOF), invalid whence.
// Expect: 2, 4, 5, 5 (clamped), 5 (clamped), VFS_INVALID.
// Depends: vfs::initrd_fs, MemPool
JARVIS_TEST(initrd_fs_file_lseek_modes, "PRE: iocd | POST: none") {
    CpioWriter writer;
    writer.add("./hello.txt", "HELLO", 5, 0100644);
    writer.add_trailer();
    install_synth(writer);

    vfs::Vnode *root = vfs::initrd_fs.get_root();
    vfs::Vnode *file = root->ops->lookup(*root, "hello.txt");
    JARVIS_ASSERT(file != nullptr);

    uint64_t pos = 0;
    int64_t set_ret = file->ops->lseek(*file, 2, vfs::SEEK_SET, &pos);
    uint64_t after_set = pos;
    int64_t cur_ret = file->ops->lseek(*file, 2, vfs::SEEK_CUR, &pos);
    uint64_t after_cur = pos;
    int64_t end_ret = file->ops->lseek(*file, 0, vfs::SEEK_END, &pos);
    uint64_t after_end = pos;
    int64_t past_ret = file->ops->lseek(*file, 1, vfs::SEEK_END, &pos);
    uint64_t after_past = pos;
    int64_t clamp_ret = file->ops->lseek(*file, 99, vfs::SEEK_SET, &pos);
    uint64_t after_clamp = pos;
    int64_t bad_ret = file->ops->lseek(*file, 0, 99, &pos);

    file->ops->close(*file);
    restore_boot_initrd();

    JARVIS_ASSERT_EQ(static_cast<int64_t>(2), set_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2), after_set);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(4), cur_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(4), after_cur);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(5), end_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(5), after_end);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(5), past_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(5), after_past);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(5), clamp_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(5), after_clamp);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, bad_ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Closing an initrd file vnode releases both MemPool blocks
// acquired at lookup time (the InitrdFileNode private data and the vnode
// itself) — repeated lookup/close cycles are ResourceTracker-clean, and the
// second file is still resolvable after the first one was closed.
// Input: Archive with "./one" and "./two"; lookup both, close both.
// Expect: Both lookups succeed with distinct vnodes and distinct private
//         data; both closes complete; no leak (checked by the isolation
//         snapshot).
// Depends: vfs::initrd_fs, MemPool, ResourceTracker
JARVIS_TEST(initrd_fs_file_close_releases_blocks, "PRE: iocd | POST: none") {
    CpioWriter writer;
    writer.add("./one", "1111", 4, 0100644);
    writer.add("./two", "22", 2, 0100644);
    writer.add_trailer();
    install_synth(writer);

    vfs::Vnode *root = vfs::initrd_fs.get_root();
    vfs::Vnode *first = root->ops->lookup(*root, "one");
    vfs::Vnode *second = root->ops->lookup(*root, "two");

    bool distinct = (first != nullptr) && (second != nullptr) &&
                    (first != second) &&
                    (first->private_data != second->private_data);
    uint64_t first_size = first ? first->size : 0;
    uint64_t second_size = second ? second->size : 0;

    if (first)
        first->ops->close(*first);
    if (second)
        second->ops->close(*second);
    restore_boot_initrd();

    JARVIS_ASSERT(first != nullptr);
    JARVIS_ASSERT(second != nullptr);
    JARVIS_ASSERT(distinct);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(4), first_size);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2), second_size);
    JARVIS_TEST_PASS();
}

void register_initrd_fs_tests() {
    Logger::info("Registering initrd fs tests");
    JARVIS_REGISTER_TEST(initrd_fs_root_directory_contract);
    JARVIS_REGISTER_TEST(initrd_fs_root_lseek_fails_closed);
    JARVIS_REGISTER_TEST(initrd_fs_root_readdir_lists_archive);
    JARVIS_REGISTER_TEST(initrd_fs_lookup_builds_file_vnode);
    JARVIS_REGISTER_TEST(initrd_fs_file_readonly_contract);
    JARVIS_REGISTER_TEST(initrd_fs_file_lseek_modes);
    JARVIS_REGISTER_TEST(initrd_fs_file_close_releases_blocks);
}
