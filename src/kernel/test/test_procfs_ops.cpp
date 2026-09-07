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

/// @file test_procfs_ops.cpp
/// @brief procfs operation-contract tests (milestone v0.4.10 issue #124).
///        test_vfs_procfs.cpp covers the *content* paths (read/fstat/
///        readdir/lookup); the remaining 36 functions — every open/close,
///        every not-supported write/ioctl/readdir/lookup and the per-node
///        lseek implementations — were never entered.  This class drives
///        each of them on the real proc_fs vnodes.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/procfs.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/task/scheduler.hpp>
#include <string.hpp>

using namespace kernel;

namespace {

/// @brief An ioctl request number that no procfs node implements.
constexpr uint64_t k_unsupported_request = 0xDEADBEEF;

/// @brief A whence value outside SEEK_SET/SEEK_CUR/SEEK_END.
constexpr int k_invalid_whence = 99;

} // namespace

// Runmode: kernel
// Testidea: The procfs root opens and closes cleanly and rejects every
// byte-oriented or control operation — write, lseek and ioctl all report
// VFS_INVALID because /proc is a directory namespace, not a byte stream.
// Input: proc_fs.get_root(); open(0); write(4); lseek(SEEK_SET); ioctl;
//        close.
// Expect: open == 0; write/lseek == VFS_INVALID; ioctl == VFS_INVALID;
//         close completes.
// Depends: vfs::proc_fs
JARVIS_TEST(procfs_ops_root_rejects_byte_ops,
            "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::proc_fs.get_root();
    JARVIS_ASSERT(root != nullptr);

    int open_ret = root->ops->open(*root, 0);

    const uint8_t payload[4] = {'x', 'y', 'z', '\n'};
    int64_t write_ret = root->ops->write(*root, payload, sizeof(payload), 0);

    uint64_t seek_pos = 7;
    int64_t lseek_ret = root->ops->lseek(*root, 0, vfs::SEEK_SET, &seek_pos);

    kernel::CheckedPtr<uint8_t> ioctl_arg;
    int ioctl_ret = root->ops->ioctl(*root, k_unsupported_request, ioctl_arg);

    root->ops->close(*root);

    JARVIS_ASSERT_EQ(0, open_ret);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, write_ret);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, lseek_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), ioctl_ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: /proc/meminfo is a read-only regular file: open/close succeed,
// ioctl/readdir/lookup are rejected as not-supported, reading at or past the
// cached content length returns 0 (EOF), and lseek implements all three
// whence modes with clamping to the content length.
// Input: lookup("meminfo"); open(0); ioctl; readdir; lookup("sub");
//        read(offset=content_len); lseek SEEK_SET 4 / SEEK_CUR +2 /
//        SEEK_END 0 / invalid whence; close.
// Expect: open == 0; ioctl/readdir == VFS_INVALID; lookup == nullptr;
//         EOF read == 0; SEEK_SET -> 4; SEEK_CUR -> 6; SEEK_END -> len;
//         invalid whence -> VFS_INVALID; fstat size == len.
// Depends: vfs::proc_fs
JARVIS_TEST(procfs_ops_meminfo_readonly_contract,
            "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::proc_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    vfs::Vnode *meminfo = root->ops->lookup(*root, "meminfo");
    JARVIS_ASSERT(meminfo != nullptr);

    int open_ret = meminfo->ops->open(*meminfo, 0);

    vfs::VfsStat stat = {};
    int fstat_ret = meminfo->ops->fstat(*meminfo, stat);

    uint8_t eof_buf[4] = {};
    int64_t eof_read = meminfo->ops->read(*meminfo, eof_buf, sizeof(eof_buf),
                                          stat.st_size);

    kernel::CheckedPtr<uint8_t> ioctl_arg;
    int ioctl_ret = meminfo->ops->ioctl(*meminfo, k_unsupported_request,
                                        ioctl_arg);

    uint64_t dir_pos = 0;
    vfs::Dirent dent = {};
    int readdir_ret = meminfo->ops->readdir(*meminfo, dir_pos, dent);
    vfs::Vnode *child = meminfo->ops->lookup(*meminfo, "sub");

    uint64_t pos = 0;
    int64_t set_ret = meminfo->ops->lseek(*meminfo, 4, vfs::SEEK_SET, &pos);
    uint64_t after_set = pos;
    int64_t cur_ret = meminfo->ops->lseek(*meminfo, 2, vfs::SEEK_CUR, &pos);
    uint64_t after_cur = pos;
    int64_t end_ret = meminfo->ops->lseek(*meminfo, 0, vfs::SEEK_END, &pos);
    uint64_t after_end = pos;
    int64_t bad_ret = meminfo->ops->lseek(*meminfo, 0, k_invalid_whence, &pos);

    meminfo->ops->close(*meminfo);

    JARVIS_ASSERT_EQ(0, open_ret);
    JARVIS_ASSERT_EQ(0, fstat_ret);
    JARVIS_ASSERT(stat.st_size > 0);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(0), eof_read);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), ioctl_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), readdir_ret);
    JARVIS_ASSERT(child == nullptr);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(4), set_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(4), after_set);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(6), cur_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(6), after_cur);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(stat.st_size), end_ret);
    JARVIS_ASSERT_EQ(stat.st_size, after_end);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, bad_ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: /proc/pci mirrors the meminfo contract for a much larger cached
