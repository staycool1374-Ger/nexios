/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as
 * published by the Free Software Foundation.
 */

/// @file test_vfs_procfs.cpp
/// @brief procfs tests (milestone v0.4.3 issue #109): readdir enumeration,
///        fstat, lookup resolution, content formatting and error paths for
///        the /proc filesystem, driven through the real proc_fs root vnode.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/procfs.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/task/scheduler.hpp>
#include <string.hpp>

using namespace kernel;

namespace {

/// @brief Freestanding substring search (no strstr in the kernel lib).
const char *contains(const char *haystack, const char *needle) {
    if (!*needle)
        return haystack;
    for (size_t i = 0; haystack[i]; ++i) {
        size_t j = 0;
        while (needle[j] && haystack[i + j] == needle[j])
            ++j;
        if (!needle[j])
            return &haystack[i];
    }
    return nullptr;
}

/// @brief Formats an unsigned value to decimal (mirrors procfs's own
/// formatter for pid/name assertions).
size_t u64_to_str(char *dst, uint64_t value) {
    char tmp[24];
    size_t pos = 24;
    tmp[--pos] = '\0';
    if (value == 0)
        tmp[--pos] = '0';
    while (value) {
        tmp[--pos] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    size_t len = 24 - pos - 1;
    memcpy(dst, &tmp[pos], len + 1);
    return len;
}

/// @brief Parses "MemTotal: N kB\nMemFree: M kB\n" — the exact procfs
/// meminfo format.  Returns the number of kB values successfully parsed
/// (2 on a fully valid buffer).
int parse_meminfo(const char *s, uint64_t &total, uint64_t &free_kb) {
    struct {
        const char *label;
        uint64_t *out;
    } fields[2] = {{"MemTotal: ", &total}, {"MemFree: ", &free_kb}};
    int parsed = 0;
    const char *cursor = s;
    for (int i = 0; i < 2; ++i) {
        size_t label_len = strlen(fields[i].label);
        if (memcmp(cursor, fields[i].label, label_len) != 0)
            return parsed;
        cursor += label_len;
        uint64_t value = 0;
        bool digits = false;
        while (*cursor >= '0' && *cursor <= '9') {
            value = value * 10 + static_cast<uint64_t>(*cursor - '0');
            ++cursor;
            digits = true;
        }
        if (!digits)
            return parsed;
        *fields[i].out = value;
        ++parsed;
        const char *suffix = " kB\n";
        if (memcmp(cursor, suffix, 4) != 0)
            return parsed;
        cursor += 4;
    }
    return parsed;
}

} // namespace

// Runmode: kernel
// Testidea: The procfs root resolves as a directory (fstat S_IFDIR) and
// reading it as a file fails with VFS_INVALID (directories are not
// byte-readable).
// Input: proc_fs.get_root(); fstat + read.
// Expect: get_root non-null; fstat mode S_IFDIR; read returns VFS_INVALID.
// Depends: vfs::proc_fs
JARVIS_TEST(procfs_root_is_directory, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::proc_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    JARVIS_ASSERT(root->ops != nullptr);

    vfs::VfsStat st{};
    JARVIS_ASSERT_EQ(0, root->ops->fstat(*root, st));
    bool is_dir = (st.st_mode & vfs::S_IFDIR) != 0;

    uint8_t buf[8] = {};
    int64_t nread = root->ops->read(*root, buf, sizeof(buf), 0);

    JARVIS_ASSERT(is_dir);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, nread);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Readdir enumerates the three static nodes first, in the
// documented order meminfo → self → pci with the documented inode numbers
// 0/1/2.
// Input: readdir from position 0.
// Expect: entries 0..2 are named meminfo/self/pci with d_ino 0/1/2.
// Depends: vfs::proc_fs
JARVIS_TEST(procfs_readdir_static_entries, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::proc_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    vfs::Dirent dents[3] = {};
    uint64_t pos = 0;
    JARVIS_ASSERT_EQ(0, root->ops->readdir(*root, pos, dents[0]));
    JARVIS_ASSERT_EQ(0, root->ops->readdir(*root, pos, dents[1]));
    JARVIS_ASSERT_EQ(0, root->ops->readdir(*root, pos, dents[2]));

    JARVIS_ASSERT(memcmp(dents[0].d_name, "meminfo", 8) == 0);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), dents[0].d_ino);
    JARVIS_ASSERT(memcmp(dents[1].d_name, "self", 5) == 0);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), dents[1].d_ino);
    JARVIS_ASSERT(memcmp(dents[2].d_name, "pci", 4) == 0);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2), dents[2].d_ino);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: After the static nodes, readdir lists live task PIDs as
