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

/// @file arp.hpp
/// @brief ARP (Address Resolution Protocol) — IPv4 → MAC resolution.

#include <types.hpp>
#include <kernel/net/ether.hpp>

namespace net {

constexpr size_t ARP_CACHE_SIZE = 32;

/// @brief ARP packet header (28 bytes).
struct ArpHeader {
    uint16_t htype; ///< Hardware type (1 = Ethernet).
    uint16_t ptype; ///< Protocol type (0x0800 = IPv4).
    uint8_t hlen;   ///< Hardware address length (6).
    uint8_t plen;   ///< Protocol address length (4).
    uint16_t oper;  ///< Operation (1 = request, 2 = reply).
    MacAddr sha;    ///< Sender hardware address.
    uint32_t spa;   ///< Sender protocol address (IPv4, big-endian).
    MacAddr tha;    ///< Target hardware address.
    uint32_t tpa;   ///< Target protocol address (IPv4, big-endian).
} __attribute__((packed));

/// ARP hardware type: Ethernet.
constexpr uint16_t ARP_HTYPE_ETHER = 1;
/// ARP operation: request.
constexpr uint16_t ARP_OPER_REQUEST = 1;
/// ARP operation: reply.
constexpr uint16_t ARP_OPER_REPLY = 2;

/// @brief A single ARP cache entry.
struct ArpCacheEntry {
    uint32_t ip; ///< IPv4 address (big-endian).
    MacAddr mac; ///< Resolved MAC address.
    bool valid;  ///< Whether this entry contains valid data.
};

/// ARP cache — resolves IP → MAC, caches results.
class ArpCache {
  public:
    ArpCache();

    /// Look up a MAC for the given IP.
    /// @return true if found, populates @p mac.
    bool lookup(uint32_t ip, MacAddr &mac) const;

    /// Update the cache with a new IP→MAC mapping.
    void update(uint32_t ip, MacAddr mac);

    /// Remove an entry.
    void remove(uint32_t ip);

    /// Clear all entries.
    void clear();

    /// Get a pointer to the entry array (for direct iteration).
    const ArpCacheEntry *entries() const {
        return entries_;
    }

  private:
    ArpCacheEntry entries_[ARP_CACHE_SIZE];

    int find(uint32_t ip) const;
    int find_empty() const;
};

} // namespace net