// buffer: open/close succeed, write/ioctl/readdir/lookup are rejected, and
// lseek honours all three whence modes against the PCI tree length.
// Input: lookup("pci"); open(0); write(4); ioctl; readdir; lookup("sub");
//        lseek SEEK_SET/CUR/END/invalid; close.
// Expect: open == 0; write/ioctl/readdir == VFS_INVALID; lookup == nullptr;
//         SEEK_SET 8 -> 8; SEEK_CUR +4 -> 12; SEEK_END -> size;
//         invalid whence -> VFS_INVALID.
// Depends: vfs::proc_fs
JARVIS_TEST(procfs_ops_pci_readonly_contract,
            "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::proc_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    vfs::Vnode *pci = root->ops->lookup(*root, "pci");
    JARVIS_ASSERT(pci != nullptr);

    int open_ret = pci->ops->open(*pci, 0);

    vfs::VfsStat stat = {};
    int fstat_ret = pci->ops->fstat(*pci, stat);

    const uint8_t payload[4] = {'x', 'y', 'z', '\n'};
    int64_t write_ret = pci->ops->write(*pci, payload, sizeof(payload), 0);

    kernel::CheckedPtr<uint8_t> ioctl_arg;
    int ioctl_ret = pci->ops->ioctl(*pci, k_unsupported_request, ioctl_arg);

    uint64_t dir_pos = 0;
    vfs::Dirent dent = {};
    int readdir_ret = pci->ops->readdir(*pci, dir_pos, dent);
    vfs::Vnode *child = pci->ops->lookup(*pci, "sub");

    uint64_t pos = 0;
    int64_t set_ret = pci->ops->lseek(*pci, 8, vfs::SEEK_SET, &pos);
    uint64_t after_set = pos;
    int64_t cur_ret = pci->ops->lseek(*pci, 4, vfs::SEEK_CUR, &pos);
    uint64_t after_cur = pos;
    int64_t end_ret = pci->ops->lseek(*pci, 0, vfs::SEEK_END, &pos);
    uint64_t after_end = pos;
    int64_t bad_ret = pci->ops->lseek(*pci, 0, k_invalid_whence, &pos);

    pci->ops->close(*pci);

    JARVIS_ASSERT_EQ(0, open_ret);
    JARVIS_ASSERT_EQ(0, fstat_ret);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, write_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), ioctl_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), readdir_ret);
    JARVIS_ASSERT(child == nullptr);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(8), set_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(8), after_set);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(12), cur_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(12), after_cur);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(stat.st_size), end_ret);
    JARVIS_ASSERT_EQ(stat.st_size, after_end);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, bad_ret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: /proc/self is a per-task directory alias: open/close succeed,