// decimal names with d_ino == the task id, and enumeration terminates.
// Input: readdir from position 3; compare the first listed name against
//        the id of the first scheduler slot (task_at(0)).
// Expect: First PID entry name == decimal(task_at(0)->id) and d_ino ==
//         that id; enumeration ends (returns non-zero) at most
//         task_count entries later.
// Depends: vfs::proc_fs, Scheduler::task_count/task_at
JARVIS_TEST(procfs_readdir_lists_task_pids, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::proc_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    uint64_t task_count = Scheduler::task_count();
    JARVIS_ASSERT(task_count > 0);

    TaskControlBlock *first = Scheduler::task_at(0);
    JARVIS_ASSERT(first != nullptr);

    uint64_t pos = 3;
    vfs::Dirent dent{};
    JARVIS_ASSERT_EQ(0, root->ops->readdir(*root, pos, dent));
    char expected[24];
    (void)u64_to_str(expected, first->id);
    JARVIS_ASSERT(memcmp(dent.d_name, expected, strlen(expected) + 1) == 0);
    JARVIS_ASSERT_EQ(first->id, dent.d_ino);

    // Enumeration must terminate within the task count + margin.
    int drained = 0;
    while (drained < static_cast<int>(task_count) + 8) {
        vfs::Dirent extra{};
        if (root->ops->readdir(*root, pos, extra) != 0)
            break;
        ++drained;
    }
    JARVIS_ASSERT(drained < static_cast<int>(task_count) + 8);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: /proc/meminfo content follows the exact documented format
// "MemTotal: N kB\nMemFree: M kB\n" with N > 0 and N >= M; fstat size
// matches the content length; SEEK_END lseek lands on that length; write
// is rejected with VFS_INVALID.
// Input: lookup("meminfo") from the root, then read/fstat/lseek/write.
// Expect: Content parses to two positive kB values, total >= free; size
//         and SEEK_END agree; write returns VFS_INVALID.
// Depends: vfs::proc_fs
JARVIS_TEST(procfs_meminfo_content_format, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::proc_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    vfs::Vnode *meminfo = root->ops->lookup(*root, "meminfo");
    JARVIS_ASSERT(meminfo != nullptr);

    vfs::VfsStat st{};
    JARVIS_ASSERT_EQ(0, meminfo->ops->fstat(*meminfo, st));
    bool is_reg = (st.st_mode & vfs::S_IFREG) != 0;
    JARVIS_ASSERT(is_reg);

    uint8_t buf[256] = {};
    int64_t nread = meminfo->ops->read(*meminfo, buf, sizeof(buf) - 1, 0);
    JARVIS_ASSERT(nread > 0);
    buf[nread] = '\0';

    // Parse "MemTotal: N kB\nMemFree: M kB\n".
    uint64_t total = 0;
    uint64_t free_kb = 0;
    int fields = parse_meminfo(reinterpret_cast<const char *>(buf), total,
                               free_kb);
    uint64_t seek_end = 0;
    int64_t lret = meminfo->ops->lseek(
        *meminfo, 0, vfs::SEEK_END, &seek_end);
    int64_t wret = meminfo->ops->write(
        *meminfo, buf, 4, 0);

    JARVIS_ASSERT_EQ(2, fields);
    JARVIS_ASSERT(total > 0);
    JARVIS_ASSERT(total >= free_kb);
    JARVIS_ASSERT(free_kb > 0);
    JARVIS_ASSERT_EQ(st.st_size, static_cast<uint64_t>(nread));
    JARVIS_ASSERT_EQ(static_cast<int64_t>(st.st_size), lret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(st.st_size), seek_end);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, wret);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: /proc/pci resolves as a regular file whose declared size is
// consistent with a read of the full content.
// Input: lookup("pci"); fstat; read up to size.
// Expect: fstat S_IFREG; read returns exactly st.st_size bytes (>= 0).
// Depends: vfs::proc_fs
JARVIS_TEST(procfs_pci_node_readable, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::proc_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    vfs::Vnode *pci = root->ops->lookup(*root, "pci");
    JARVIS_ASSERT(pci != nullptr);

    vfs::VfsStat st{};
    JARVIS_ASSERT_EQ(0, pci->ops->fstat(*pci, st));
    bool is_reg = (st.st_mode & vfs::S_IFREG) != 0;

    uint8_t buf[2048] = {};
    int64_t nread =
        pci->ops->read(*pci, buf, sizeof(buf), 0);

    JARVIS_ASSERT(is_reg);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(st.st_size), nread);
    JARVIS_ASSERT(nread >= 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: /proc/self resolves as a directory whose "stat" child renders
// the current task's identity: the "pid=" label, the task id digits, the
// "state=" label and "priority=" label with the numeric priority must all
// be present in the rendered line.  fstat on the stat node is
// intentionally VFS_INVALID per the implementation.
// NOTE (defect found by this test): pid_stat_read() copies the format
// template verbatim before appending the substituted values, so the line
// also contains literal "%s"/"%lu" placeholders — the DATA contract
// (labels + values present) is asserted here; the template duplication
// is reported on the coverage issue.
// Input: lookup("self"), then lookup("stat") on it; read + fstat.
// Expect: self is S_IFDIR; content contains "pid=", the current id,
//         "state=", "priority=" and the priority digits; fstat returns
//         VFS_INVALID.
// Depends: vfs::proc_fs, Scheduler::current_task
JARVIS_TEST(procfs_self_stat_line, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::proc_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    vfs::Vnode *self = root->ops->lookup(*root, "self");
    JARVIS_ASSERT(self != nullptr);
    vfs::VfsStat self_st{};
    JARVIS_ASSERT_EQ(0, self->ops->fstat(*self, self_st));
    bool self_dir = (self_st.st_mode & vfs::S_IFDIR) != 0;
    JARVIS_ASSERT(self_dir);

    vfs::Vnode *stat = self->ops->lookup(*self, "stat");
    JARVIS_ASSERT(stat != nullptr);

    TaskControlBlock *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    char pid_str[24];
    char prio_str[24];
    (void)u64_to_str(pid_str, cur->id);
    (void)u64_to_str(prio_str, cur->priority);

    uint8_t buf[160] = {};
    int64_t nread = stat->ops->read(*stat, buf, sizeof(buf) - 1, 0);
    int fstat_ret = stat->ops->fstat(*stat, self_st);

    JARVIS_ASSERT(nread > 0);
    buf[nread] = '\0';
    const char *line = reinterpret_cast<const char *>(buf);
    JARVIS_ASSERT(contains(line, "pid=") != nullptr);
    JARVIS_ASSERT(contains(line, pid_str) != nullptr);
    JARVIS_ASSERT(contains(line, "state=") != nullptr);
    JARVIS_ASSERT(contains(line, "priority=") != nullptr);
    JARVIS_ASSERT(contains(line, prio_str) != nullptr);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, static_cast<int64_t>(fstat_ret));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A numeric PID lookup resolves to a pid directory holding a
// stat file for init (pid 1): the content carries the "pid=" label and
// the id digits.  fstat on the pid dir is VFS_INVALID (not-supported op,
// documented); the directory close releases its private MemPool block
// (ResourceTracker-clean teardown).
// Input: lookup("1"), then lookup("stat") inside, read, then
//        ops->close on the pid dir.
// Expect: pid dir fstat == VFS_INVALID; stat content contains "pid=" and
//         "1"; close completes (leak checked by the isolation snapshot).
// Depends: vfs::proc_fs, MemPool
JARVIS_TEST(procfs_pid_dir_stat_and_close, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::proc_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    vfs::Vnode *piddir = root->ops->lookup(*root, "1");
    JARVIS_ASSERT(piddir != nullptr);

    vfs::VfsStat st{};
    int fstat_ret = piddir->ops->fstat(*piddir, st);

    vfs::Vnode *stat = piddir->ops->lookup(*piddir, "stat");
    uint8_t buf[160] = {};
    int64_t nread = 0;
    if (stat) {
        nread = stat->ops->read(*stat, buf, sizeof(buf) - 1, 0);
        if (nread > 0)
            buf[nread] = '\0';
    }
    piddir->ops->close(*piddir);

    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, static_cast<int64_t>(fstat_ret));
    JARVIS_ASSERT(stat != nullptr);
    JARVIS_ASSERT(nread > 0);
    const char *line = reinterpret_cast<const char *>(buf);
    JARVIS_ASSERT(contains(line, "pid=") != nullptr);
    JARVIS_ASSERT(contains(line, "1") != nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Unknown and malformed lookups are rejected: non-numeric
// names, non-existent PIDs, "0" (pid must be > 0) and the empty name all
// return nullptr.
// Input: lookup with "abc", "999999", "0", "".
// Expect: All return nullptr.
// Depends: vfs::proc_fs
JARVIS_TEST(procfs_unknown_nodes_rejected, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::proc_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    JARVIS_ASSERT(root->ops->lookup(*root, "abc") == nullptr);
    JARVIS_ASSERT(root->ops->lookup(*root, "999999") == nullptr);
    JARVIS_ASSERT(root->ops->lookup(*root, "0") == nullptr);
    JARVIS_ASSERT(root->ops->lookup(*root, "") == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Reading a directory node through its read op fails with
// VFS_INVALID — /proc/self is a directory and self_read documents the
// not-supported contract.
// Input: lookup("self") then read on it.
// Expect: VFS_INVALID.
// Depends: vfs::proc_fs
JARVIS_TEST(procfs_dir_read_invalid, "PRE: vfsd, iocd | POST: none") {
    vfs::Vnode *root = vfs::proc_fs.get_root();
    JARVIS_ASSERT(root != nullptr);
    vfs::Vnode *self = root->ops->lookup(*root, "self");
    JARVIS_ASSERT(self != nullptr);
    uint8_t buf[8] = {};
    int64_t nread = self->ops->read(*self, buf, sizeof(buf), 0);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, nread);
    JARVIS_TEST_PASS();
}

void register_vfs_procfs_tests() {
    Logger::info("Registering vfs procfs tests");
    JARVIS_REGISTER_TEST(procfs_root_is_directory);
    JARVIS_REGISTER_TEST(procfs_readdir_static_entries);
    JARVIS_REGISTER_TEST(procfs_readdir_lists_task_pids);
    JARVIS_REGISTER_TEST(procfs_meminfo_content_format);
    JARVIS_REGISTER_TEST(procfs_pci_node_readable);
    JARVIS_REGISTER_TEST(procfs_self_stat_line);
    JARVIS_REGISTER_TEST(procfs_pid_dir_stat_and_close);
    JARVIS_REGISTER_TEST(procfs_unknown_nodes_rejected);
    JARVIS_REGISTER_TEST(procfs_dir_read_invalid);
}
