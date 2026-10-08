#pragma once

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

/// @file daemon_mgr.hpp
/// @brief Daemon lifecycle manager — registration, spawning, death
/// notification, and restart.

#pragma once

#include <types.hpp>

namespace kernel {
namespace daemon {

static constexpr uint64_t MAX_DAEMONS = CONFIG_MAX_DAEMONS;
static constexpr uint64_t MAX_RESTART_COUNT = 10;

/// @brief Entry for a single managed daemon.
struct DaemonEntry {
    const char *name;             ///< Human-readable daemon name.
    const char *initrd_path;      ///< Path to the ELF in the initrd.
    uint64_t pid;                 ///< Current PID (0 if not running).
    uint64_t restart_count;       ///< Number of automatic restarts performed.
    void (*set_pid_fn)(uint64_t); ///< Callback to store the PID.
    uint64_t (*get_pid_fn)();     ///< Callback to retrieve the current PID.
};

/// @brief Initialize the daemon manager and spawn registered daemons.
void init();

/// @brief Kill and re-register all daemons (used during recovery).
/// Also resets num_daemons_ to 0 so fresh registrations work.
void reset_clear_daemons();
/// @brief Register a daemon for lifecycle management.
/// @param name Human-readable name for the daemon.
/// @param initrd_path Path to the daemon ELF in the initrd.
/// @param set_pid Callback to store the PID when the daemon is spawned.
/// @param get_pid Callback to retrieve the daemon's current PID.
/// @return true if registered successfully.
bool register_daemon(const char *name, const char *initrd_path,
                     void (*set_pid)(uint64_t), uint64_t (*get_pid)());
/// @brief Notify the daemon manager that a daemon has exited.
/// @param pid      PID of the daemon that died.
/// @param log_only If true, skip dmesg push (test isolation cleanup).
void notify_death(uint64_t pid, bool log_only = false);
/// @brief Suppress or re-enable the "[DAEMON] ... died" serial message.
///        Used by reboot_from_table() to avoid noise during teardown.
void set_suppress_death_msg(bool v);
/// @brief Restart any daemons that have died (up to MAX_RESTART_COUNT each).
void restart_stale_daemons();

/// @brief Ensure a daemon is running.  If its PID is 0 or the task is no
///        longer valid, reload it from the initrd ELF immediately.
void ensure_running(const char *name);

/// @brief Terminate a daemon by name.  Marks the task as TERMINATED and
///        clears the daemon entry so ensure_running will reload it.
void terminate(const char *name);

/// @brief Reset a daemon's restart count so it can be restarted again.
///        Sets restart_count=0, pid=0, and calls set_pid_fn(0).
void reset_restart_count(const char *name);

/// @brief Return a const reference to the i-th daemon entry.
const DaemonEntry &get_entry(uint64_t index);

/// @brief True when @p pid is a currently managed daemon task.
/// Used by snapshot-restore to preserve daemon CSpaces (persistent
/// system state holding grants): test fixtures must never break them.
bool is_managed(uint64_t pid);

/// @name Test-isolation helpers
/// @brief Copy all daemon entries and count into @p entries_out / @p num_out.
void capture_state(DaemonEntry *out_entries, uint64_t &num_out);
/// @brief Restore daemon entries and count from @p entries_in / @p num_in.
void restore_state(const DaemonEntry *entries_in, uint64_t num_in);

} // namespace daemon
} // namespace kernel
