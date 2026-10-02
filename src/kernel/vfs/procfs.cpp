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

/// @file procfs.cpp
/// @brief Process filesystem implementation (meminfo, pci, self, pid
/// directories).

#include <kernel/vfs/procfs.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/arch/pci.hpp>
#include <string.hpp>
#include <utils.hpp>

namespace kernel {
namespace vfs {

// ── helpers ──
/// @brief Convert a 64-bit unsigned integer to a decimal string.
static void uint64_to_str(char *buffer, uint64_t n) {
    char tmp[32];
    int pos = 32;
    tmp[--pos] = '\0';
    if (n == 0)
        tmp[--pos] = '0';
    else
        while (n) {
            tmp[--pos] = static_cast<char>('0' + (n % 10));
            n /= 10;
        }
    size_t i = 0;
    while (tmp[pos])
        buffer[i++] = tmp[pos++];
    buffer[i] = '\0';
}

// ── meminfo vnode ──
/// @brief Vnode with cached memory info content.
struct MemInfoVnode {
    Vnode base;           ///< Base Vnode.
    char content[256];    ///< Cached content string.
    uint64_t content_len; ///< Length of cached content.
};

/// @brief Read from /proc/meminfo.
static int64_t meminfo_read(Vnode &self, uint8_t *buf, uint64_t count,
                            uint64_t offset) {
    auto *mi = static_cast<MemInfoVnode *>(self.private_data);
    if (!mi)
        return VFS_INVALID;
    if (offset >= mi->content_len)
        return 0;
    uint64_t avail = mi->content_len - offset;
    if (count > avail)
        count = avail;
    memcpy(buf, mi->content + offset, count);
    return static_cast<int64_t>(count);
}

/// @brief Write to /proc/meminfo (not supported).
static int64_t meminfo_write(Vnode &, const uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}
/// @brief Open /proc/meminfo.
static int meminfo_open(Vnode &, uint64_t) {
    return 0;
}
/// @brief Close /proc/meminfo.
static void meminfo_close(Vnode &) {
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int64_t meminfo_lseek(Vnode &self, int64_t offset, int whence,
                             uint64_t *out_pos) {
    auto *mi = static_cast<MemInfoVnode *>(self.private_data);
    if (!mi)
        return VFS_INVALID;
    uint64_t new_pos = 0;
    switch (whence) {
    case SEEK_SET:
        new_pos = static_cast<uint64_t>(offset);
        break;
    case SEEK_CUR:
        new_pos = *out_pos + static_cast<uint64_t>(offset);
        break;
    case SEEK_END:
        new_pos = mi->content_len + static_cast<uint64_t>(offset);
        break;
    default:
        return VFS_INVALID;
    }
    if (new_pos > mi->content_len)
        new_pos = mi->content_len;
    *out_pos = new_pos;
    return static_cast<int64_t>(new_pos);
}

/// @brief Get /proc/meminfo status.
static int meminfo_fstat(Vnode &self, VfsStat &st) {
    auto *mi = static_cast<MemInfoVnode *>(self.private_data);
    if (!mi)
        return VFS_INVALID;
    st.st_size = mi->content_len;
    st.st_mode = S_IFREG;
    return 0;
}

/// @brief I/O control on /proc/meminfo (not supported).
static int meminfo_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}
/// @brief Read directory on /proc/meminfo (not supported).
static int meminfo_readdir(Vnode &, uint64_t &, Dirent &) {
    return VFS_INVALID;
}
/// @brief Look up child in /proc/meminfo (not supported).
static Vnode *meminfo_lookup(Vnode &, const char *) {
    return nullptr;
}

static const VnodeOps meminfo_ops = {
    meminfo_read,   meminfo_write, meminfo_open,  meminfo_close,
    meminfo_lseek,  meminfo_fstat, meminfo_ioctl, meminfo_readdir,
    meminfo_lookup, nullptr,       nullptr,
    nullptr, // create
};

static MemInfoVnode meminfo_vnode = {};

// ── /proc/pci vnode ──
/// @brief Vnode with cached PCI device tree content.
struct PciVnode {
    Vnode base;           ///< Base Vnode.
    char content[2048];   ///< Cached PCI tree string.
    uint64_t content_len; ///< Length of cached content.
};

/// @brief Read from /proc/pci.
static int64_t pci_read(Vnode &self, uint8_t *buf, uint64_t count,
                        uint64_t offset) {
    auto *pi = static_cast<PciVnode *>(self.private_data);
    if (!pi)
        return VFS_INVALID;
    if (offset >= pi->content_len)
        return 0;
    uint64_t avail = pi->content_len - offset;
    if (count > avail)
        count = avail;
    memcpy(buf, pi->content + offset, count);
    return static_cast<int64_t>(count);
}

/// @brief Write to /proc/pci (not supported).
static int64_t pci_write(Vnode &, const uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}
/// @brief Open /proc/pci.
static int pci_open(Vnode &, uint64_t) {
    return 0;
}
/// @brief Close /proc/pci.
static void pci_close(Vnode &) {
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int64_t pci_lseek(Vnode &self, int64_t offset, int whence,
                         uint64_t *out_pos) {
    auto *pi = static_cast<PciVnode *>(self.private_data);
    if (!pi)
        return VFS_INVALID;
    uint64_t new_pos = 0;
    switch (whence) {
    case SEEK_SET:
        new_pos = static_cast<uint64_t>(offset);
        break;
    case SEEK_CUR:
        new_pos = *out_pos + static_cast<uint64_t>(offset);
        break;
    case SEEK_END:
        new_pos = pi->content_len + static_cast<uint64_t>(offset);
        break;
    default:
        return VFS_INVALID;
    }
    if (new_pos > pi->content_len)
        new_pos = pi->content_len;
    *out_pos = new_pos;
    return static_cast<int64_t>(new_pos);
}

/// @brief Get /proc/pci status.
static int pci_fstat(Vnode &self, VfsStat &st) {
    auto *pi = static_cast<PciVnode *>(self.private_data);
    if (!pi)
        return VFS_INVALID;
    st.st_size = pi->content_len;
    st.st_mode = S_IFREG;
    return 0;
}

/// @brief I/O control on /proc/pci (not supported).
static int pci_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}
/// @brief Read directory on /proc/pci (not supported).
static int pci_readdir(Vnode &, uint64_t &, Dirent &) {
    return VFS_INVALID;
}
/// @brief Look up child in /proc/pci (not supported).
static Vnode *pci_lookup(Vnode &, const char *) {
    return nullptr;
}

static const VnodeOps pci_ops = {
    pci_read,  pci_write,   pci_open,   pci_close, pci_lseek, pci_fstat,
    pci_ioctl, pci_readdir, pci_lookup, nullptr,   nullptr,   nullptr,
};

static PciVnode pci_vnode = {};

// ── /proc/self vnode ── (redirects to current pid's dir)
/// @brief Read from /proc/self (not supported).
static int64_t self_read(Vnode &, uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}
/// @brief Write to /proc/self (not supported).
static int64_t self_write(Vnode &, const uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}
/// @brief Open /proc/self.
static int self_open(Vnode &, uint64_t) {
    return 0;
}
/// @brief Close /proc/self.
static void self_close(Vnode &) {
}
/// @brief Seek on /proc/self (not supported).
static int64_t self_lseek(Vnode &, int64_t, int, uint64_t *) {
    return VFS_INVALID;
}
/// @brief Get /proc/self status.
static int self_fstat(Vnode &, VfsStat &vfs_stat) {
    vfs_stat.st_size = 0;
    vfs_stat.st_mode = S_IFDIR;
    return 0;
}
/// @brief I/O control on /proc/self (not supported).
static int self_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}
/// @brief Read directory on /proc/self (not supported).
static int self_readdir(Vnode &, uint64_t &, Dirent &) {
    return VFS_INVALID;
}

static Vnode *self_lookup(Vnode &self, const char *name);

static const VnodeOps self_ops = {
    self_read,  self_write,   self_open,   self_close, self_lseek, self_fstat,
    self_ioctl, self_readdir, self_lookup, nullptr,    nullptr,
    nullptr, // create
};

static Vnode self_vnode = {
    &self_ops, 1, 0, S_IFDIR, nullptr, 0, nullptr,
};

// ── pid stat vnode ──
/// @brief Vnode representing a process's stat file.
struct PidStatVnode {
    Vnode base;             ///< Base Vnode.
    uint64_t pid;           ///< Process ID.
    TaskControlBlock *task; ///< Pointer to the task's TCB.
};

/// @brief Read process stat info.
static int64_t pid_stat_read(Vnode &self, uint8_t *buf, uint64_t count,
                             uint64_t offset) {
    auto *ps = static_cast<PidStatVnode *>(self.private_data);
    if (!ps || !ps->task)
        return VFS_INVALID;

    char tmp[128];
    char id_str[32];
    char prio_str[32];
    uint64_to_str(id_str, ps->task->id);
    uint64_to_str(prio_str, ps->task->priority);

    size_t len = 0;
    const char *state_str = "?";
    // NOLINTBEGIN(bugprone-branch-clone)
    switch (ps->task->state) {
    case TaskState::READY:
        state_str = "R";
        break;
    case TaskState::RUNNING:
        state_str = "R";
        break;
    // NOLINTEND(bugprone-branch-clone)
    case TaskState::BLOCKED:
        state_str = "B";
        break;
    case TaskState::WAITING:
        state_str = "W";
        break;
    case TaskState::TERMINATED:
        state_str = "T";
        break;
    case TaskState::REAPED:
        state_str = "Z";
        break;
    }

    const char *fmt = "pid=%s state=%s priority=%s ticks=%lu\n";
    const char *f = fmt;
    while (*f)
        tmp[len++] = *f++;
    const char *src = id_str;
    while (*src)
        tmp[len++] = *src++;
    f = fmt + 4;
    while (*f != '%')
        tmp[len++] = *f++;
    src = state_str;
    while (*src)
        tmp[len++] = *src++;
    while (*f != '%')
        tmp[len++] = *f++;
    src = prio_str;
    while (*src)
        tmp[len++] = *src++;
    while (*f) {
        if (*f == '\\' && *(f + 1) == 'n') {
            tmp[len++] = '\n';
            f += 2;
            break;
        }
        tmp[len++] = *f++;
    }
    src = fmt;
    while (*src != '%')
        src++;
    src++; // skip %
    uint64_to_str(id_str, ps->task->executed_ticks);
    src = id_str;
    while (*src)
        tmp[len++] = *src++;
    tmp[len] = '\0';

    if (offset >= len)
        return 0;
    uint64_t avail = len - offset;
    if (count > avail)
        count = avail;
    memcpy(buf, tmp + offset, count);
    return static_cast<int64_t>(count);
}

/// @brief Write to process stat (not supported).
static int64_t pid_stat_write(Vnode &, const uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}
/// @brief Open process stat.
static int pid_stat_open(Vnode &, uint64_t) {
    return 0;
}
/// @brief Close process stat.
static void pid_stat_close(Vnode &) {
}
/// @brief Seek within process stat.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int64_t pid_stat_lseek(Vnode &self, int64_t offset, int whence,
                              uint64_t *out_pos) {
    (void)self;
    uint64_t new_pos = 0;
    switch (whence) {
    case SEEK_SET:
        new_pos = static_cast<uint64_t>(offset);
        break;
    case SEEK_CUR:
        new_pos = *out_pos + static_cast<uint64_t>(offset);
        break;
    default:
        new_pos = static_cast<uint64_t>(offset);
        break;
    }
    if (new_pos > 256)
        new_pos = 256;
    *out_pos = new_pos;
    return static_cast<int64_t>(new_pos);
}
/// @brief Get process stat status.
static int pid_stat_fstat(Vnode &, VfsStat &) {
    return VFS_INVALID;
}
/// @brief I/O control on process stat (not supported).
static int pid_stat_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}
/// @brief Read directory on process stat (not supported).
static int pid_stat_readdir(Vnode &, uint64_t &, Dirent &) {
    return VFS_INVALID;
}
/// @brief Look up child in process stat (not supported).
static Vnode *pid_stat_lookup(Vnode &, const char *) {
    return nullptr;
}

