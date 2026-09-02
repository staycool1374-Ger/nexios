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

/// @file net.cpp
/// @brief Network stack core implementation.

#include <kernel/net/net.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <string.hpp>
#include <logger.hpp>

using namespace kernel;

namespace net {

// M-3 (audit-drivers-vfs-net-v0.4.2): the RX path (net_poll / net_handle_frame)
// and the send/query paths (net_send_udp, net_arp_resolve) run in task context
// on a single core, but they interleave through cooperative polling — guard
// the shared mutable state with a module spinlock so a poll-driven RX can never
// tear an ARP-cache update or the ICMP reply record mid-read/write.
static sync::SpinLock g_net_lock{};

static ArpCache g_arp_cache{};
static uint16_t g_ip_ident = 0;

// Last ICMP echo reply (for ping)
static IcmpEchoReply g_icmp_reply{};

/// @brief Reset the ICMP echo reply record.
void net_icmp_clear_reply() {
    SpinLockGuard<sync::SpinLock> guard(g_net_lock);
    g_icmp_reply.received = false;
}

/// @brief Record an ICMP echo reply.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void net_icmp_set_reply(uint16_t ident, uint16_t seq, Ipv4Addr src) {
    SpinLockGuard<sync::SpinLock> guard(g_net_lock);
    g_icmp_reply.received = true;
    g_icmp_reply.ident = ident;
    g_icmp_reply.seq = seq;
    g_icmp_reply.rx_tick = arch::Timer::ticks();
    g_icmp_reply.src = src;
}

/// @brief Return a pointer to the last received ICMP echo reply, or nullptr.
const IcmpEchoReply *net_icmp_last_reply() {
    // The caller uses the returned record immediately (poll-driven RX in the
    // same task context); the lock orders the write before this read.
    SpinLockGuard<sync::SpinLock> guard(g_net_lock);
    return g_icmp_reply.received ? &g_icmp_reply : nullptr;
}

/// @brief Return a reference to the global ARP cache.
ArpCache &net_arp_cache() {
    return g_arp_cache;
}

/// @brief Initialise a NIC with its MAC, IP, subnet, and gateway.
void net_init(Nic &nic, MacAddr mac, Ipv4Addr ip, Ipv4Addr subnet,
              Ipv4Addr gateway) {
    nic.mac = mac;
    nic.ip = ip;
    nic.subnet = subnet;
    nic.gateway = gateway;
    {
        SpinLockGuard<sync::SpinLock> guard(g_net_lock);
        g_arp_cache.clear();
    }
    kernel::gs::try_set_nic(&nic);
    Logger::info("net: initialized %d.%d.%d.%d", ip.addr[0], ip.addr[1],
                 ip.addr[2], ip.addr[3]);
}

/// @brief Handle an incoming Ethernet frame: dispatch ARP / IPv4 / ICMP.
void net_handle_frame(const uint8_t *data, size_t len, Nic &nic) {
    if (len < sizeof(EtherHeader))
        return;
    auto *eth = reinterpret_cast<const EtherHeader *>(data);
    uint16_t type = __builtin_bswap16(eth->type);

    if (type == ETH_TYPE_ARP) {
        if (len < sizeof(EtherHeader) + sizeof(ArpHeader))
            return;
        auto *arp =
            reinterpret_cast<const ArpHeader *>(data + sizeof(EtherHeader));

        if (arp->htype != __builtin_bswap16(ARP_HTYPE_ETHER))
            return;
        if (arp->ptype != __builtin_bswap16(ETH_TYPE_IPV4))
            return;

        uint16_t oper = __builtin_bswap16(arp->oper);

        if (oper == ARP_OPER_REQUEST) {
            // Check if the target IP is ours
            if (arp->tpa == nic.ip.as_u32()) {
                // Send ARP reply
                uint8_t reply_buf[sizeof(EtherHeader) + sizeof(ArpHeader)];
                auto *reply_eth = reinterpret_cast<EtherHeader *>(reply_buf);
                auto *reply_arp = reinterpret_cast<ArpHeader *>(
                    reply_buf + sizeof(EtherHeader));

                reply_eth->dst = eth->src;
                reply_eth->src = nic.mac;
                reply_eth->type = __builtin_bswap16(ETH_TYPE_ARP);

                reply_arp->htype = arp->htype;
                reply_arp->ptype = arp->ptype;
                reply_arp->hlen = ETH_ADDR_LEN;
                reply_arp->plen = IPV4_ADDR_LEN;
                reply_arp->oper = __builtin_bswap16(ARP_OPER_REPLY);
                reply_arp->sha = nic.mac;
                reply_arp->spa = arp->tpa;
                reply_arp->tha = arp->sha;
                reply_arp->tpa = arp->spa;

                nic.send_frame(reply_buf, sizeof(reply_buf));
            }
        } else if (oper == ARP_OPER_REPLY) {
            // Update ARP cache
            SpinLockGuard<sync::SpinLock> guard(g_net_lock);
            g_arp_cache.update(arp->spa, arp->sha);
        }
    } else if (type == ETH_TYPE_IPV4) {
        if (len < sizeof(EtherHeader) + sizeof(Ipv4Header))
            return;
        auto *ip =
            reinterpret_cast<const Ipv4Header *>(data + sizeof(EtherHeader));
        size_t ip_hdr_len = static_cast<size_t>(ip->ver_ihl & 0x0F) * 4;
        if (ip_hdr_len < IPV4_MIN_HEADER_LEN)
            return;

        if (ip->protocol == IP_PROTO_ICMP) {
            size_t ip_total_len = __builtin_bswap16(ip->total_length);
            if (len < sizeof(EtherHeader) + ip_total_len)
                return;
            size_t icmp_offset = sizeof(EtherHeader) + ip_hdr_len;
            size_t icmp_len = ip_total_len - ip_hdr_len;
            if (icmp_len < ICMP_HEADER_LEN)
                return;

            auto *icmp =
                reinterpret_cast<const IcmpHeader *>(data + icmp_offset);

            if (icmp->type == ICMP_TYPE_ECHO_REPLY) {
                SpinLockGuard<sync::SpinLock> guard(g_net_lock);
                g_icmp_reply.received = true;
                g_icmp_reply.ident = icmp->ident;
                g_icmp_reply.seq = icmp->seq;
                g_icmp_reply.rx_tick = arch::Timer::ticks();
                g_icmp_reply.src = ip->src;
            }
        }
    }
}

