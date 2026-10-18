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

/// @file udp.hpp
/// @brief UDP protocol types and helpers.

#include <types.hpp>

namespace net {

/// @brief UDP header (8 bytes).
struct UdpHeader {
    uint16_t src_port; ///< Source port (big-endian).
    uint16_t dst_port; ///< Destination port (big-endian).
    uint16_t length;   ///< Header + data length (big-endian).
    uint16_t checksum; ///< Optional checksum (big-endian; 0 = no checksum).
} __attribute__((packed));

} // namespace net
