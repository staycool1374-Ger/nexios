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

/// @file devfs.cpp
/// @brief Device filesystem implementation (tty, null, console, kbd, random).

#include <kernel/vfs/devfs.hpp>
#include <kernel/random.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <services/terminal/terminal.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/keyboard.hpp>
#include <constants.hpp>
#include <string.hpp>

namespace kernel {
namespace vfs {

// ── serial helpers (COM1) ──
/// @brief Check if the serial port has data available.
[[gnu::always_inline]] static inline bool serial_has_data() {
#if defined(CONFIG_ARCH_X86_64)
    return arch::inb(arch::COM1_LSR) & 1;
#else
    return false;
#endif
}

/// @brief Initialize the device filesystem, flushing keyboard buffer.
void devfs_init() {
    arch::Keyboard::flush();
}

// ── tty vnode (serial + keyboard console) ──
/// @brief Read from the tty device (serial + keyboard input).
static int64_t tty_read(Vnode &self, uint8_t *buffer, uint64_t count,
                        uint64_t) {
    if (count == 0)
        return 0;
    // VULN-W2: previously spun on arch::pause() for UINT64_MAX iterations
    // without descheduling, monopolising the CPU core (WCET violation).  Use
    // the same cooperative blocking pattern as Syscall::sys_receive: mark
    // BLOCKED, reschedule, and re-check only after being re-dispatched.
    while (true) {
        if (serial_has_data()) {
#if defined(CONFIG_ARCH_X86_64)
            char character = static_cast<char>(arch::inb(arch::COM1));
            buffer[0] = (character == '\r') ? '\n' : character;
            return 1;
#endif
        }
        char key_char = 0;
        if (arch::Keyboard::getchar(key_char)) {
            buffer[0] = static_cast<uint8_t>(key_char);
            return 1;
        }
        if (self.private_data &&
            (reinterpret_cast<uint64_t>(self.private_data) & O_NONBLOCK)) {
            return VFS_INVALID;
        }
        auto *cur = kernel::Scheduler::current_task();
        if (!cur)
            return VFS_INVALID;
        cur->state = TaskState::BLOCKED;
        kernel::Scheduler::reschedule();
        // v0.4.0 MP-1: sti/hlt/cli is the USER-task blocked-wait pattern.
        if (cur->is_user_) {
            arch::sti();
            arch::hlt();
            arch::cli();
        }
    }
}

/// @brief Write to the tty device (serial output).
static int64_t tty_write(Vnode &, const uint8_t *buf, uint64_t count,
                         uint64_t) {
    service::Terminal::write(reinterpret_cast<const char *>(buf), count);
    return static_cast<int64_t>(count);
}

/// @brief Open the tty device.
static int tty_open(Vnode &self, uint64_t flags) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    self.private_data = reinterpret_cast<void *>(flags);
    return 0;
}

/// @brief Close the tty device.
static void tty_close(Vnode &) {
}

/// @brief Seek on tty (not supported).
static int64_t tty_lseek(Vnode &, int64_t, int, uint64_t *) {
    return VFS_INVALID;
}
/// @brief Get tty device status.
static int tty_fstat(Vnode &, VfsStat &stat) {
    stat.st_size = 0;
    stat.st_mode = S_IFCHR;
    return 0;
}
/// @brief I/O control on tty (not supported).
static int tty_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}
/// @brief Read directory on tty (not supported).
static int tty_readdir(Vnode &, uint64_t &, Dirent &) {
    return VFS_INVALID;
}
/// @brief Look up child in tty (not supported).
static Vnode *tty_lookup(Vnode &, const char *) {
    return nullptr;
}

static const VnodeOps tty_ops = {
    tty_read,  tty_write,   tty_open,   tty_close, tty_lseek, tty_fstat,
    tty_ioctl, tty_readdir, tty_lookup, nullptr,   nullptr,
    nullptr, // create
};

static Vnode tty_vnode = {
    &tty_ops, 1, 0, S_IFCHR, nullptr, 0, nullptr,
};

// ── null vnode ──
/// @brief Read from null device (returns 0 bytes).
static int64_t null_read(Vnode &, uint8_t *, uint64_t, uint64_t) {
    return 0;
}
/// @brief Write to null device (discards data).
static int64_t null_write(Vnode &, const uint8_t *, uint64_t count, uint64_t) {
    return static_cast<int64_t>(count);
}
/// @brief Open the null device.
static int null_open(Vnode &, uint64_t) {
    return 0;
}
/// @brief Close the null device.
static void null_close(Vnode &) {
}
/// @brief Seek on null device.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int64_t null_lseek(Vnode &, int64_t offset, int whence,
                          uint64_t *out_pos) {
    switch (whence) {
    case SEEK_SET:
        *out_pos = static_cast<uint64_t>(offset);
        break;
    case SEEK_CUR:
        *out_pos += static_cast<uint64_t>(offset);
        break;
    case SEEK_END:
        *out_pos = static_cast<uint64_t>(offset);
        break;
    default:
        break;
    }
    return static_cast<int64_t>(*out_pos);
}
/// @brief Get null device status.
static int null_fstat(Vnode &, VfsStat &stat) {
    stat.st_size = 0;
    stat.st_mode = S_IFCHR;
    return 0;
}
/// @brief I/O control on null (not supported).
static int null_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}
/// @brief Read directory on null (not supported).
static int null_readdir(Vnode &, uint64_t &, Dirent &) {
    return VFS_INVALID;
}
/// @brief Look up child in null (not supported).
static Vnode *null_lookup(Vnode &, const char *) {
    return nullptr;
}

