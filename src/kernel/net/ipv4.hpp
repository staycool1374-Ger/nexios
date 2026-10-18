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

/// @file ipv4.hpp
/// @brief IPv4 protocol types and helpers.

#include <types.hpp>

namespace net {

constexpr size_t IPV4_ADDR_LEN = 4;
constexpr size_t IPV4_MIN_HEADER_LEN = 20;
constexpr size_t IPV4_MAX_HEADER_LEN = 60;

/// @brief IPv4 address (4 bytes, network-byte-order).
struct Ipv4Addr {
    uint8_t addr[IPV4_ADDR_LEN]; ///< Octets in network order.

    bool operator==(const Ipv4Addr &o) const {
        for (size_t i = 0; i < IPV4_ADDR_LEN; ++i)
            if (addr[i] != o.addr[i])
                return false;
        return true;
    }

    uint32_t as_u32() const {
        return (static_cast<uint32_t>(addr[0]) << 24) |
               (static_cast<uint32_t>(addr[1]) << 16) |
               (static_cast<uint32_t>(addr[2]) << 8) |
               static_cast<uint32_t>(addr[3]);
    }

    static Ipv4Addr from_u32(uint32_t v) {
        Ipv4Addr a;
        a.addr[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
        a.addr[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
        a.addr[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
        a.addr[3] = static_cast<uint8_t>(v & 0xFF);
        return a;
    }
} __attribute__((packed));

/// @brief IPv4 header (20 bytes, no options).
struct Ipv4Header {
    uint8_t ver_ihl;       ///< Version (4) | header length (5 = 20 bytes).
    uint8_t dscp_ecn;      ///< DSCP + ECN.
    uint16_t total_length; ///< Total packet length (big-endian).
    uint16_t ident;        ///< Identification (big-endian).
    uint16_t flags_frag;   ///< Flags + fragment offset (big-endian).
    uint8_t ttl;           ///< Time to live.
    uint8_t protocol;      ///< Protocol (1 = ICMP, 6 = TCP, 17 = UDP).
    uint16_t checksum;     ///< Header checksum (big-endian).
    Ipv4Addr src;          ///< Source address.
    Ipv4Addr dst;          ///< Destination address.
} __attribute__((packed));

/// @brief IPv4 protocol number constants.
enum Ipv4Protocol : uint8_t {
    IP_PROTO_ICMP = 1, ///< ICMP.
    IP_PROTO_TCP = 6,  ///< TCP.
    IP_PROTO_UDP = 17, ///< UDP.
};

/// @brief Compute an IPv4 header one's-complement checksum.
/// @return Big-endian checksum value.
inline uint16_t ipv4_checksum(const Ipv4Header *hdr) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(hdr);
    uint32_t sum = 0;
    uint16_t word = 0;
    for (int i = 0; i < 20; i += 2) { // first 20 bytes = 10 half-words
        __builtin_memcpy(&word, bytes + i, sizeof(word));
        sum += word;
    }
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~sum & 0xFFFF);
}

} // namespace net