/// @brief Resolve an IPv4 address to a MAC via ARP (with retries and cache).
bool net_arp_resolve(Nic &nic, uint32_t target_ip, MacAddr &out_mac) {
    // Check cache first
    if (g_arp_cache.lookup(target_ip, out_mac))
        return true;

    // Build ARP request once
    uint8_t buf[sizeof(EtherHeader) + sizeof(ArpHeader)];
    auto *eth = reinterpret_cast<EtherHeader *>(buf);
    auto *arp = reinterpret_cast<ArpHeader *>(buf + sizeof(EtherHeader));

    eth->dst = MAC_BROADCAST;
    eth->src = nic.mac;
    eth->type = __builtin_bswap16(ETH_TYPE_ARP);

    arp->htype = __builtin_bswap16(ARP_HTYPE_ETHER);
    arp->ptype = __builtin_bswap16(ETH_TYPE_IPV4);
    arp->hlen = ETH_ADDR_LEN;
    arp->plen = IPV4_ADDR_LEN;
    arp->oper = __builtin_bswap16(ARP_OPER_REQUEST);
    arp->sha = nic.mac;
    arp->spa = nic.ip.as_u32();
    arp->tha = MAC_NULL;
    arp->tpa = target_ip;

    for (int attempt = 0; attempt < 3; ++attempt) {
        nic.send_frame(buf, sizeof(buf));
        uint64_t deadline = arch::Timer::ticks() + 50;
        while (arch::Timer::ticks() < deadline) {
            for (int p = 0; p < 5; ++p)
                if (net_poll(nic))
                    break;
            if (g_arp_cache.lookup(target_ip, out_mac))
                return true;
            arch::pause();
        }
    }

    return false;
}

/// @brief Build and send an IPv4+UDP packet.
bool net_send_udp(Nic &nic, Ipv4Addr dst_ip, uint16_t dst_port,
                  uint16_t src_port, const uint8_t *payload,
                  size_t payload_len) {
    // Resolve MAC via ARP
    MacAddr dst_mac{};
    if (!net_arp_resolve(nic, dst_ip.as_u32(), dst_mac)) {
        Logger::warn("net: ARP resolution failed for %d.%d.%d.%d",
                     dst_ip.addr[0], dst_ip.addr[1], dst_ip.addr[2],
                     dst_ip.addr[3]);
        return false;
    }

    size_t ip_payload_len = sizeof(UdpHeader) + payload_len;
    size_t total_ip_len = IPV4_MIN_HEADER_LEN + ip_payload_len;
    size_t frame_len = sizeof(EtherHeader) + total_ip_len;

    if (frame_len > MAX_PACKET_SIZE) {
        Logger::error("net: packet too large (%zu)", frame_len);
        return false;
    }

    uint8_t buf[MAX_PACKET_SIZE];
    memset(buf, 0, MAX_PACKET_SIZE);

    // Ethernet header
    auto *eth = reinterpret_cast<EtherHeader *>(buf);
    eth->dst = dst_mac;
    eth->src = nic.mac;
    eth->type = __builtin_bswap16(ETH_TYPE_IPV4);

    // IPv4 header
    auto *ip = reinterpret_cast<Ipv4Header *>(buf + sizeof(EtherHeader));
    ip->ver_ihl = 0x45; // IPv4, 20-byte header
    ip->dscp_ecn = 0;
    ip->total_length = __builtin_bswap16(static_cast<uint16_t>(total_ip_len));
    // M-3: atomic IP identification — the RX/send paths interleave through
    // cooperative polling and a plain post-increment could duplicate values.
    ip->ident = __builtin_bswap16(static_cast<uint16_t>(
        __atomic_fetch_add(&g_ip_ident, 1U, __ATOMIC_RELAXED)));
    ip->flags_frag = __builtin_bswap16(0x4000); // Don't Fragment
    ip->ttl = 64;
    ip->protocol = IP_PROTO_UDP;
    ip->checksum = 0;
    ip->src = nic.ip;
    ip->dst = dst_ip;
    ip->checksum = ipv4_checksum(ip);

    // UDP header
    auto *udp = reinterpret_cast<UdpHeader *>(buf + sizeof(EtherHeader) +
                                              IPV4_MIN_HEADER_LEN);
    udp->src_port = __builtin_bswap16(src_port);
    udp->dst_port = __builtin_bswap16(dst_port);
    udp->length = __builtin_bswap16(
        static_cast<uint16_t>(sizeof(UdpHeader) + payload_len));
    udp->checksum = 0; // UDP checksum is optional (0 = no checksum)

    // Payload
    if (payload_len > 0) {
        memcpy(buf + sizeof(EtherHeader) + IPV4_MIN_HEADER_LEN +
                   sizeof(UdpHeader),
               payload, payload_len);
    }

    return nic.send_frame(buf, frame_len);
}