// write/lseek/ioctl/readdir are rejected, an unknown child name resolves to
// nullptr and "stat" resolves to the current task's live stat vnode.
// Input: lookup("self"); open(0); write; lseek; ioctl; readdir;
//        lookup("bogus"); lookup("stat"); close.
// Expect: open == 0; write/lseek/ioctl/readdir == VFS_INVALID;
//         lookup("bogus") == nullptr; lookup("stat") != nullptr and its
//         mode is S_IFREG.
// Depends: vfs::proc_fs, Scheduler::current_task
JARVIS_TEST(procfs_ops_self_directory_contract,
            "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::proc_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    vfs::Vnode *self = root->ops->lookup(*root, "self");
    JARVIS_ASSERT(self != nullptr);

    int open_ret = self->ops->open(*self, 0);

    const uint8_t payload[4] = {'x', 'y', 'z', '\n'};
    int64_t write_ret = self->ops->write(*self, payload, sizeof(payload), 0);

    uint64_t seek_pos = 0;
    int64_t lseek_ret = self->ops->lseek(*self, 0, vfs::SEEK_SET, &seek_pos);

    kernel::CheckedPtr<uint8_t> ioctl_arg;
    int ioctl_ret = self->ops->ioctl(*self, k_unsupported_request, ioctl_arg);

    uint64_t dir_pos = 0;
    vfs::Dirent dent = {};
    int readdir_ret = self->ops->readdir(*self, dir_pos, dent);

    vfs::Vnode *bogus = self->ops->lookup(*self, "bogus");
    vfs::Vnode *stat = self->ops->lookup(*self, "stat");

    self->ops->close(*self);

    JARVIS_ASSERT_EQ(0, open_ret);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, write_ret);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, lseek_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), ioctl_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), readdir_ret);
    JARVIS_ASSERT(bogus == nullptr);
    JARVIS_ASSERT(stat != nullptr);
    JARVIS_ASSERT((stat->mode & vfs::S_IFREG) != 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A numeric PID directory is a namespace, not a file: open/close
// succeed, read/write/lseek/ioctl/readdir are rejected, only "stat" resolves
// and the close path releases the MemPool block allocated at lookup time
// (ResourceTracker-clean teardown).
// Input: lookup("1"); open(0); read; write; lseek; ioctl; readdir;
//        lookup("stat"); lookup("other"); close.
// Expect: open == 0; read/write/lseek/ioctl/readdir == VFS_INVALID;
//         lookup("stat") != nullptr; lookup("other") == nullptr;
//         close completes.
// Depends: vfs::proc_fs, MemPool
JARVIS_TEST(procfs_ops_pid_dir_namespace_contract,
            "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::proc_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    vfs::Vnode *piddir = root->ops->lookup(*root, "1");
    JARVIS_ASSERT(piddir != nullptr);
    JARVIS_ASSERT((piddir->mode & vfs::S_IFDIR) != 0);

    int open_ret = piddir->ops->open(*piddir, 0);

    uint8_t scratch[4] = {};
    int64_t read_ret = piddir->ops->read(*piddir, scratch, sizeof(scratch), 0);
    int64_t write_ret =
        piddir->ops->write(*piddir, scratch, sizeof(scratch), 0);

    uint64_t seek_pos = 0;
    int64_t lseek_ret = piddir->ops->lseek(*piddir, 0, vfs::SEEK_SET,
                                           &seek_pos);

    kernel::CheckedPtr<uint8_t> ioctl_arg;
    int ioctl_ret = piddir->ops->ioctl(*piddir, k_unsupported_request,
                                       ioctl_arg);

    uint64_t dir_pos = 0;
    vfs::Dirent dent = {};
    int readdir_ret = piddir->ops->readdir(*piddir, dir_pos, dent);

    vfs::Vnode *stat = piddir->ops->lookup(*piddir, "stat");
    vfs::Vnode *other = piddir->ops->lookup(*piddir, "other");

    piddir->ops->close(*piddir);

    JARVIS_ASSERT_EQ(0, open_ret);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, read_ret);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, write_ret);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, lseek_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), ioctl_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), readdir_ret);
    JARVIS_ASSERT(stat != nullptr);
    JARVIS_ASSERT(stat->ops != nullptr);
    JARVIS_ASSERT(other == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The per-task "stat" file is a generated read-only file:
// open/close succeed, write/ioctl/readdir/lookup are rejected, reading at or
// past the generated length returns 0, and lseek implements SEEK_SET/SEEK_CUR
// with the documented 256-byte clamp on the fallback branch.
// Input: lookup("1") -> lookup("stat"); open(0); read at offset 0 and at
//        offset 4096; write; ioctl; readdir; lookup("sub");
//        lseek SEEK_SET 4 / SEEK_CUR +2 / fallback whence 4096; close.
// Expect: open == 0; first read > 0; EOF read == 0; write/ioctl/readdir
//         == VFS_INVALID; lookup == nullptr; SEEK_SET -> 4; SEEK_CUR -> 6;
//         fallback whence -> clamped to 256.
// Depends: vfs::proc_fs
JARVIS_TEST(procfs_ops_pid_stat_file_contract,
            "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::proc_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    vfs::Vnode *piddir = root->ops->lookup(*root, "1");
    JARVIS_ASSERT(piddir != nullptr);
    vfs::Vnode *stat = piddir->ops->lookup(*piddir, "stat");
    JARVIS_ASSERT(stat != nullptr);

    int open_ret = stat->ops->open(*stat, 0);

    uint8_t buf[160] = {};
    int64_t head_read = stat->ops->read(*stat, buf, sizeof(buf) - 1, 0);
    int64_t eof_read = stat->ops->read(*stat, buf, sizeof(buf) - 1, 4096);

    const uint8_t payload[4] = {'x', 'y', 'z', '\n'};
    int64_t write_ret = stat->ops->write(*stat, payload, sizeof(payload), 0);

    kernel::CheckedPtr<uint8_t> ioctl_arg;
    int ioctl_ret = stat->ops->ioctl(*stat, k_unsupported_request, ioctl_arg);

    uint64_t dir_pos = 0;
    vfs::Dirent dent = {};
    int readdir_ret = stat->ops->readdir(*stat, dir_pos, dent);
    vfs::Vnode *child = stat->ops->lookup(*stat, "sub");

    uint64_t pos = 0;
    int64_t set_ret = stat->ops->lseek(*stat, 4, vfs::SEEK_SET, &pos);
    uint64_t after_set = pos;
    int64_t cur_ret = stat->ops->lseek(*stat, 2, vfs::SEEK_CUR, &pos);
    uint64_t after_cur = pos;
    int64_t clamp_ret = stat->ops->lseek(*stat, 4096, k_invalid_whence, &pos);
    uint64_t after_clamp = pos;

    stat->ops->close(*stat);
    piddir->ops->close(*piddir);

    JARVIS_ASSERT_EQ(0, open_ret);
    JARVIS_ASSERT(head_read > 0);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(0), eof_read);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, write_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), ioctl_ret);
    JARVIS_ASSERT_EQ(static_cast<int>(vfs::VFS_INVALID), readdir_ret);
    JARVIS_ASSERT(child == nullptr);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(4), set_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(4), after_set);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(6), cur_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(6), after_cur);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(256), clamp_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(256), after_clamp);
    JARVIS_TEST_PASS();
}

void register_procfs_ops_tests() {
    Logger::info("Registering procfs ops tests");
    JARVIS_REGISTER_TEST(procfs_ops_root_rejects_byte_ops);
    JARVIS_REGISTER_TEST(procfs_ops_meminfo_readonly_contract);
    JARVIS_REGISTER_TEST(procfs_ops_pci_readonly_contract);
    JARVIS_REGISTER_TEST(procfs_ops_self_directory_contract);
    JARVIS_REGISTER_TEST(procfs_ops_pid_dir_namespace_contract);
    JARVIS_REGISTER_TEST(procfs_ops_pid_stat_file_contract);
}
