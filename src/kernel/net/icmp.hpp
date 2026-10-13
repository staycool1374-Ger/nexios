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

/// @file icmp.hpp
/// @brief ICMP (Internet Control Message Protocol) — echo request/reply types
/// and checksum.

#include <types.hpp>

namespace net {

/// ICMP Echo Reply message type.
constexpr uint8_t ICMP_TYPE_ECHO_REPLY = 0;
/// ICMP Echo Request message type.
constexpr uint8_t ICMP_TYPE_ECHO_REQUEST = 8;

/// Size of the ICMP header (type + code + checksum + ident + seq).
constexpr size_t ICMP_HEADER_LEN = 8;

/// @brief ICMP echo request/reply header (8 bytes).
struct IcmpHeader {
    uint8_t type;      ///< Message type (ECHO_REQUEST = 8, ECHO_REPLY = 0).
    uint8_t code;      ///< Code (0 for echo).
    uint16_t checksum; ///< ICMP header + data checksum (big-endian).
    uint16_t ident;    ///< Identifier (big-endian).
    uint16_t seq;      ///< Sequence number (big-endian).
} __attribute__((packed));

/// @brief Compute a 16-bit one's-complement checksum over @p len bytes.
/// Used for ICMP and IPv4 header checksums.
inline uint16_t icmp_checksum(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    auto *words = reinterpret_cast<const uint16_t *>(data);
    size_t word_count = len / 2;
    for (size_t i = 0; i < word_count; ++i) {
        sum += words[i];
    }
    if (len & 1) {
        sum += static_cast<uint16_t>(data[len - 1]) << 8;
    }
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~sum & 0xFFFF);
}

} // namespace net
