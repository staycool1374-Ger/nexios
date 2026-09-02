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

/// @file virtio_net.cpp
/// @brief Virtio-net NIC driver implementation.

#include <kernel/driver/virtio_net.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/sync/spinlock.hpp>
#include <string.hpp>
#include <logger.hpp>
#include <lib/atomic.hpp>

using namespace kernel;
using namespace arch;
using sync::IrqSpinLockGuard;

namespace kernel::net {

static bool virtio_net_send_frame(const uint8_t *data, size_t len);

static VirtioNetDevice *g_virtio_net_dev = nullptr;

VirtioNetDevice::~VirtioNetDevice() {
    if (queue_size == 0)
        return;
    auto pages_for = [](size_t bytes) {
        return (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    };
    size_t desc_pages = pages_for(queue_size * sizeof(arch::VirtqDesc));
    size_t avail_pages =
        pages_for(2 * sizeof(uint16_t) + queue_size * sizeof(uint16_t));
    size_t used_pages = pages_for(2 * sizeof(uint16_t) +
                                  queue_size * sizeof(arch::VirtqUsedElem));
    auto free_pages = [](uint64_t base, size_t n) {
        if (!base)
            return;
        for (size_t i = 0; i < n; ++i)
            PMM::free_page(base + i * PAGE_SIZE);
    };
    free_pages(rx_desc_phys, desc_pages);
    free_pages(rx_avail_phys, avail_pages);
    free_pages(rx_used_phys, used_pages);
    free_pages(tx_desc_phys, desc_pages);
    free_pages(tx_avail_phys, avail_pages);
    free_pages(tx_used_phys, used_pages);
    for (uint64_t phys : rx_bufs_phys)
        if (phys)
            PMM::free_page(phys);
    if (tx_buf_phys)
        PMM::free_page(tx_buf_phys);
}

static bool alloc_queue_pages(uint64_t &desc_phys, uint64_t &avail_phys,
                              uint64_t &used_phys, arch::VirtqDesc *&desc,
                              arch::VirtqAvail *&avail, arch::VirtqUsed *&used,
                              uint16_t queue_size) {
    size_t desc_size = queue_size * sizeof(arch::VirtqDesc);
    size_t avail_size = sizeof(uint16_t) * 2 + queue_size * sizeof(uint16_t);
    size_t used_size =
        sizeof(uint16_t) * 2 + queue_size * sizeof(arch::VirtqUsedElem);

    // Round to pages
    size_t desc_pages = (desc_size + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t avail_pages = (avail_size + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t used_pages = (used_size + PAGE_SIZE - 1) / PAGE_SIZE;

    desc_phys = PMM::alloc_contiguous(desc_pages);
    avail_phys = PMM::alloc_contiguous(avail_pages);
    used_phys = PMM::alloc_contiguous(used_pages);

    if (!desc_phys || !avail_phys || !used_phys) {
        Logger::error("virtio-net: OOM for queue pages");
        return false;
    }

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    desc = reinterpret_cast<arch::VirtqDesc *>(HHDM_OFFSET + desc_phys);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    avail = reinterpret_cast<arch::VirtqAvail *>(HHDM_OFFSET + avail_phys);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    used = reinterpret_cast<arch::VirtqUsed *>(HHDM_OFFSET + used_phys);

    memset(desc, 0, desc_pages * PAGE_SIZE);
    memset(avail, 0, avail_pages * PAGE_SIZE);
    memset(used, 0, used_pages * PAGE_SIZE);

    return true;
}

/// @brief Fills one RX descriptor + avail-ring entry.  Lock-free helper:
///        callers must hold dev.lock_ (via IrqSpinLockGuard).  The
///        atomic_fence ordering (descriptor -> avail entry -> fence ->
///        idx++) is mandatory and must be preserved.
static void rx_ring_fill_locked(VirtioNetDevice &dev, uint16_t qi) {
    dev.rx_desc[qi].addr = dev.rx_bufs_phys[qi];
    dev.rx_desc[qi].len = MAX_PACKET_SIZE;
    dev.rx_desc[qi].flags = VIRTIO_DESC_F_WRITE;
    dev.rx_desc[qi].next = 0;

    dev.rx_avail->ring[dev.rx_avail->idx % dev.queue_size] = qi;
    kernel::atomic_fence();
    dev.rx_avail->idx = static_cast<uint16_t>(dev.rx_avail->idx + 1);
}

static void add_rx_buf(VirtioNetDevice &dev, int idx) {
    IrqSpinLockGuard guard(dev.lock_);
    rx_ring_fill_locked(dev, static_cast<uint16_t>(idx));
}

bool virtio_net_probe(Nic &nic) {
    arch::VirtioTransport transport{};
    static const uint16_t net_ids[] = {
        VIRTIO_DEVICE_NET,       // 0x1041 — modern-only
        0x1043,                  // alternate modern ID
        VIRTIO_DEVICE_NET_LEGACY // 0x1000 — transitional
    };
    bool found = false;
    for (auto id : net_ids) {
        if (arch::virtio_find_device(id, transport)) {
            found = true;
            break;
        }
    }
    if (!found) {
        Logger::info("virtio-net: no device found");
        return false;
    }

    if (!arch::virtio_init_transport(transport)) {
        Logger::error("virtio-net: transport init failed");
        return false;
    }

    uint64_t features = VIRTIO_F_VERSION_1;
    if (!arch::virtio_negotiate_features(transport, features)) {
        return false;
    }

#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-possible-null-dereference"
#endif
    auto *dev_mem = kernel::MemPool::alloc(sizeof(VirtioNetDevice));
    if (dev_mem == nullptr)
        return false;
    auto *dev = new (dev_mem) VirtioNetDevice();
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
    dev->transport = transport;
    dev->queue_size = 16;
    dev->rx_avail_idx = 0;
    dev->tx_avail_idx = 0;
    dev->rx_last_seen_used = 0;
    dev->tx_inflight_ = false;

    // Allocate queue memory
    if (!alloc_queue_pages(dev->rx_desc_phys, dev->rx_avail_phys,
                           dev->rx_used_phys, dev->rx_desc, dev->rx_avail,
                           dev->rx_used, dev->queue_size) ||
        !alloc_queue_pages(dev->tx_desc_phys, dev->tx_avail_phys,
                           dev->tx_used_phys, dev->tx_desc, dev->tx_avail,
                           dev->tx_used, dev->queue_size)) {
        dev->~VirtioNetDevice();
        kernel::MemPool::free(dev);
        return false;
    }

    // Setup RX and TX queues
    if (!arch::virtio_setup_queue(transport, VIRTIO_NET_QUEUE_RX,
                                  dev->queue_size, dev->rx_desc_phys,
                                  dev->rx_avail_phys, dev->rx_used_phys) ||
        !arch::virtio_setup_queue(transport, VIRTIO_NET_QUEUE_TX,
                                  dev->queue_size, dev->tx_desc_phys,
                                  dev->tx_avail_phys, dev->tx_used_phys)) {
        dev->~VirtioNetDevice();
        kernel::MemPool::free(dev);
        return false;
    }

    // Allocate RX DMA buffers
    for (int i = 0; i < 16; ++i) {
        uint64_t phys = PMM::alloc_page();
        if (!phys) {
            Logger::error("virtio-net: OOM for RX buffer %d", i);
            dev->~VirtioNetDevice();
            kernel::MemPool::free(dev);
            return false;
        }
        dev->rx_bufs_phys[i] = phys;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        dev->rx_bufs[i] = reinterpret_cast<uint8_t *>(HHDM_OFFSET + phys);
        memset(dev->rx_bufs[i], 0, PAGE_SIZE);

        // Add to RX available ring
        add_rx_buf(*dev, i);
    }

    // Allocate TX DMA buffer
    dev->tx_buf_phys = PMM::alloc_page();
    if (!dev->tx_buf_phys) {
        dev->~VirtioNetDevice();
        kernel::MemPool::free(dev);
        return false;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    dev->tx_buf = reinterpret_cast<uint8_t *>(HHDM_OFFSET + dev->tx_buf_phys);

    // DRIVER_OK
    uint8_t status = arch::virtio_read_status(transport);
    arch::virtio_write_status(transport, status | VIRTIO_STATUS_DRIVER_OK);

    // Read MAC from device config (offset 0 for virtio-net)
    MacAddr mac{};
    if (transport.device_cfg.virt_addr == 0) {
        Logger::error("virtio-net: device cfg not mapped, using fallback MAC");
        mac = {{0x52, 0x54, 0x00, 0x12, 0x34, 0x56}};
    } else {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *cfg = reinterpret_cast<volatile uint8_t *>(
            transport.device_cfg.virt_addr);
        for (int i = 0; i < 6; ++i)
            mac.addr[i] = cfg[i];
    }

    // Initialize NIC abstraction
    nic.name = "eth0";
    nic.mac = mac;
    nic.ip = Ipv4Addr::from_u32(0); // assigned later
    nic.subnet = Ipv4Addr::from_u32(0);
    nic.gateway = Ipv4Addr::from_u32(0);
    nic.send_frame = virtio_net_send_frame;
    nic.poll_frame = virtio_net_poll;
    nic.driver_data = dev;

    dev->nic = &nic;
    g_virtio_net_dev = dev;

    Logger::info("virtio-net: MAC %x:%x:%x:%x:%x:%x", mac.addr[0], mac.addr[1],
                 mac.addr[2], mac.addr[3], mac.addr[4], mac.addr[5]);
    return true;
}

static bool virtio_net_send_frame(const uint8_t *data, size_t len) {
    if (!g_virtio_net_dev)
        return false;
    auto &dev = *g_virtio_net_dev;

    if (len + VIRTIO_NET_HDR_SIZE > PAGE_SIZE)
        return false;

    // Program + notify under the lock.  The single TX buffer must not be
    // overwritten while a previous descriptor is still in flight.  Capture
    // the used-ring snapshot BEFORE the notify: if the device completes the
    // descriptor before our post-notify read, the snapshot already reflects
    // completion and the poll would spin forever.
    uint16_t used_snapshot = 0;
    {
        IrqSpinLockGuard guard(dev.lock_);
        if (dev.tx_inflight_)
            return false; // previous send not yet consumed

        used_snapshot = dev.tx_used->idx;

        auto *hdr = reinterpret_cast<VirtioNetHdr *>(dev.tx_buf);
        memset(hdr, 0, VIRTIO_NET_HDR_SIZE);
        memcpy(dev.tx_buf + VIRTIO_NET_HDR_SIZE, data, len);

        uint16_t idx = dev.tx_avail_idx % dev.queue_size;
        dev.tx_desc[idx].addr = dev.tx_buf_phys;
        dev.tx_desc[idx].len = static_cast<uint32_t>(VIRTIO_NET_HDR_SIZE + len);
        dev.tx_desc[idx].flags = 0;
        dev.tx_desc[idx].next = 0;

        dev.tx_avail->ring[dev.tx_avail->idx % dev.queue_size] = idx;
        kernel::atomic_fence();
        dev.tx_avail->idx = static_cast<uint16_t>(dev.tx_avail->idx + 1);
        kernel::atomic_fence();

        arch::virtio_notify(dev.transport, VIRTIO_NET_QUEUE_TX);

        dev.tx_avail_idx = static_cast<uint16_t>(dev.tx_avail_idx + 1);
        dev.tx_inflight_ = true;
    }

    // Bounded completion poll OUTSIDE the lock (no cli across a spin loop).
    // Bound matches the virtio-blk request path (FLAW-06 pattern).
    int timeout = 1000000;
    while (dev.tx_used->idx == used_snapshot && --timeout > 0) {
        kernel::atomic_fence();
        arch::pause();
    }
    bool ok = timeout > 0;

    {
        IrqSpinLockGuard guard(dev.lock_);
        dev.tx_inflight_ = false;
    }
    return ok;
}

bool virtio_net_poll(uint8_t *buf, size_t &len) {
    if (!g_virtio_net_dev)
        return false;
    auto &dev = *g_virtio_net_dev;

    // Consume + recycle + advance atomically so a buffer is never handed to
    // the device twice nor to the producer while in use.
    IrqSpinLockGuard guard(dev.lock_);
    kernel::atomic_fence();
    uint16_t used_idx = dev.rx_used->idx;
    if (used_idx == dev.rx_last_seen_used)
        return false;

    uint16_t slot = dev.rx_last_seen_used % dev.queue_size;
    uint32_t desc_idx = dev.rx_used->ring[slot].id;
    uint32_t pkt_len = dev.rx_used->ring[slot].len;

    // M-4 (audit-drivers-vfs-net-v0.4.2): desc_idx comes from the DEVICE and
    // is used unchecked as an index into rx_bufs[].  A faulty/malicious device
    // can cause an out-of-bounds read/write.  Bounds-check against queue_size
    // and recycle the slot on an invalid id.
    if (desc_idx >= dev.queue_size) {
        dev.rx_last_seen_used =
            static_cast<uint16_t>(dev.rx_last_seen_used + 1);
        return false;
    }

    if (pkt_len > VIRTIO_NET_HDR_SIZE) {
        size_t frame_len = pkt_len - VIRTIO_NET_HDR_SIZE;
        if (frame_len > MAX_PACKET_SIZE)
            frame_len = MAX_PACKET_SIZE;
        memcpy(buf, dev.rx_bufs[desc_idx] + VIRTIO_NET_HDR_SIZE, frame_len);
        len = frame_len;
    } else {
        len = 0;
    }

    rx_ring_fill_locked(dev, static_cast<uint16_t>(desc_idx));
    dev.rx_last_seen_used = static_cast<uint16_t>(dev.rx_last_seen_used + 1);
    return len > 0;
}

void virtio_net_destroy() {
    if (!g_virtio_net_dev)
        return;
    auto *dev = g_virtio_net_dev;
    g_virtio_net_dev = nullptr; // late poll/send null-check and return

    // M-5 (audit-drivers-vfs-net-v0.4.2): a send_frame() may be mid-flight —
    // it captured `dev` before our null (the local reference survives the
    // g_virtio_net_dev = nullptr check) and polls tx_used OUTSIDE the lock.
    // Drain tx_inflight_ (bounded) so no completion poll touches freed
    // descriptors/memory after we destruct.
    {
        IrqSpinLockGuard guard(dev->lock_);
        if (dev->tx_inflight_) {
            // Completion poll for a concurrent sender: bounded, no alloc.
            uint64_t t0 = arch::Timer::ticks();
            while (dev->tx_inflight_ && arch::Timer::ticks() - t0 < 1000000)
                arch::pause();
        }
    }

    dev->~VirtioNetDevice();
    MemPool::free(dev);
}

} // namespace kernel::net