static const VnodeOps pid_stat_ops = {
    pid_stat_read,   pid_stat_write, pid_stat_open,  pid_stat_close,
    pid_stat_lseek,  pid_stat_fstat, pid_stat_ioctl, pid_stat_readdir,
    pid_stat_lookup, nullptr,        nullptr,
    nullptr, // create
};

// ── pid watchdog vnode (issue #41) ──
/// @brief Vnode representing a process's watchdog file.
struct PidWatchdogVnode {
    Vnode base;             ///< Base Vnode.
    uint64_t pid;           ///< Process ID.
    TaskControlBlock *task; ///< Pointer to the task's TCB.
};

/// @brief Read process watchdog info: armed/period/last-kick/expires.
/// Kick is exclusively via SYS_WATCHDOG_KICK (write returns VFS_INVALID).
static int64_t pid_watchdog_read(Vnode &self, uint8_t *buf, uint64_t count,
                                 uint64_t offset) {
    auto *pw = static_cast<PidWatchdogVnode *>(self.private_data);
    if (!pw || !pw->task)
        return VFS_INVALID;

    char tmp[128];
    char num[32];
    size_t len = 0;
    const char *keys[] = {"armed=", " period=", " last_kick=", " expires="};
    uint64_t vals[4] = {pw->task->wdog_armed ? 1ULL : 0ULL,
                        pw->task->wdog_period_ticks,
                        pw->task->wdog_last_kick_tick,
                        pw->task->wdog_expiry_tick};
    for (size_t ki = 0; ki < 4; ++ki) {
        const char *k = keys[ki];
        while (*k && len < sizeof(tmp) - 24)
            tmp[len++] = *k++;
        uint64_to_str(num, vals[ki]);
        const char *s = num;
        while (*s && len < sizeof(tmp) - 2)
            tmp[len++] = *s++;
    }
    if (len < sizeof(tmp) - 1)
        tmp[len++] = '\n';
    tmp[len] = '\0';

    if (offset >= len)
        return 0;
    uint64_t avail = len - offset;
    if (count > avail)
        count = avail;
    memcpy(buf, tmp + offset, count);
    return static_cast<int64_t>(count);
}

