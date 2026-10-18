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
static constexpr uint64_t IOCD_UNREGISTER_DEVICE = 207;

/// @brief IOCD IPC request message.
struct Msg {
    uint64_t type;    ///< Message type (IOCD_REGISTER_DEVICE, etc.).
    uint64_t args[7]; ///< Type-specific arguments.
};

/// @brief MMIO policy protocol (issue #303 item 4). The daemon authorizes
///        WHICH registered driver may use WHICH bar index; the kernel
///        still enforces WHAT gets mapped via MEMORY-type MmioCaps, and
///        MmioUserMap::drain_task owns PTE teardown on task exit.
///        - MMIO_MAP: args[0]=client pid (self-asserted, same trust model
///          as KEYBOARD_READ), args[1]=bar index in [0, 4). Reply 0 on
///          grant (idempotent reuse), -1 on unregistered pid, out-of-range
///          bar, or full grant table.
///        - MMIO_UNMAP: args[0]=client pid, args[1]=bar index. Reply 0 on
///          release, -1 when no live record exists.
///        - UNREGISTER_DEVICE: args[0]=pid. Removes the registration and
///          drops its grant records (device exit path); -1 if unknown.
///        Routing caveat: the daemon replies to args[0], never to the IPC
///        sender (same self-asserted trust model as KEYBOARD_READ). A
///        request with a bogus args[0] strands the requester's send_sync
///        — callers must always pass their own live pid.

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
/// @return true if the current task's PID matches the recorded IOCD PID.
bool is_iocd_task();
/// @brief Keyboard ownership for input routing (issue #340 P1).
/// False (default) = legacy: kernel handle_irq pushes its ring, shell
/// polls hardware. True = daemon-owned: handle_irq forwards raw
/// scancodes to iocd (never both — single-ownership invariant).
/// Set only by explicit consumers (shell, tests); fail-open per
/// keystroke on daemon-down/ISR-context regardless of the flag.
/// Snapshot-covered (test isolation restores it like daemon PIDs).
bool is_keyboard_owned();
/// @brief Set or clear keyboard ownership.
/// @param owned True to route keyboard bytes through iocd.
void set_keyboard_owned(bool owned);

} // namespace iocd
} // namespace kernel
