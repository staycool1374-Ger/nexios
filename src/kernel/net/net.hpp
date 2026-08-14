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

/// @file net.hpp
/// @brief Network stack core — NIC interface, packet dispatch.

#pragma once

#include <types.hpp>
#include <kernel/net/ether.hpp>
#include <kernel/net/arp.hpp>
#include <kernel/net/ipv4.hpp>
#include <kernel/net/udp.hpp>
#include <kernel/net/icmp.hpp>

namespace net {

constexpr size_t MAX_PACKET_SIZE = 2048;

/// @brief NIC (Network Interface Card) abstraction exposed to the network
/// stack.
struct Nic {
    const char *name; ///< Interface name (e.g. "eth0").
    MacAddr mac;      ///< This NIC's MAC address.
    Ipv4Addr ip;      ///< Assigned IPv4 address.
    Ipv4Addr subnet;  ///< Subnet mask.
    Ipv4Addr gateway; ///< Default gateway.

    /// @brief Send a raw Ethernet frame. Implemented by the NIC driver.
    bool (*send_frame)(const uint8_t *data, size_t len);

    /// @brief Poll for a received frame. Non-blocking.
    bool (*poll_frame)(uint8_t *buf, size_t &len);

    /// @brief Called when a frame arrives — set by the stack.
    void (*on_frame)(const uint8_t *data, size_t len);

    void *driver_data; ///< Driver private data.
};

/// Initialize the network stack for a NIC.
void net_init(Nic &nic, MacAddr mac, Ipv4Addr ip, Ipv4Addr subnet,
              Ipv4Addr gateway);

/// Handle an incoming Ethernet frame (called by the NIC driver).
void net_handle_frame(const uint8_t *data, size_t len, Nic &nic);

/// Send an ARP request to resolve @p target_ip.
bool net_arp_resolve(Nic &nic, uint32_t target_ip, MacAddr &out_mac);

/// Send an IPv4 + UDP packet.
/// @return true if the packet was enqueued for transmission.
bool net_send_udp(Nic &nic, Ipv4Addr dst_ip, uint16_t dst_port,
                  uint16_t src_port, const uint8_t *payload,
                  size_t payload_len);

/// Get the ARP cache (for inspection/debugging).
ArpCache &net_arp_cache();

/// Send an ICMP Echo Request.
/// @return true if the request was sent.
bool net_send_icmp_echo(Nic &nic, Ipv4Addr dst_ip, uint16_t id, uint16_t seq,
                        const uint8_t *data, size_t data_len);

/// Globally registered NIC — now owned by kernel::gs (get_nic/try_set_nic).

/// @brief ICMP echo reply record for ping.
struct IcmpEchoReply {
    bool received;    ///< Whether a reply has been received.
    uint16_t ident;   ///< Echo identifier (big-endian).
    uint16_t seq;     ///< Sequence number (big-endian).
    uint64_t rx_tick; ///< Tick count at receipt.
    Ipv4Addr src;     ///< Source IPv4 address.
};

/// Reset the ICMP echo reply record.
void net_icmp_clear_reply();
void net_icmp_set_reply(uint16_t ident, uint16_t seq, Ipv4Addr src);
/// Get the last ICMP echo reply received.
const IcmpEchoReply *net_icmp_last_reply();

/// Poll the NIC for an incoming frame and dispatch it.
/// Non-blocking — returns false if nothing available.
bool net_poll(Nic &nic);

} // namespace net