/// @brief Write to process watchdog (not supported — kick via syscall).
static int64_t pid_watchdog_write(Vnode &, const uint8_t *, uint64_t,
                                  uint64_t) {
    return VFS_INVALID;
}
/// @brief Open process watchdog.
static int pid_watchdog_open(Vnode &, uint64_t) {
    return 0;
}
/// @brief Close process watchdog.
static void pid_watchdog_close(Vnode &) {
}
/// @brief Seek within process watchdog.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int64_t pid_watchdog_lseek(Vnode &self, int64_t offset, int whence,
                                  uint64_t *out_pos) {
    (void)self;
    uint64_t new_pos = 0;
    switch (whence) {
    case SEEK_SET:
        new_pos = static_cast<uint64_t>(offset);
        break;
    case SEEK_CUR:
        new_pos = *out_pos + static_cast<uint64_t>(offset);
        break;
    default:
        new_pos = static_cast<uint64_t>(offset);
        break;
    }
    if (new_pos > 256)
        new_pos = 256;
    *out_pos = new_pos;
    return static_cast<int64_t>(new_pos);
}
/// @brief Get process watchdog status.
static int pid_watchdog_fstat(Vnode &, VfsStat &) {
    return VFS_INVALID;
}
/// @brief I/O control on process watchdog (not supported).
static int pid_watchdog_ioctl(Vnode &, uint64_t,
                              kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}
