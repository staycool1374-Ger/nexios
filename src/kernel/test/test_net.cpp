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

/// @file test_net.cpp
/// @brief Networking subsystem tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/net/ether.hpp>
#include <kernel/net/arp.hpp>
#include <kernel/net/ipv4.hpp>
#include <kernel/net/udp.hpp>
#include <kernel/net/net.hpp>
#include <string.hpp>

using namespace kernel;

namespace {

// Test NIC addresses.
const net::MacAddr k_our_mac = {{0x52, 0x54, 0x00, 0xAA, 0xBB, 0xCC}};
const net::MacAddr k_peer_mac = {{0x52, 0x54, 0x00, 0x11, 0x22, 0x33}};
// 10.0.0.x in network order (as_u32 compares host-order words).
constexpr uint32_t k_our_ip = 0x0A000001;
constexpr uint32_t k_peer_ip = 0x0A000002;
constexpr uint32_t k_other_ip = 0x0A000099;

/// @brief Last frame handed to the mock send hook + send count.
uint8_t g_sent[net::MAX_PACKET_SIZE] = {};
size_t g_sent_len = 0;
uint64_t g_sends = 0;

/// @brief One-shot poll hook: delivers a single ARP reply, then goes dry.
bool g_poll_armed = false;

bool mock_send_frame(const uint8_t *data, size_t len) {
    g_sends += 1;
    g_sent_len = len < sizeof(g_sent) ? len : sizeof(g_sent);
    for (size_t i = 0; i < g_sent_len; ++i)
        g_sent[i] = data[i];
    return true;
}

bool mock_poll_empty(uint8_t * /*buf*/, size_t & /*len*/) {
    return false;
}

/// @brief Mock NIC with no hardware behind it.  Constructed manually —
///        net_init() is deliberately NOT used (it would overwrite the
///        global NIC registration via gs::try_set_nic).
net::Nic make_mock_nic() {
    net::Nic nic{};
    nic.name = "mock";
    nic.mac = k_our_mac;
    nic.ip = net::Ipv4Addr::from_u32(k_our_ip);
    nic.subnet = net::Ipv4Addr::from_u32(0xFFFFFF00);
    nic.gateway = net::Ipv4Addr::from_u32(0x0A000001);
    nic.send_frame = mock_send_frame;
    nic.poll_frame = mock_poll_empty;
    nic.on_frame = nullptr;
    nic.driver_data = nullptr;
    return nic;
}

void mock_reset() {
    g_sent_len = 0;
    g_sends = 0;
    g_poll_armed = false;
    for (size_t i = 0; i < sizeof(g_sent); ++i)
        g_sent[i] = 0;
    net::net_arp_cache().clear();
    net::net_icmp_clear_reply();
}

void build_arp_request(uint8_t *frame, const net::MacAddr &sha,
                       uint32_t spa, uint32_t tpa) {
    auto *eth = reinterpret_cast<net::EtherHeader *>(frame);
    eth->dst = net::MAC_BROADCAST;
    eth->src = sha;
    eth->type = __builtin_bswap16(net::ETH_TYPE_ARP);
    auto *arp = reinterpret_cast<net::ArpHeader *>(frame +
                                                  sizeof(net::EtherHeader));
    arp->htype = __builtin_bswap16(net::ARP_HTYPE_ETHER);
    arp->ptype = __builtin_bswap16(net::ETH_TYPE_IPV4);
    arp->hlen = net::ETH_ADDR_LEN;
    arp->plen = net::IPV4_ADDR_LEN;
    arp->oper = __builtin_bswap16(net::ARP_OPER_REQUEST);
    arp->sha = sha;
    arp->spa = spa;
    arp->tha = net::MAC_NULL;
    arp->tpa = tpa;
}

void build_arp_reply(uint8_t *frame, const net::MacAddr &sha, uint32_t spa) {
    auto *eth = reinterpret_cast<net::EtherHeader *>(frame);
    eth->dst = k_our_mac;
    eth->src = sha;
    eth->type = __builtin_bswap16(net::ETH_TYPE_ARP);
    auto *arp = reinterpret_cast<net::ArpHeader *>(frame +
                                                  sizeof(net::EtherHeader));
    arp->htype = __builtin_bswap16(net::ARP_HTYPE_ETHER);
    arp->ptype = __builtin_bswap16(net::ETH_TYPE_IPV4);
    arp->hlen = net::ETH_ADDR_LEN;
    arp->plen = net::IPV4_ADDR_LEN;
    arp->oper = __builtin_bswap16(net::ARP_OPER_REPLY);
    arp->sha = sha;
    arp->spa = spa;
    arp->tha = k_our_mac;
    arp->tpa = k_our_ip;
}

/// @brief One-shot poll hook: delivers a single ARP reply, then goes dry.
bool mock_poll_once(uint8_t *buf, size_t &len) {
    if (!g_poll_armed)
        return false;
    g_poll_armed = false;
    build_arp_reply(buf, k_peer_mac, k_peer_ip);
    len = sizeof(net::EtherHeader) + sizeof(net::ArpHeader);
    return true;
}

} // namespace