/// @brief Poll the NIC for an incoming frame and dispatch it (non-blocking).
bool net_poll(Nic &nic) {
    if (!nic.poll_frame)
        return false;
    uint8_t buf[MAX_PACKET_SIZE];
    size_t len = 0;
    if (!nic.poll_frame(buf, len))
        return false;
    net_handle_frame(buf, len, nic);
    return true;
}

/// @brief Build and send an ICMP Echo Request (with self-ping loopback).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool net_send_icmp_echo(Nic &nic, Ipv4Addr dst_ip, uint16_t id, uint16_t seq,
                        const uint8_t *data, size_t data_len) {
    // Loopback / self-ping: reflect instantly
    if (dst_ip.as_u32() == 0x7F000001 || dst_ip.as_u32() == nic.ip.as_u32()) {
        SpinLockGuard<sync::SpinLock> guard(g_net_lock);
        g_icmp_reply.received = true;
        g_icmp_reply.ident = id;
        g_icmp_reply.seq = seq;
        g_icmp_reply.rx_tick = arch::Timer::ticks();
        g_icmp_reply.src = dst_ip;
        return true;
    }

    MacAddr dst_mac{};
    if (!net_arp_resolve(nic, dst_ip.as_u32(), dst_mac)) {
        return false;
    }

    size_t icmp_total = ICMP_HEADER_LEN + data_len;
    size_t ip_total = IPV4_MIN_HEADER_LEN + icmp_total;
    size_t frame_len = sizeof(EtherHeader) + ip_total;

    if (frame_len > MAX_PACKET_SIZE) {
        Logger::error("net: ping packet too large (%zu)", frame_len);
        return false;
    }

    uint8_t buf[MAX_PACKET_SIZE];
    memset(buf, 0, MAX_PACKET_SIZE);

    // Ethernet
    auto *eth = reinterpret_cast<EtherHeader *>(buf);
    eth->dst = dst_mac;
    eth->src = nic.mac;
    eth->type = __builtin_bswap16(ETH_TYPE_IPV4);

    // IPv4
    auto *ip = reinterpret_cast<Ipv4Header *>(buf + sizeof(EtherHeader));
    ip->ver_ihl = 0x45;
    ip->dscp_ecn = 0;
    ip->total_length = __builtin_bswap16(static_cast<uint16_t>(ip_total));
    ip->ident = __builtin_bswap16(static_cast<uint16_t>(
        __atomic_fetch_add(&g_ip_ident, 1U, __ATOMIC_RELAXED)));
    ip->flags_frag = __builtin_bswap16(0x4000);
    ip->ttl = 64;
    ip->protocol = IP_PROTO_ICMP;
    ip->checksum = 0;
    ip->src = nic.ip;
    ip->dst = dst_ip;
    ip->checksum = ipv4_checksum(ip);

    // ICMP header
    auto *icmp = reinterpret_cast<IcmpHeader *>(buf + sizeof(EtherHeader) +
                                                IPV4_MIN_HEADER_LEN);
    icmp->type = ICMP_TYPE_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->ident = id;
    icmp->seq = seq;

    // Payload
    if (data_len > 0) {
        memcpy(buf + sizeof(EtherHeader) + IPV4_MIN_HEADER_LEN +
                   ICMP_HEADER_LEN,
               data, data_len);
    }

    icmp->checksum =
        icmp_checksum(reinterpret_cast<const uint8_t *>(icmp), icmp_total);

    return nic.send_frame(buf, frame_len);
}

} // namespace net
