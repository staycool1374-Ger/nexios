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

/// @file virtio_net.hpp
/// @brief Virtio-net NIC driver — modern (Virtio 1.0) PCI transport.

#pragma once

#include <types.hpp>
#include <kernel/arch/virtio.hpp>
#include <kernel/net/net.hpp>

namespace kernel::net {
using ::net::Ipv4Addr;
using ::net::MacAddr;
using ::net::MAX_PACKET_SIZE;
using ::net::Nic;

/// Virtio-net header (prepended to each packet by the device)
struct VirtioNetHdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} __attribute__((packed));

constexpr size_t VIRTIO_NET_HDR_SIZE = sizeof(VirtioNetHdr);
constexpr uint16_t VIRTIO_NET_QUEUE_RX = 0;
constexpr uint16_t VIRTIO_NET_QUEUE_TX = 1;

/// Virtio-net NIC driver state
struct VirtioNetDevice {
    arch::VirtioTransport transport;

    // Queue memory (physically contiguous)
    uint64_t rx_desc_phys;
    uint64_t rx_avail_phys;
    uint64_t rx_used_phys;
    arch::VirtqDesc *rx_desc;
    arch::VirtqAvail *rx_avail;
    arch::VirtqUsed *rx_used;

    uint64_t tx_desc_phys;
    uint64_t tx_avail_phys;
    uint64_t tx_used_phys;
    arch::VirtqDesc *tx_desc;
    arch::VirtqAvail *tx_avail;
    arch::VirtqUsed *tx_used;

    // DMA buffers for RX
    uint8_t *rx_bufs[16];
    uint64_t rx_bufs_phys[16];

    // TX buffer
    uint8_t *tx_buf;
    uint64_t tx_buf_phys;

    uint16_t queue_size;
    uint16_t rx_avail_idx;
    uint16_t tx_avail_idx;
    uint16_t rx_last_seen_used;

    ~VirtioNetDevice();

    Nic *nic; // back-pointer to the NIC abstraction
};

/// Probe and initialize a Virtio-net device.
/// @param nic  The NIC abstraction to populate.
/// @return true if a Virtio-net device was found and initialized.
bool virtio_net_probe(Nic &nic);

/// Poll for a received frame.  Non-blocking — returns false if nothing
/// available.
/// @param buf  Buffer to copy the frame into.
/// @param len  On success, set to the number of bytes written.
/// @return true if a frame was received.
bool virtio_net_poll(uint8_t *buf, size_t &len);

} // namespace kernel::net