// Runmode: kernel
// Testidea: Verifies MacAddr equality and broadcast/null detection.
// Input: Compare various MAC addresses
// Expect: Equality works, broadcast/null detection works
// Depends: net::MacAddr
JARVIS_TEST(net_mac_address_ops, "PRE: none | POST: none") {
    net::MacAddr a = {{0x52, 0x54, 0x00, 0x12, 0x34, 0x56}};
    net::MacAddr b = {{0x52, 0x54, 0x00, 0x12, 0x34, 0x56}};
    net::MacAddr c = {{0x52, 0x54, 0x00, 0x12, 0x34, 0x57}};
    JARVIS_ASSERT(a == b);
    JARVIS_ASSERT(a != c);
    JARVIS_ASSERT(!a.is_broadcast());
    JARVIS_ASSERT(!a.is_null());
    JARVIS_ASSERT(net::MAC_BROADCAST.is_broadcast());
    JARVIS_ASSERT(net::MAC_NULL.is_null());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies Ipv4Addr from_u32 and as_u32 round-trip.
// Input: net::Ipv4Addr::from_u32(0xC0A80101) -> as_u32()
// Expect: as_u32 returns 0xC0A80101
// Depends: net::Ipv4Addr
JARVIS_TEST(net_ipv4_addr_roundtrip, "PRE: none | POST: none") {
    uint32_t ip = 0xC0A80101; // 192.168.1.1
    net::Ipv4Addr addr = net::Ipv4Addr::from_u32(ip);
    JARVIS_ASSERT_EQ(ip, addr.as_u32());
    JARVIS_ASSERT_EQ(192, addr.addr[0]);
    JARVIS_ASSERT_EQ(168, addr.addr[1]);
    JARVIS_ASSERT_EQ(1, addr.addr[2]);
    JARVIS_ASSERT_EQ(1, addr.addr[3]);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies ARP cache add, lookup, remove, and clear operations.
// Input: net::ArpCache
// Expect: add + lookup returns correct entry, remove removes it, clear clears
// all Depends: net::ArpCache, net::MacAddr
JARVIS_TEST(net_arp_cache_ops, "PRE: none | POST: none") {
    net::ArpCache cache;
    net::MacAddr mac = {{0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01}};
    uint32_t ip = 0xC0A80164; // 192.168.1.100

    net::MacAddr out;
    JARVIS_ASSERT(!cache.lookup(ip, out));

    cache.update(ip, mac);
    JARVIS_ASSERT(cache.lookup(ip, out));
    JARVIS_ASSERT(out == mac);

    cache.remove(ip);
    JARVIS_ASSERT(!cache.lookup(ip, out));

    cache.update(ip, mac);
    cache.clear();
    JARVIS_ASSERT(!cache.lookup(ip, out));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies IPv4 header checksum calculation on a known header.
// Input: Build a minimal IPv4 header, compute checksum
// Expect: Checksum matches known value
// Depends: net::ipv4_checksum
JARVIS_TEST(net_ipv4_checksum, "PRE: none | POST: none") {
    net::Ipv4Header hdr = {};
    hdr.ver_ihl = 0x45;
    hdr.total_length = __builtin_bswap16(20);
    hdr.ident = __builtin_bswap16(1);
    hdr.flags_frag = __builtin_bswap16(0x4000);
    hdr.ttl = 64;
    hdr.protocol = 17; // UDP
    hdr.src = net::Ipv4Addr::from_u32(0xC0A80101);
    hdr.dst = net::Ipv4Addr::from_u32(0xC0A80102);
    hdr.checksum = 0;

    uint16_t sum = net::ipv4_checksum(&hdr);
    hdr.checksum = sum;

    // Verify: checksum of the complete header should be 0
    uint16_t verify = net::ipv4_checksum(&hdr);
    JARVIS_ASSERT_EQ(0, verify);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that __builtin_bswap16 correctly swaps EtherType
// constants for wire transmission (x86 is little-endian).
// Input: ETH_TYPE_IPV4 = 0x0800, ETH_TYPE_ARP = 0x0806
// Expect: After bswap16, the values are in little-endian for x86 memory
// Depends: __builtin_bswap16
JARVIS_TEST(net_ether_type_swap, "PRE: none | POST: none") {
    uint16_t ipv4_le = __builtin_bswap16(net::ETH_TYPE_IPV4);
    JARVIS_ASSERT_EQ((uint16_t)0x0008, ipv4_le);
    uint16_t arp_le = __builtin_bswap16(net::ETH_TYPE_ARP);
    JARVIS_ASSERT_EQ((uint16_t)0x0608, arp_le);
    JARVIS_TEST_PASS();
}



// Runmode: kernel
// Testidea: An ARP request for our IP must be answered with a well-formed
//           reply (oper=REPLY, our MAC as sha, requester as tha/dst).
// Input: Mock NIC; handle_frame(ARP request, tpa = our IP).
// Expect: Exactly one send; reply oper REPLY, tha/dst = requester,
//         spa = our IP.
// Depends: net_handle_frame ARP-request path
JARVIS_TEST(net_arp_request_for_us_sends_reply, "PRE: none | POST: none") {
    mock_reset();
    net::Nic nic = make_mock_nic();
    uint8_t frame[sizeof(net::EtherHeader) + sizeof(net::ArpHeader)] = {};
    build_arp_request(frame, k_peer_mac, k_peer_ip, k_our_ip);

    net::net_handle_frame(frame, sizeof(frame), nic);

    JARVIS_ASSERT_EQ(1ULL, g_sends);
    JARVIS_ASSERT_EQ(sizeof(frame), g_sent_len);
    auto *eth =
        reinterpret_cast<const net::EtherHeader *>(g_sent);
    auto *arp = reinterpret_cast<const net::ArpHeader *>(
        g_sent + sizeof(net::EtherHeader));
    JARVIS_ASSERT(eth->dst == k_peer_mac);
    JARVIS_ASSERT(eth->src == k_our_mac);
    JARVIS_ASSERT_EQ(__builtin_bswap16(net::ARP_OPER_REPLY), arp->oper);
    JARVIS_ASSERT(arp->sha == k_our_mac);
    JARVIS_ASSERT_EQ(k_our_ip, arp->spa);
    JARVIS_ASSERT(arp->tha == k_peer_mac);
    JARVIS_ASSERT_EQ(k_peer_ip, arp->tpa);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: An ARP request for somebody else's IP must not trigger a reply
//           (no unsolicited traffic, no cache pollution).
// Input: Mock NIC; handle_frame(ARP request, tpa = other IP).
// Expect: No sends; cache has no entry for either IP.
// Depends: net_handle_frame target-IP guard
JARVIS_TEST(net_arp_request_not_for_us_ignored, "PRE: none | POST: none") {
    mock_reset();
    net::Nic nic = make_mock_nic();
    uint8_t frame[sizeof(net::EtherHeader) + sizeof(net::ArpHeader)] = {};
    build_arp_request(frame, k_peer_mac, k_peer_ip, k_other_ip);

    net::net_handle_frame(frame, sizeof(frame), nic);

    net::MacAddr out{};
    JARVIS_ASSERT_EQ(0ULL, g_sends);
    JARVIS_ASSERT(!net::net_arp_cache().lookup(k_peer_ip, out));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: An ARP reply installs a cache entry (sender IP -> sender MAC),
//           which net_arp_resolve then hits without transmitting.
// Input: Mock NIC; handle_frame(ARP reply from peer); resolve peer IP.
// Expect: Cache lookup succeeds with peer MAC; resolve true, no sends.
// Depends: net_handle_frame ARP-reply path, ArpCache::update, resolve hit
JARVIS_TEST(net_arp_reply_updates_cache, "PRE: none | POST: none") {
    mock_reset();
    net::Nic nic = make_mock_nic();
    uint8_t frame[sizeof(net::EtherHeader) + sizeof(net::ArpHeader)] = {};
    build_arp_reply(frame, k_peer_mac, k_peer_ip);

    net::net_handle_frame(frame, sizeof(frame), nic);

    net::MacAddr out{};
    JARVIS_ASSERT(net::net_arp_cache().lookup(k_peer_ip, out));
    JARVIS_ASSERT(out == k_peer_mac);
    JARVIS_ASSERT(net::net_arp_resolve(nic, k_peer_ip, out));
    JARVIS_ASSERT(out == k_peer_mac);
    JARVIS_ASSERT_EQ(0ULL, g_sends);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Malformed ARP input fails closed — runt frames, truncated ARP
//           bodies, and wrong htype/ptype produce no reply and no cache
//           entry (no crash on short reads).
// Input: Mock NIC; handle_frame with len 10, len 14+10, bad htype, bad ptype.
// Expect: Zero sends every time; no cache entry for the peer IP.
// Depends: net_handle_frame length/type guards
JARVIS_TEST(net_arp_malformed_rejected, "PRE: none | POST: none") {
    mock_reset();
    net::Nic nic = make_mock_nic();
    uint8_t frame[sizeof(net::EtherHeader) + sizeof(net::ArpHeader)] = {};
    build_arp_request(frame, k_peer_mac, k_peer_ip, k_our_ip);
    net::MacAddr out{};

    net::net_handle_frame(frame, 10, nic);
    net::net_handle_frame(frame, sizeof(net::EtherHeader) + 10, nic);
    auto *arp = reinterpret_cast<net::ArpHeader *>(frame +
                                                  sizeof(net::EtherHeader));
    const uint16_t saved_htype = arp->htype;
    arp->htype = __builtin_bswap16(6);
    net::net_handle_frame(frame, sizeof(frame), nic);
    arp->htype = saved_htype;
    const uint16_t saved_ptype = arp->ptype;
    arp->ptype = __builtin_bswap16(net::ETH_TYPE_IPV6);
    net::net_handle_frame(frame, sizeof(frame), nic);
    arp->ptype = saved_ptype;

    JARVIS_ASSERT_EQ(0ULL, g_sends);
    JARVIS_ASSERT(!net::net_arp_cache().lookup(k_peer_ip, out));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: An ICMP echo reply for a ping records ident/seq/source in the
//           reply record, observable via net_icmp_last_reply. Since issue
//           #179 the record holds host order: RFC-order wire bytes
//           [0x12,0x34] record as 0x1234 (previously stored raw as 0x3412,
//           self-consistent only because sends were equally unswapped).
// Input: Mock NIC; clear_reply; handle_frame(IPv4+ICMP ECHO_REPLY,
//        ident 0x1234, seq 7, src = peer IP).
// Expect: last_reply non-null with matching ident/seq/src.
// Depends: net_handle_frame IPv4/ICMP path, icmp record helpers
JARVIS_TEST(net_icmp_echo_reply_recorded, "PRE: none | POST: none") {
    mock_reset();
    net::Nic nic = make_mock_nic();
    uint8_t frame[sizeof(net::EtherHeader) + 20 + 8] = {};
    auto *eth = reinterpret_cast<net::EtherHeader *>(frame);
    eth->dst = k_our_mac;
    eth->src = k_peer_mac;
    eth->type = __builtin_bswap16(net::ETH_TYPE_IPV4);
    frame[14] = 0x45;
    frame[16] = 0;
    frame[17] = 28;
    frame[20] = 0x40;
    frame[22] = 64;
    frame[23] = 1;
    frame[26] = 10;
    frame[27] = 0;
    frame[28] = 0;
    frame[29] = 2;
    frame[34] = net::ICMP_TYPE_ECHO_REPLY;
    frame[35] = 0;
    frame[38] = 0x12;
    frame[39] = 0x34;
    frame[40] = 0;
    frame[41] = 7;

    net::net_handle_frame(frame, sizeof(frame), nic);

    const net::IcmpEchoReply *reply = net::net_icmp_last_reply();
    JARVIS_ASSERT(reply != nullptr);
    JARVIS_ASSERT_EQ(0x1234, reply->ident);
    JARVIS_ASSERT_EQ(7, reply->seq);
    JARVIS_ASSERT(reply->src == net::Ipv4Addr::from_u32(k_peer_ip));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: net_arp_resolve serves a cached entry without transmitting —
//           the fast path used by every UDP send.
// Input: Mock NIC; seed cache via handle_frame(ARP reply); resolve.
// Expect: Resolve true with peer MAC; zero sends (no request broadcast).
// Depends: net_arp_resolve cache-hit path
JARVIS_TEST(net_arp_resolve_cache_hit, "PRE: none | POST: none") {
    mock_reset();
    net::Nic nic = make_mock_nic();
    uint8_t frame[sizeof(net::EtherHeader) + sizeof(net::ArpHeader)] = {};
    build_arp_reply(frame, k_peer_mac, k_peer_ip);
    net::net_handle_frame(frame, sizeof(frame), nic);

    net::MacAddr out{};
    JARVIS_ASSERT(net::net_arp_resolve(nic, k_peer_ip, out));
    JARVIS_ASSERT(out == k_peer_mac);
    JARVIS_ASSERT_EQ(0ULL, g_sends);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Self-ping loopback reflects instantly without ARP or
//           transmission, exercising the echo build + icmp_checksum +
//           reply record in one shot.
// Input: Mock NIC; net_send_icmp_echo to our own IP and to 127.0.0.1.
// Expect: Both true; reply recorded with the second call's id/seq.
// Depends: net_send_icmp_echo loopback, icmp_checksum
JARVIS_TEST(net_icmp_self_ping_loopback, "PRE: none | POST: none") {
    mock_reset();
    net::Nic nic = make_mock_nic();

    JARVIS_ASSERT(net::net_send_icmp_echo(
        nic, net::Ipv4Addr::from_u32(k_our_ip), 0x1111, 1, nullptr, 0));
    JARVIS_ASSERT(net::net_send_icmp_echo(
        nic, net::Ipv4Addr::from_u32(0x7F000001), 0x2222, 2, nullptr, 0));

    const net::IcmpEchoReply *reply = net::net_icmp_last_reply();
    JARVIS_ASSERT(reply != nullptr);
    JARVIS_ASSERT_EQ(0x2222, reply->ident);
    JARVIS_ASSERT_EQ(2, reply->seq);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Echo requests go onto the wire in RFC 792 big-endian order
//           (issue #179): with a seeded ARP cache, sending id 0x1234 seq 7
//           to the peer must place [0x12,0x34,0x00,0x07] at the ICMP
//           ident/seq offsets of the captured frame.
// Input: Mock NIC; seed ARP cache via handle_frame(ARP reply); send echo.
// Expect: send true; one captured frame; ident/seq bytes big-endian.
// Depends: net_send_icmp_echo wire build, ARP cache-hit path
JARVIS_TEST(net_icmp_echo_request_wire_order, "PRE: none | POST: none") {
    mock_reset();
    net::Nic nic = make_mock_nic();
    uint8_t arp[sizeof(net::EtherHeader) + sizeof(net::ArpHeader)] = {};
    build_arp_reply(arp, k_peer_mac, k_peer_ip);
    net::net_handle_frame(arp, sizeof(arp), nic);

    JARVIS_ASSERT(net::net_send_icmp_echo(
        nic, net::Ipv4Addr::from_u32(k_peer_ip), 0x1234, 7, nullptr, 0));
    JARVIS_ASSERT_EQ(1ULL, g_sends);
    // Ether(14) + IPv4(20) + ICMP type/code/checksum(4): ident at 38.
    JARVIS_ASSERT(g_sent_len >= 42);
    JARVIS_ASSERT(g_sent[38] == 0x12);
    JARVIS_ASSERT(g_sent[39] == 0x34);
    JARVIS_ASSERT(g_sent[40] == 0x00);
    JARVIS_ASSERT(g_sent[41] == 0x07);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Malformed IPv4/ICMP frames are rejected without recording a
//           reply (issue #178): total_length < header length underflows
//           icmp_len (size_t wraps) so an IcmpHeader would be read past the
//           frame; a non-IPv4 version nibble is rejected as well.
// Input: Mock NIC; (a) IHL 6 / total_length 20 frame, (b) version-6 frame
//        with otherwise valid echo-reply bytes.
// Expect: last_reply() == nullptr in both cases.
// Depends: net_handle_frame IPv4 length/version guards
JARVIS_TEST(net_icmp_malformed_length_rejected, "PRE: none | POST: none") {
    mock_reset();
    net::Nic nic = make_mock_nic();
    uint8_t bad[40] = {};
    auto *beth = reinterpret_cast<net::EtherHeader *>(bad);
    beth->dst = k_our_mac;
    beth->src = k_peer_mac;
    beth->type = __builtin_bswap16(net::ETH_TYPE_IPV4);
    bad[14] = 0x46;
    bad[16] = 0;
    bad[17] = 20;
    bad[23] = 1;
    net::net_handle_frame(bad, sizeof(bad), nic);
    JARVIS_ASSERT(net::net_icmp_last_reply() == nullptr);

    mock_reset();
    uint8_t v6[sizeof(net::EtherHeader) + 20 + 8] = {};
    auto *veth = reinterpret_cast<net::EtherHeader *>(v6);
    veth->dst = k_our_mac;
    veth->src = k_peer_mac;
    veth->type = __builtin_bswap16(net::ETH_TYPE_IPV4);
    v6[14] = 0x65;
    v6[16] = 0;
    v6[17] = 28;
    v6[23] = 1;
    v6[34] = net::ICMP_TYPE_ECHO_REPLY;
    v6[38] = 0x12;
    v6[39] = 0x34;
    net::net_handle_frame(v6, sizeof(v6), nic);
    JARVIS_ASSERT(net::net_icmp_last_reply() == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A frame whose IP total_length leaves fewer than 8 bytes for
//           ICMP must be dropped before any IcmpHeader read (net.cpp:170)
//           — the last of the three #178 length guards without a probe.
// Input: Mock NIC; version-4/IHL-5 frame, total_length 24 (icmp_len 4).
// Expect: last_reply() == nullptr (no short-header record).
// Depends: net_handle_frame ICMP truncation guard
JARVIS_TEST(net_icmp_truncated_rejected, "PRE: none | POST: none") {
    mock_reset();
    net::Nic nic = make_mock_nic();
    uint8_t short_frame[sizeof(net::EtherHeader) + 24] = {};
    auto *seth = reinterpret_cast<net::EtherHeader *>(short_frame);
    seth->dst = k_our_mac;
    seth->src = k_peer_mac;
    seth->type = __builtin_bswap16(net::ETH_TYPE_IPV4);
    short_frame[14] = 0x45;
    short_frame[16] = 0;
    short_frame[17] = 24;
    short_frame[23] = 1;
    short_frame[34] = net::ICMP_TYPE_ECHO_REPLY;
    net::net_handle_frame(short_frame, sizeof(short_frame), nic);
    JARVIS_ASSERT(net::net_icmp_last_reply() == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: UDP send builds a well-formed Ether/IPv4/UDP frame (issue
//           #138): with a seeded ARP cache, sending 2 payload bytes to the
//           peer must emit one frame with IP proto 17, big-endian ports
//           and the payload inline after the 8-byte UDP header.
// Input: Mock NIC; seed cache via handle_frame(ARP reply); send_udp.
// Expect: true; 1 frame of 14+20+8+2 bytes; proto/ports/payload exact.
// Depends: net_send_udp frame build, ARP cache-hit path
JARVIS_TEST(net_udp_send_builds_frame, "PRE: none | POST: none") {
    mock_reset();
    net::Nic nic = make_mock_nic();
    uint8_t arp[sizeof(net::EtherHeader) + sizeof(net::ArpHeader)] = {};
    build_arp_reply(arp, k_peer_mac, k_peer_ip);
    net::net_handle_frame(arp, sizeof(arp), nic);

    const uint8_t payload[] = {'h', 'i'};
    JARVIS_ASSERT(net::net_send_udp(nic,
                                    net::Ipv4Addr::from_u32(k_peer_ip), 8080,
                                    1234, payload, sizeof(payload)));
    JARVIS_ASSERT_EQ(1ULL, g_sends);
    JARVIS_ASSERT_EQ(14ULL + 20ULL + 8ULL + 2ULL, g_sent_len);
    JARVIS_ASSERT(g_sent[23] == 17);
    JARVIS_ASSERT(g_sent[34] == 0x04 && g_sent[35] == 0xD2);
    JARVIS_ASSERT(g_sent[36] == 0x1F && g_sent[37] == 0x90);
    JARVIS_ASSERT(g_sent[42] == 'h' && g_sent[43] == 'i');
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: net_poll pulls one frame through the NIC hook into the
//           dispatcher (issue #138): an armed one-shot poll delivering an
//           ARP reply must populate the cache; disarmed polls return false.
// Input: Mock NIC with the one-shot poll hook armed once.
// Expect: First poll true + cache hit with zero sends; second poll false.
// Depends: net_poll dispatch, net_arp_resolve cache-hit path
JARVIS_TEST(net_poll_dispatches_frame, "PRE: none | POST: none") {
    mock_reset();
    net::Nic nic = make_mock_nic();
    nic.poll_frame = mock_poll_once;
    g_poll_armed = true;
    JARVIS_ASSERT(net::net_poll(nic));
    net::MacAddr out{};
    JARVIS_ASSERT(net::net_arp_resolve(nic, k_peer_ip, out));
    JARVIS_ASSERT(out == k_peer_mac);
    JARVIS_ASSERT_EQ(0ULL, g_sends);
    JARVIS_ASSERT(!net::net_poll(nic));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The direct reply injector records exactly what it is given
//           (issue #138): set_reply stores ident/seq/src for last_reply.
// Input: Clear, then set_reply(0xAAAA, 5, peer).
// Expect: last_reply non-null with the injected ident/seq/src.
// Depends: net_icmp_set_reply / net_icmp_last_reply
JARVIS_TEST(net_icmp_set_reply_records, "PRE: none | POST: none") {
    mock_reset();
    JARVIS_ASSERT(net::net_icmp_last_reply() == nullptr);
    net::net_icmp_set_reply(0xAAAA, 5, net::Ipv4Addr::from_u32(k_peer_ip));
    const net::IcmpEchoReply *reply = net::net_icmp_last_reply();
    JARVIS_ASSERT(reply != nullptr);
    JARVIS_ASSERT_EQ(0xAAAA, reply->ident);
    JARVIS_ASSERT_EQ(5, reply->seq);
    JARVIS_ASSERT(reply->src == net::Ipv4Addr::from_u32(k_peer_ip));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all network stack tests.
// Input: None
// Expect: All net tests registered
// Depends: Test framework, Logger
void register_net_tests() {
    Logger::info("Registering network stack tests");
    JARVIS_REGISTER_TEST(net_mac_address_ops);
    JARVIS_REGISTER_TEST(net_ipv4_addr_roundtrip);
    JARVIS_REGISTER_TEST(net_arp_cache_ops);
    JARVIS_REGISTER_TEST(net_ipv4_checksum);
    JARVIS_REGISTER_TEST(net_ether_type_swap);
    JARVIS_REGISTER_TEST(net_arp_request_for_us_sends_reply);
    JARVIS_REGISTER_TEST(net_arp_request_not_for_us_ignored);
    JARVIS_REGISTER_TEST(net_arp_reply_updates_cache);
    JARVIS_REGISTER_TEST(net_arp_malformed_rejected);
    JARVIS_REGISTER_TEST(net_icmp_echo_reply_recorded);
    JARVIS_REGISTER_TEST(net_icmp_echo_request_wire_order);
    JARVIS_REGISTER_TEST(net_icmp_malformed_length_rejected);
    JARVIS_REGISTER_TEST(net_icmp_truncated_rejected);
    JARVIS_REGISTER_TEST(net_udp_send_builds_frame);
    JARVIS_REGISTER_TEST(net_poll_dispatches_frame);
    JARVIS_REGISTER_TEST(net_icmp_set_reply_records);
    JARVIS_REGISTER_TEST(net_arp_resolve_cache_hit);
    JARVIS_REGISTER_TEST(net_icmp_self_ping_loopback);
}