/// @brief Read directory on process watchdog (not supported).
static int pid_watchdog_readdir(Vnode &, uint64_t &, Dirent &) {
    return VFS_INVALID;
}
/// @brief Look up child in process watchdog (not supported).
static Vnode *pid_watchdog_lookup(Vnode &, const char *) {
    return nullptr;
}

static const VnodeOps pid_watchdog_ops = {
    pid_watchdog_read,   pid_watchdog_write, pid_watchdog_open,
    pid_watchdog_close,  pid_watchdog_lseek, pid_watchdog_fstat,
    pid_watchdog_ioctl,  pid_watchdog_readdir, pid_watchdog_lookup,
    nullptr,             nullptr,
    nullptr, // create
};

// ── pid directory vnode ──
/// @brief Vnode representing a per-process directory in /proc.
struct PidDirVnode {
    Vnode base;             ///< Base Vnode.
    uint64_t pid;           ///< Process ID.
    TaskControlBlock *task; ///< Pointer to the task's TCB.
    Vnode stat_vnode;       ///< Embedded stat vnode.
    PidStatVnode stat_data; ///< Embedded stat vnode data.
    Vnode watchdog_vnode;   ///< Embedded watchdog vnode (issue #41).
    PidWatchdogVnode watchdog_data; ///< Embedded watchdog vnode data.
};