static const VnodeOps null_ops = {
    null_read,  null_write,   null_open,   null_close, null_lseek, null_fstat,
    null_ioctl, null_readdir, null_lookup, nullptr,    nullptr,
    nullptr, // create
};

static Vnode null_vnode = {
    &null_ops, 2, 0, S_IFCHR, nullptr, 0, nullptr,
};

// ── console vnode ──
/// @brief Read from console (not supported).
static int64_t console_read(Vnode &, uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}
/// @brief Write to console (terminal output).
static int64_t console_write(Vnode &, const uint8_t *buf, uint64_t count,
                             uint64_t) {
    service::Terminal::write(reinterpret_cast<const char *>(buf), count);
    return static_cast<int64_t>(count);
}
/// @brief Open the console device.
static int console_open(Vnode &, uint64_t) {
    return 0;
}
/// @brief Close the console device.
static void console_close(Vnode &) {
}
/// @brief Seek on console (not supported).
static int64_t console_lseek(Vnode &, int64_t, int, uint64_t *) {
    return VFS_INVALID;
}
/// @brief Get console device status.
static int console_fstat(Vnode &, VfsStat &stat) {
    stat.st_size = 0;
    stat.st_mode = S_IFCHR;
    return 0;
}
/// @brief I/O control on console (not supported).
static int console_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}
/// @brief Read directory on console (not supported).
static int console_readdir(Vnode &, uint64_t &, Dirent &) {
    return VFS_INVALID;
}
/// @brief Look up child in console (not supported).
static Vnode *console_lookup(Vnode &, const char *) {
    return nullptr;
}

static const VnodeOps console_ops = {
    console_read,   console_write, console_open,  console_close,
    console_lseek,  console_fstat, console_ioctl, console_readdir,
    console_lookup, nullptr,       nullptr,
    nullptr, // create
};

static Vnode console_vnode = {
    &console_ops, 3, 0, S_IFCHR, nullptr, 0, nullptr,
};

// ── kbd vnode ──
/// @brief Read from the keyboard device.
static int64_t kbd_read(Vnode &self, uint8_t *buffer, uint64_t count,
                        uint64_t) {
    if (count == 0)
        return 0;
    char key_char = 0;
    // VULN-W2: cooperative blocking (see tty_read) instead of UINT64_MAX spin.
    while (true) {
        if (arch::Keyboard::getchar(key_char)) {
            buffer[0] = static_cast<uint8_t>(key_char);
            return 1;
        }
        if (self.private_data &&
            (reinterpret_cast<uint64_t>(self.private_data) & O_NONBLOCK)) {
            return VFS_INVALID;
        }
        auto *cur = kernel::Scheduler::current_task();
        if (!cur)
            return VFS_INVALID;
        cur->state = TaskState::BLOCKED;
        kernel::Scheduler::reschedule();
        // v0.4.0 MP-1: sti/hlt/cli is the USER-task blocked-wait pattern.
        if (cur->is_user_) {
            arch::sti();
            arch::hlt();
            arch::cli();
        }
    }
}
/// @brief Write to keyboard device (not supported).
static int64_t kbd_write(Vnode &, const uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}
/// @brief Open the keyboard device.
static int kbd_open(Vnode &self, uint64_t flags) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    self.private_data = reinterpret_cast<void *>(flags);
    return 0;
}
/// @brief Close the keyboard device.
static void kbd_close(Vnode &) {
}
/// @brief Seek on keyboard (not supported).
static int64_t kbd_lseek(Vnode &, int64_t, int, uint64_t *) {
    return VFS_INVALID;
}
/// @brief Get keyboard device status.
static int kbd_fstat(Vnode &, VfsStat &stat) {
    stat.st_size = 0;
    stat.st_mode = S_IFCHR;
    return 0;
}
/// @brief I/O control on keyboard (not supported).
static int kbd_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}
/// @brief Read directory on keyboard (not supported).
static int kbd_readdir(Vnode &, uint64_t &, Dirent &) {
    return VFS_INVALID;
}
/// @brief Look up child in keyboard (not supported).
static Vnode *kbd_lookup(Vnode &, const char *) {
    return nullptr;
}

static const VnodeOps kbd_ops = {
    kbd_read,  kbd_write,   kbd_open,   kbd_close, kbd_lseek, kbd_fstat,
    kbd_ioctl, kbd_readdir, kbd_lookup, nullptr,   nullptr,
    nullptr, // create
};

