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

/// @file iocd.hpp
/// @brief I/O Controller Daemon IPC interface and message types.

#include <types.hpp>

namespace kernel {
namespace iocd {

static constexpr uint64_t IOCD_REGISTER_DEVICE = 200;
static constexpr uint64_t IOCD_IRQ_EVENT = 201;
static constexpr uint64_t IOCD_MMIO_MAP = 202;
static constexpr uint64_t IOCD_MMIO_UNMAP = 203;
static constexpr uint64_t IOCD_KEYBOARD_READ = 204;
static constexpr uint64_t IOCD_SERIAL_READ = 205;
static constexpr uint64_t IOCD_SERIAL_WRITE = 206;

/// @brief IOCD IPC request message.
struct Msg {
    uint64_t type;    ///< Message type (IOCD_REGISTER_DEVICE, etc.).
    uint64_t args[7]; ///< Type-specific arguments.
};

/// @brief IOCD IPC reply message.
struct Reply {
    int64_t result;   ///< Return code (negative on error).
    uint64_t data[4]; ///< Type-specific result data.
};

/// @brief Record the PID of the IOCD (I/O Controller Daemon) task.
void set_iocd_pid(uint64_t pid);
/// @brief Get the recorded IOCD PID.
/// @return The PID, or 0 if not yet set.
uint64_t get_iocd_pid();
/// @brief Check if the current task is the IOCD.
/// @return true if the current task's PID matches the IOCD PID.
bool is_iocd_task();

} // namespace iocd
} // namespace kernel
