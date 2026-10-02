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

/// @file watchdogd.hpp
/// @brief Watchdog daemon IPC message types and PID tracking (issue #277).

#pragma once

#include <types.hpp>

namespace kernel {

struct TaskControlBlock;

namespace watchdogd {

static constexpr uint64_t WDOG_SUPERVISE = 300;
static constexpr uint64_t WDOG_STATUS = 301;
static constexpr uint64_t WDOG_DISARM = 302;

struct Msg {
    uint64_t sender_id; ///< Task ID of the sender.
    uint64_t type;      ///< Request type (WDOG_*).
    uint64_t arg0;      ///< First argument (varies by type).
    uint64_t arg1;      ///< Second argument (varies by type).
};

struct Reply {
    int64_t result; ///< Return code (0 on success, negative on error).
    uint64_t data0; ///< Response data field 0.
    uint64_t data1; ///< Response data field 1.
};

/// @brief Record the PID of the watchdog daemon task.
void set_watchdogd_pid(uint64_t pid);
/// @brief Get the recorded watchdog daemon PID.
/// @return The PID, or 0 if not yet set.
uint64_t get_watchdogd_pid();
/// @brief Check if the current task is the watchdog daemon.
/// @return true if the current task's PID matches the daemon PID.
bool is_watchdogd_task();

/// @brief Mint WdogCap supervision authorities into watchdogd's CSpace.
///        Called once from init_task_main after watchdogd reports READY:
///        creates one WdogCap per supervised pid and installs each into
///        watchdogd's CSpace with WRITE right.
/// @param supervised_pids Array of supervised task IDs.
/// @param count Number of entries (bounded by the caller's array).
/// @param out_handles Output encoded handles, one per pid (-1 on failure).
void grant_supervision(const uint64_t *supervised_pids, size_t count,
                       uint64_t *out_handles);

} // namespace watchdogd
} // namespace kernel
