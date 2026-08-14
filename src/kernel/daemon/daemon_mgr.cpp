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

/// @file daemon_mgr.cpp
/// @brief Daemon lifecycle manager implementation.

#include <kernel/daemon/daemon_mgr.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/elf/elf.hpp>
#include <initrd/initrd.hpp>
#include <kernel/log/dmesg.hpp>
#include <logger.hpp>
#include <string.hpp>

extern "C" void debug_write(const char *s);
extern "C" void debug_write_hex(uint64_t value);

namespace kernel {
namespace daemon {

/// Registered daemon entries.
static DaemonEntry entries_[MAX_DAEMONS] = {};
/// Number of registered daemons.
static uint64_t num_daemons_ = 0;
/// Suppress [DAEMON] died serial output (e.g. during reboot_from_table
/// teardown).
static bool suppress_death_msg_ = false;

/// @brief Initialize the daemon manager — clear all entries and reset count.
void init() {
    for (uint64_t i = 0; i < MAX_DAEMONS; ++i) {
        entries_[i].name = nullptr;
        entries_[i].pid = 0;
    }
    num_daemons_ = 0;
}

bool register_daemon(const char *name, const char *initrd_path,
                     void (*set_pid)(uint64_t), uint64_t (*get_pid)()) {
    if (num_daemons_ >= MAX_DAEMONS)
        return false;

    entries_[num_daemons_].name = name;
    entries_[num_daemons_].initrd_path = initrd_path;
    entries_[num_daemons_].pid = get_pid ? get_pid() : 0;
    entries_[num_daemons_].restart_count = 0;
    entries_[num_daemons_].set_pid_fn = set_pid;
    entries_[num_daemons_].get_pid_fn = get_pid;
    ++num_daemons_;

    Logger::info("daemon_mgr: registered '%s' at path '%s' (PID=%d)", name,
                 initrd_path, entries_[num_daemons_ - 1].pid);
    return true;
}

/// @brief Kill and re-register all daemons — resets state and clears count.
void reset_clear_daemons() {
    for (uint64_t i = 0; i < num_daemons_; ++i) {
        entries_[i].restart_count = 0;
        entries_[i].pid = 0;
        if (entries_[i].set_pid_fn) {
            entries_[i].set_pid_fn(0);
        }
    }
    num_daemons_ = 0;
    Logger::info("daemon_mgr: all daemon states reset");
}

/// @brief Notify the manager that a daemon has exited.
/// @param pid  PID of the daemon that died.
void set_suppress_death_msg(bool v) {
    suppress_death_msg_ = v;
}

void notify_death(uint64_t pid, bool log_only) {
    for (uint64_t i = 0; i < num_daemons_; ++i) {
        if (entries_[i].pid == pid) {
            if (!suppress_death_msg_) {
                debug_write("[DAEMON] '");
                debug_write(entries_[i].name);
                debug_write("' died (PID=");
                debug_write_hex(pid);
                debug_write("), restart_count=");
                debug_write_hex(entries_[i].restart_count);
                debug_write("\n");
            }
            if (!log_only) {
                log::dmesg_push_base(0xDA01, entries_[i].name, pid);
            }

            // Reset PID via the module's setter so all IPC
            // callers immediately fail
            if (entries_[i].set_pid_fn) {
                entries_[i].set_pid_fn(0);
            }
            entries_[i].pid = 0;
            return;
        }
    }
}

/// @brief Snapshot all daemon entries for test isolation.
/// @param[out] out_entries  Array to copy entries into (must hold MAX_DAEMONS).
/// @param[out] num_out      Set to the current daemon count.
void capture_state(DaemonEntry *out_entries, uint64_t &num_out) {
    for (uint64_t i = 0; i < MAX_DAEMONS; ++i) {
        out_entries[i] = entries_[i];
    }
    num_out = num_daemons_;
}

/// @brief Restore daemon entries from a captured snapshot (test isolation).
/// @param[in] entries_in  Array of entries to restore.
/// @param[in] num_in      Number of entries to restore.
void restore_state(const DaemonEntry *entries_in, uint64_t num_in) {
    for (uint64_t i = 0; i < MAX_DAEMONS; ++i) {
        entries_[i] = entries_in[i];
    }
    num_daemons_ = num_in;
}

/// @brief Restart any daemons whose PID is 0 (crashed / never started).
/// Respects MAX_RESTART_COUNT per daemon; logs warnings on failure.
void restart_stale_daemons() {
    for (uint64_t i = 0; i < num_daemons_; ++i) {
        if (entries_[i].pid != 0)
            continue;

        // Check restart limit
        if (entries_[i].restart_count >= MAX_RESTART_COUNT) {
            Logger::warn(
                "daemon_mgr: '%s' restart limit reached (%d), giving up",
                entries_[i].name, entries_[i].restart_count);
            continue;
        }

        // Try to find the initrd file
        initrd::InitrdFile f = initrd::find(entries_[i].initrd_path);
        if (!f.data) {
            // Try with ./ prefix
            char with_dot[256];
            with_dot[0] = '.';
            with_dot[1] = '/';
            size_t j = 0;
            while (entries_[i].initrd_path[j] && j < 254) {
                with_dot[j + 2] = entries_[i].initrd_path[j];
                ++j;
            }
            with_dot[j + 2] = '\0';
            f = initrd::find(with_dot);
        }
        if (!f.data) {
            Logger::warn(
                "daemon_mgr: '%s' initrd file not found, cannot restart",
                entries_[i].name);
            continue;
        }

        auto *hdr = reinterpret_cast<const kernel::elf::ELF64Header *>(f.data);
        if (!kernel::elf::validate_header(hdr)) {
            Logger::warn("daemon_mgr: '%s' invalid ELF header, cannot restart",
                         entries_[i].name);
            continue;
        }

        auto *task = kernel::elf::load(hdr, f.data, f.size);
        if (!task) {
            Logger::warn("daemon_mgr: '%s' elf::load failed, cannot restart",
                         entries_[i].name);
            continue;
        }

        task->priority = 20;
        task->base_priority = 20;
        task->period_ticks = 10;
        {
            size_t j = 0;
            while (entries_[i].name && entries_[i].name[j] &&
                   j < CONFIG_TASK_NAME_LEN - 1) {
                task->name[j] = entries_[i].name[j];
                ++j;
            }
            task->name[j] = '\0';
        }
        {
            uint64_t bg = 0;
            uint64_t budget =
                (strcmp(entries_[i].name, "iocd") == 0) ? 3ULL : 2ULL;
            task->init_sporadic_server(budget, 10, bg, 1);
        }

        kernel::Scheduler::add_task(*task);

        ++entries_[i].restart_count;
        entries_[i].pid = task->id;
        if (entries_[i].set_pid_fn) {
            entries_[i].set_pid_fn(task->id);
        }

        debug_write("[DAEMON] '");
        debug_write(entries_[i].name);
        debug_write("' restarted (PID=");
        debug_write_hex(task->id);
        debug_write(", restart #");
        debug_write_hex(entries_[i].restart_count);
        debug_write(")\n");
        log::dmesg_push_base(0xDA02, entries_[i].name, task->id);
    }
}

/// @brief Ensure a daemon is running — reload from initrd ELF if needed.
/// @param name  Name of the daemon to check / restart.
void ensure_running(const char *name) {
    for (uint64_t i = 0; i < num_daemons_; ++i) {
        if (!entries_[i].name || strcmp(entries_[i].name, name) != 0)
            continue;

        // Already running — verify the task is still alive
        if (entries_[i].pid != 0) {
            auto *task = Scheduler::find_task(entries_[i].pid);
            if (task && task->magic == TaskControlBlock::TCB_MAGIC &&
                task->state != TaskState::TERMINATED) {
                return; // healthy
            }
            // Stale PID — fall through to restart
            entries_[i].pid = 0;
        }

        // Load ELF from initrd (same logic as restart_stale_daemons)
        initrd::InitrdFile f = initrd::find(entries_[i].initrd_path);
        if (!f.data) {
            char with_dot[256];
            with_dot[0] = '.';
            with_dot[1] = '/';
            size_t j = 0;
            while (entries_[i].initrd_path[j] && j < 254) {
                with_dot[j + 2] = entries_[i].initrd_path[j];
                ++j;
            }
            with_dot[j + 2] = '\0';
            f = initrd::find(with_dot);
        }
        if (!f.data) {
            Logger::warn(
                "daemon_mgr: ensure_running('%s') — initrd file not found",
                name);
            return;
        }

        auto *hdr = reinterpret_cast<const kernel::elf::ELF64Header *>(f.data);
        if (!kernel::elf::validate_header(hdr)) {
            Logger::warn("daemon_mgr: ensure_running('%s') — invalid ELF",
                         name);
            return;
        }

        auto *task = kernel::elf::load(hdr, f.data, f.size);
        if (!task) {
            Logger::warn("daemon_mgr: ensure_running('%s') — elf::load failed",
                         name);
            return;
        }

        task->priority = 20;
        task->base_priority = 20;
        task->period_ticks = 10;
        {
            size_t j = 0;
            while (entries_[i].name && entries_[i].name[j] &&
                   j < CONFIG_TASK_NAME_LEN - 1) {
                task->name[j] = entries_[i].name[j];
                ++j;
            }
            task->name[j] = '\0';
        }
        {
            uint64_t bg = 0;
            uint64_t budget =
                (strcmp(entries_[i].name, "iocd") == 0) ? 3ULL : 2ULL;
            task->init_sporadic_server(budget, 10, bg, 1);
        }

        Scheduler::add_task(*task);

        entries_[i].pid = task->id;
        entries_[i].restart_count = 0;
        if (entries_[i].set_pid_fn) {
            entries_[i].set_pid_fn(task->id);
        }

        debug_write("[DAEMON] '");
        debug_write(entries_[i].name);
        debug_write("' ensured (PID=");
        debug_write_hex(task->id);
        debug_write(")\n");
        log::dmesg_push_base(0xDA03, entries_[i].name, task->id);
        return;
    }

    Logger::warn("daemon_mgr: ensure_running('%s') — no daemon with that name",
                 name);
}

/// @brief Terminate a daemon by name — marks task TERMINATED and clears entry.
/// @param name  Name of the daemon to terminate.
void terminate(const char *name) {
    for (uint64_t i = 0; i < num_daemons_; ++i) {
        if (!entries_[i].name || strcmp(entries_[i].name, name) != 0)
            continue;

        if (entries_[i].pid == 0) {
            // Already dead — nothing to do
            return;
        }

        auto *task = Scheduler::find_task(entries_[i].pid);
        auto *current = Scheduler::current_task();
        if (task && task != current &&
            task->magic == TaskControlBlock::TCB_MAGIC) {
            task->state = TaskState::TERMINATED;
            task->exit_code = 0;
        }

        notify_death(entries_[i].pid); // clears PID + calls set_pid_fn(0)
        entries_[i].restart_count = 0; // allow subsequent ensure_running

        debug_write("[DAEMON] '");
        debug_write(entries_[i].name);
        debug_write("' terminated\n");
        log::dmesg_push_base(0xDA04, entries_[i].name, 0);
        return;
    }

    Logger::warn("daemon_mgr: terminate('%s') — no daemon with that name",
                 name);
}

/// @brief Reset a daemon's restart count and PID so it can be restarted.
/// @param name  Name of the daemon to reset.
void reset_restart_count(const char *name) {
    for (uint64_t i = 0; i < num_daemons_; ++i) {
        if (entries_[i].name && strcmp(entries_[i].name, name) == 0) {
            entries_[i].restart_count = 0;
            entries_[i].pid = 0;
            if (entries_[i].set_pid_fn) {
                entries_[i].set_pid_fn(0);
            }
            Logger::info("daemon_mgr: reset restart count for '%s'", name);
            return;
        }
    }
    Logger::warn("daemon_mgr: no daemon named '%s' found", name);
}

/// @brief Return a const reference to the i-th daemon entry.
/// @param index  Entry index (0-based).
/// @return DaemonEntry reference (sentinel if out of range).
const DaemonEntry &get_entry(uint64_t index) {
    if (index >= MAX_DAEMONS) {
        static DaemonEntry sentinel = {};
        return sentinel;
    }
    return entries_[index];
}

} // namespace daemon
} // namespace kernel