/// @brief Read from a PID directory (not supported).
static int64_t pid_dir_read(Vnode &, uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}
/// @brief Write to a PID directory (not supported).
static int64_t pid_dir_write(Vnode &, const uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}
/// @brief Open a PID directory.
static int pid_dir_open(Vnode &, uint64_t) {
    return 0;
}
/// @brief Close a PID directory, freeing its private data.
static void pid_dir_close(Vnode &self) {
    if (self.private_data) {
        auto *pd = static_cast<PidDirVnode *>(self.private_data);
        kernel::test::ResourceTracker::instance().track_vnode_remove();
        kernel::MemPool::free(pd);
    }
}
/// @brief Seek on PID directory (not supported).
static int64_t pid_dir_lseek(Vnode &, int64_t, int, uint64_t *) {
    return VFS_INVALID;
}
/// @brief Get PID directory status.
static int pid_dir_fstat(Vnode &, VfsStat &) {
    return VFS_INVALID;
}
/// @brief I/O control on PID directory (not supported).
static int pid_dir_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}
/// @brief Read directory on PID directory (not supported).
static int pid_dir_readdir(Vnode &, uint64_t &, Dirent &) {
    return VFS_INVALID;
}

/// @brief Look up a child entry in a PID directory.
static Vnode *pid_dir_lookup(Vnode &self, const char *name) {
    auto *pd = static_cast<PidDirVnode *>(self.private_data);
    if (!pd)
        return nullptr;
    if (strcmp(name, "stat") == 0)
        return &pd->stat_vnode;
    if (strcmp(name, "watchdog") == 0)
        return &pd->watchdog_vnode;
    return nullptr;
}

static const VnodeOps pid_dir_ops = {
    pid_dir_read,   pid_dir_write, pid_dir_open,  pid_dir_close,
    pid_dir_lseek,  pid_dir_fstat, pid_dir_ioctl, pid_dir_readdir,
    pid_dir_lookup, nullptr,       nullptr,
    nullptr, // create
};

// ── self resolution ──
/// @brief Resolve a path component under /proc/self.
static Vnode *self_lookup(Vnode &self, const char *name) {
    (void)self;
    TaskControlBlock *task = Scheduler::current_task();
    if (!task)
        return nullptr;

    // Look for an existing pid dir or create one dynamically
    // For simplicity, return stat of current task
    if (strcmp(name, "stat") == 0) {
        static PidStatVnode cur_stat = {};
        cur_stat.pid = task->id;
        cur_stat.task = task;
        cur_stat.base.ops = &pid_stat_ops;
        cur_stat.base.ino = task->id;
        cur_stat.base.size = 0;
        cur_stat.base.mode = S_IFREG;
        cur_stat.base.private_data = &cur_stat;
        cur_stat.base.parent = &self;
        return &cur_stat.base;
    }
    // Issue #41: same pattern for the watchdog file of the current task.
    if (strcmp(name, "watchdog") == 0) {
        static PidWatchdogVnode cur_wdog = {};
        cur_wdog.pid = task->id;
        cur_wdog.task = task;
        cur_wdog.base.ops = &pid_watchdog_ops;
        cur_wdog.base.ino = task->id;
        cur_wdog.base.size = 0;
        cur_wdog.base.mode = S_IFREG;
        cur_wdog.base.private_data = &cur_wdog;
        cur_wdog.base.parent = &self;
        return &cur_wdog.base;
    }
    return nullptr;
}