static Vnode kbd_vnode = {
    &kbd_ops, 4, 0, S_IFCHR, nullptr, 0, nullptr,
};

// ── random vnode ──
/// @brief Read random bytes from the random device.
static int64_t random_read(Vnode &, uint8_t *buffer, uint64_t count, uint64_t) {
    kernel::random_fill(buffer, count);
    return static_cast<int64_t>(count);
}
/// @brief Write to random device (discards data, returns written count).
static int64_t random_write(Vnode &, const uint8_t *, uint64_t count,
                            uint64_t) {
    return static_cast<int64_t>(count);
}
/// @brief Open the random device.
static int random_open(Vnode &, uint64_t) {
    return 0;
}
/// @brief Close the random device.
static void random_close(Vnode &) {
}
/// @brief Seek on random device (not supported).
static int64_t random_lseek(Vnode &, int64_t, int, uint64_t *) {
    return VFS_INVALID;
}
/// @brief Get random device status.
static int random_fstat(Vnode &, VfsStat &stat) {
    stat.st_size = 0;
    stat.st_mode = S_IFCHR;
    return 0;
}
/// @brief I/O control on random (not supported).
static int random_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}
/// @brief Read directory on random (not supported).
static int random_readdir(Vnode &, uint64_t &, Dirent &) {
    return VFS_INVALID;
}
/// @brief Look up child in random (not supported).
static Vnode *random_lookup(Vnode &, const char *) {
    return nullptr;
}

static const VnodeOps random_ops = {
    random_read,   random_write, random_open,  random_close,
    random_lseek,  random_fstat, random_ioctl, random_readdir,
    random_lookup, nullptr,      nullptr,      nullptr,
};

static Vnode random_vnode = {
    &random_ops, 5, 0, S_IFCHR, nullptr, 0, nullptr,
};

// ── devfs root ──
/// @brief Device directory entry linking a name to a vnode.
struct DevEntry {
    const char *name; ///< Device name.
    Vnode *vnode;     ///< Device vnode pointer.
};

static DevEntry dev_entries[] = {
    {"tty", &tty_vnode}, {"null", &null_vnode},     {"console", &console_vnode},
    {"kbd", &kbd_vnode}, {"random", &random_vnode},
};
static const size_t dev_entry_count =
    sizeof(dev_entries) / sizeof(dev_entries[0]);

/// @brief Read from devfs root (not supported).
static int64_t dev_root_read(Vnode &, uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}
/// @brief Write to devfs root (not supported).
static int64_t dev_root_write(Vnode &, const uint8_t *, uint64_t, uint64_t) {
    return VFS_INVALID;
}
/// @brief Open the devfs root.
static int dev_root_open(Vnode &, uint64_t) {
    return 0;
}
/// @brief Close the devfs root.
static void dev_root_close(Vnode &) {
}
/// @brief Seek on devfs root (not supported).
static int64_t dev_root_lseek(Vnode &, int64_t, int, uint64_t *) {
    return VFS_INVALID;
}
/// @brief Get devfs root status.
static int dev_root_fstat(Vnode &, VfsStat &stat) {
    stat.st_size = 0;
    stat.st_mode = S_IFDIR;
    return 0;
}
/// @brief I/O control on devfs root (not supported).
static int dev_root_ioctl(Vnode &, uint64_t, kernel::CheckedPtr<uint8_t>) {
    return VFS_INVALID;
}

/// @brief Read a directory entry from devfs root.
static int dev_root_readdir(Vnode &, uint64_t &pos, Dirent &dent) {
    if (pos >= dev_entry_count)
        return VFS_INVALID;
    DevEntry &entry = dev_entries[pos];
    size_t idx = 0;
    while (entry.name[idx] && idx < 63) {
        dent.d_name[idx] = entry.name[idx];
        ++idx;
    }
    dent.d_name[idx] = '\0';
    dent.d_ino = entry.vnode->ino;
    ++pos;
    return 0;
}

/// @brief Look up a device by name in devfs root.
static Vnode *dev_root_lookup(Vnode &, const char *name) {
    for (size_t i = 0; i < dev_entry_count; ++i) {
        if (strcmp(dev_entries[i].name, name) == 0) {
            return dev_entries[i].vnode;
        }
    }
    return nullptr;
}

static const VnodeOps dev_root_ops = {
    dev_root_read,   dev_root_write, dev_root_open,  dev_root_close,
    dev_root_lseek,  dev_root_fstat, dev_root_ioctl, dev_root_readdir,
    dev_root_lookup, nullptr,        nullptr,
    nullptr, // create
};

static Vnode dev_root_vnode = {
    &dev_root_ops, 0, 0, S_IFDIR, nullptr, 0, nullptr,
};

/// @brief Get the devfs root vnode.
static Vnode *dev_get_root() {
    return &dev_root_vnode;
}

Filesystem dev_fs = {
    "devfs",
    dev_get_root,
};

} // namespace vfs
} // namespace kernel