// ── procfs root ──
/// @brief Read from procfs root (not supported).
static int64_t proc_root_read(Vnode &, uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}
/// @brief Write to procfs root (not supported).
static int64_t proc_root_write(Vnode &, const uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}
/// @brief Open the procfs root.
static int proc_root_open(Vnode &, uint64_t) {
    return 0;
}
/// @brief Close the procfs root.
static void proc_root_close(Vnode &) {
}
/// @brief Seek on procfs root (not supported).
static int64_t proc_root_lseek(Vnode &, int64_t, int, uint64_t *) {
    return VFS_INVALID;
}
/// @brief Get procfs root status.
static int proc_root_fstat(Vnode &, VfsStat &vfs_stat) {
    vfs_stat.st_size = 0;
    vfs_stat.st_mode = S_IFDIR;
    return 0;
}
/// @brief I/O control on procfs root (not supported).
static int proc_root_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}

/// @brief Read a directory entry from procfs root.
static int proc_root_readdir(Vnode &self, uint64_t &pos, Dirent &dent) {
    (void)self;
    uint64_t position = pos;
    // Static entries first
    if (position == 0) {
        const char *n = "meminfo";
        size_t i = 0;
        while (n[i] && i < 63) {
            dent.d_name[i] = n[i];
            ++i;
        }
        dent.d_name[i] = '\0';
        dent.d_ino = 0;
        ++pos;
        return 0;
    }
    if (position == 1) {
        const char *n = "self";
        size_t i = 0;
        while (n[i] && i < 63) {
            dent.d_name[i] = n[i];
            ++i;
        }
        dent.d_name[i] = '\0';
        dent.d_ino = 1;
        ++pos;
        return 0;
    }
    if (position == 2) {
        const char *n = "pci";
        size_t i = 0;
        while (n[i] && i < 63) {
            dent.d_name[i] = n[i];
            ++i;
        }
        dent.d_name[i] = '\0';
        dent.d_ino = 2;
        ++pos;
        return 0;
    }
    // PID entries
    // For readdir, skip position-3 entries worth of PIDs
    uint64_t task_count = Scheduler::task_count();
    uint64_t pid_idx = position - 3;
    uint64_t found = 0;
    for (uint64_t task_idx = 0; task_idx < task_count && found <= pid_idx;
         ++task_idx) {
        TaskControlBlock *t = Scheduler::task_at(task_idx);
        if (t) {
            if (found == pid_idx) {
                char pid_str[16];
                uint64_to_str(pid_str, t->id);
                size_t i = 0;
                while (pid_str[i] && i < 63) {
                    dent.d_name[i] = pid_str[i];
                    ++i;
                }
                dent.d_name[i] = '\0';
                dent.d_ino = t->id;
                ++pos;
                return 0;
            }
            ++found;
        }
    }
    return VFS_INVALID;
}

/// @brief Look up an entry in procfs root.
static Vnode *proc_root_lookup(Vnode &self, const char *name) {
    (void)self;
    if (strcmp(name, "meminfo") == 0) {
        meminfo_vnode.base.parent = &self;
        return &meminfo_vnode.base;
    }
    if (strcmp(name, "self") == 0) {
        self_vnode.parent = &self;
        return &self_vnode;
    }
    if (strcmp(name, "pci") == 0) {
        pci_vnode.base.parent = &self;
        return &pci_vnode.base;
    }

    // Parse numeric PID
    uint64_t pid = 0;
    const char *pid_str = name;
    while (*pid_str >= '0' && *pid_str <= '9') {
        pid = pid * 10 + (*pid_str - '0');
        ++pid_str;
    }
    if (*pid_str == '\0' && pid > 0) {
        // Find task by PID
        uint64_t task_count = Scheduler::task_count();
        for (uint64_t task_idx = 0; task_idx < task_count; ++task_idx) {
            TaskControlBlock *t = Scheduler::task_at(task_idx);
            if (t && t->id == pid) {
                auto *pd = static_cast<PidDirVnode *>(
                    MemPool::alloc(sizeof(PidDirVnode)));
                if (!pd)
                    return nullptr;
                kernel::test::ResourceTracker::instance().track_vnode_add();
                pd->pid = pid;
                pd->task = t;
                pd->base.ops = &pid_dir_ops;
                pd->base.ino = pid;
                pd->base.size = 0;
                pd->base.mode = S_IFDIR;
                pd->base.private_data = pd;
                pd->base.parent = &self;
                pd->stat_vnode.ops = &pid_stat_ops;
                pd->stat_vnode.ino = pid;
                pd->stat_vnode.size = 0;
                pd->stat_vnode.mode = S_IFREG;
                pd->stat_vnode.private_data = &pd->stat_data;
                pd->stat_vnode.parent = &pd->base;
                pd->stat_data.pid = pid;
                pd->stat_data.task = t;
                pd->watchdog_vnode.ops = &pid_watchdog_ops;
                pd->watchdog_vnode.ino = pid;
                pd->watchdog_vnode.size = 0;
                pd->watchdog_vnode.mode = S_IFREG;
                pd->watchdog_vnode.private_data = &pd->watchdog_data;
                pd->watchdog_vnode.parent = &pd->base;
                pd->watchdog_data.pid = pid;
                pd->watchdog_data.task = t;
                return &pd->base;
            }
        }
    }
    return nullptr;
}

static const VnodeOps proc_root_ops = {
    proc_root_read,   proc_root_write, proc_root_open,  proc_root_close,
    proc_root_lseek,  proc_root_fstat, proc_root_ioctl, proc_root_readdir,
    proc_root_lookup, nullptr,         nullptr,
    nullptr, // create
};

static Vnode proc_root_vnode = {
    &proc_root_ops, 0, 0, S_IFDIR, nullptr, 0, nullptr,
};

/// @brief Get the procfs root vnode (refreshes content on each call).
static Vnode *proc_get_root() {
    // Refresh meminfo content
    uint64_t total = PMM::total_memory();
    uint64_t free = PMM::free_memory();

    char tmp[256];
    size_t pos = 0;
    const char *s1 = "MemTotal: ";
    while (*s1)
        tmp[pos++] = *s1++;
    char num[32];
    uint64_to_str(num, total);
    char *np = num;
    while (*np)
        tmp[pos++] = *np++;
    const char *s2 = " kB\nMemFree: ";
    while (*s2)
        tmp[pos++] = *s2++;
    uint64_to_str(num, free);
    np = num;
    while (*np)
        tmp[pos++] = *np++;
    const char *s3 = " kB\n";
    while (*s3)
        tmp[pos++] = *s3++;
    tmp[pos] = '\0';

    memcpy(meminfo_vnode.content, tmp, pos);
    meminfo_vnode.content_len = pos;
    meminfo_vnode.base.ops = &meminfo_ops;
    meminfo_vnode.base.ino = 0;
    meminfo_vnode.base.size = pos;
    meminfo_vnode.base.mode = S_IFREG;
    meminfo_vnode.base.private_data = &meminfo_vnode;
    meminfo_vnode.base.parent = &proc_root_vnode;

    // Refresh PCI content
#if defined(CONFIG_ARCH_X86_64)
    arch::pci_print_tree(pci_vnode.content, sizeof(pci_vnode.content));
#endif
    pci_vnode.content_len = strlen(pci_vnode.content);
    pci_vnode.base.ops = &pci_ops;
    pci_vnode.base.ino = 2;
    pci_vnode.base.size = pci_vnode.content_len;
    pci_vnode.base.mode = S_IFREG;
    pci_vnode.base.private_data = &pci_vnode;
    pci_vnode.base.parent = &proc_root_vnode;

    return &proc_root_vnode;
}

Filesystem proc_fs = {
    "procfs",
    proc_get_root,
};

} // namespace vfs
} // namespace kernel
