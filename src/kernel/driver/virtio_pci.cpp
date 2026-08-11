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

/// @file virtio_pci.cpp
/// @brief Virtio PCI transport implementation (modern 1.0 interface).

#include <kernel/arch/virtio.hpp>
#include <kernel/arch/pci.hpp>
#include <logger.hpp>
#include <lib/atomic.hpp>

using namespace kernel;

namespace arch {

static uint64_t page_align_down(uint64_t x) {
    return x & ~0xFFFULL;
}
static uint64_t page_align_up(uint64_t x) {
    return (x + 0xFFF) & ~0xFFFULL;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool virtio_map_mmio(uint8_t bar, uint32_t offset, uint32_t length,
                     VirtioMmio &mmio_out, PciBdf bdf) {
    PciDeviceInfo info = pci_read_device_info(bdf);
    if (bar >= 6 || info.bars[bar].address == 0)
        return false;

    uint64_t bar_phys = info.bars[bar].address;
    uint64_t region_start = bar_phys + offset;
    uint64_t region_end = region_start + length;

    uint64_t map_start = page_align_down(region_start);
    uint64_t map_end = page_align_up(region_end);

    for (uint64_t page = map_start; page < map_end; page += PAGE_SIZE) {
        VMM::map_page(HHDM_OFFSET + page, page, false);
    }

    mmio_out.phys_addr = region_start;
    mmio_out.virt_addr = HHDM_OFFSET + region_start;
    mmio_out.length = length;
    return true;
}

static bool parse_virtio_cap(PciBdf bdf, uint8_t cap_offset,
                             VirtioPciCap &cap) {
    cap.cap_vndr = pci_config_readb(pci_make_addr(bdf, cap_offset));
    cap.cap_next = pci_config_readb(pci_make_addr(bdf, cap_offset + 1));
    cap.cap_len = pci_config_readb(pci_make_addr(bdf, cap_offset + 2));
    cap.cfg_type = pci_config_readb(pci_make_addr(bdf, cap_offset + 3));
    cap.bar = pci_config_readb(pci_make_addr(bdf, cap_offset + 4));
    cap.padding[0] = pci_config_readb(pci_make_addr(bdf, cap_offset + 5));
    cap.padding[1] = pci_config_readb(pci_make_addr(bdf, cap_offset + 6));
    cap.padding[2] = pci_config_readb(pci_make_addr(bdf, cap_offset + 7));
    cap.offset = pci_config_readl(pci_make_addr(bdf, cap_offset + 8));
    cap.length = pci_config_readl(pci_make_addr(bdf, cap_offset + 12));
    return true;
}

static VirtioMmio *virtio_cfg_target(VirtioTransport &t, uint8_t cfg_type) {
    switch (cfg_type) {
    case VIRTIO_CFG_COMMON:
        return &t.common_cfg;
    case VIRTIO_CFG_NOTIFY:
        return &t.notify_cfg;
    case VIRTIO_CFG_ISR:
        return &t.isr_cfg;
    case VIRTIO_CFG_DEVICE:
        return &t.device_cfg;
    default:
        return nullptr;
    }
}

bool virtio_find_device(uint16_t device_id, VirtioTransport &transport) {
    pci_scan_all();
    const PciDeviceInfo *dev = nullptr;
    size_t count = pci_device_count();
    for (size_t i = 0; i < count; ++i) {
        const auto &d = pci_devices()[i];
        if (d.vendor_id == VIRTIO_PCI_VENDOR && d.device_id == device_id) {
            dev = &d;
            break;
        }
    }
    if (!dev)
        return false;

    transport.bdf = dev->bdf;
    transport.device_id = dev->device_id;
    transport.modern = (dev->device_id & 0xFFF0) == 0x1040;
    transport.notify_off_multiplier = 0;

    PciBdf bdf = dev->bdf;

    // Walk Virtio-specific capabilities
    uint8_t cap_offset = pci_find_capability(bdf, 0x09);
    while (cap_offset != 0) {
        VirtioPciCap cap{};
        parse_virtio_cap(bdf, cap_offset, cap);

        if (cap.cfg_type >= VIRTIO_CFG_COMMON &&
            cap.cfg_type <= VIRTIO_CFG_DEVICE) {
            VirtioMmio *target = virtio_cfg_target(transport, cap.cfg_type);
            if (target && !virtio_map_mmio(cap.bar, cap.offset, cap.length,
                                           *target, bdf)) {
                Logger::error(
                    "virtio: failed to map cfg type %d at bar %d + 0x%x",
                    cap.cfg_type, cap.bar, cap.offset);
            }
        }

        if (cap.cfg_type == VIRTIO_CFG_NOTIFY) {
            transport.notify_off_multiplier =
                pci_config_readl(pci_make_addr(bdf, cap_offset + 16));
        }

        cap_offset = cap.cap_next;
    }

    Logger::info("virtio: found device %x at %d:%d.%d (modern=%d)", device_id,
                 bdf.bus, bdf.device, bdf.function, transport.modern);
    return true;
}

bool virtio_init_transport(VirtioTransport &transport) {
    // v0.4.0 MP-1 hardening: a transport with no MMIO mapping (null
    // virt_addr — e.g. a default-constructed transport) must be rejected
    // instead of dereferencing address 0x14 (the status register offset).
    // Pre-MP-1 the boot identity map in the kernel PML4 masked this latent
    // null-deref; private kernel-half PML4s no longer carry it.
    if (transport.common_cfg.virt_addr == 0 &&
        transport.notify_cfg.virt_addr == 0 &&
        transport.isr_cfg.virt_addr == 0 &&
        transport.device_cfg.virt_addr == 0) {
        return false;
    }

    virtio_write_status(transport, VIRTIO_STATUS_RESET);
    for (int i = 0; i < 100; ++i) {
        if (virtio_read_status(transport) == VIRTIO_STATUS_RESET)
            break;
        io_wait();
    }

    virtio_write_status(transport, VIRTIO_STATUS_ACK);
    virtio_write_status(transport, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    return true;
}

bool virtio_negotiate_features(VirtioTransport &t, uint64_t driver_features) {
    virtio_write_common(t, VIRTIO_COMMON_DEVICE_FEATURE_SEL, 0);
    uint32_t dev_features_lo =
        virtio_read_common(t, VIRTIO_COMMON_DEVICE_FEATURE);
    virtio_write_common(t, VIRTIO_COMMON_DEVICE_FEATURE_SEL, 1);
    uint32_t dev_features_hi =
        virtio_read_common(t, VIRTIO_COMMON_DEVICE_FEATURE);
    uint64_t dev_features =
        (static_cast<uint64_t>(dev_features_hi) << 32) | dev_features_lo;

    uint64_t accepted = driver_features & dev_features;

    virtio_write_common(t, VIRTIO_COMMON_DRIVER_FEATURE_SEL, 0);
    virtio_write_common(t, VIRTIO_COMMON_DRIVER_FEATURE,
                        static_cast<uint32_t>(accepted & 0xFFFFFFFF));
    virtio_write_common(t, VIRTIO_COMMON_DRIVER_FEATURE_SEL, 1);
    virtio_write_common(t, VIRTIO_COMMON_DRIVER_FEATURE,
                        static_cast<uint32_t>((accepted >> 32) & 0xFFFFFFFF));

    uint8_t status = virtio_read_status(t);
    virtio_write_status(t, status | VIRTIO_STATUS_FEATURES_OK);

    status = virtio_read_status(t);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        Logger::error("virtio: device rejected feature negotiation");
        return false;
    }

    Logger::info("virtio: features negotiated: %x", accepted);
    return true;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool virtio_setup_queue(VirtioTransport &t, uint16_t queue_idx,
                        uint16_t queue_size, uint64_t desc_phys,
                        uint64_t avail_phys, uint64_t used_phys) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    virtio_write_common16(t, VIRTIO_COMMON_QUEUE_SEL, queue_idx);
    kernel::atomic_fence();
    uint16_t size = virtio_read_common16(t, VIRTIO_COMMON_QUEUE_SIZE);
    if (size == 0) {
        Logger::error("virtio: queue %d not available", queue_idx);
        return false;
    }
    if (queue_size > size)
        queue_size = size;

    virtio_write_common(t, VIRTIO_COMMON_QUEUE_DESC_LO,
                        static_cast<uint32_t>(desc_phys & 0xFFFFFFFF));
    virtio_write_common(t, VIRTIO_COMMON_QUEUE_DESC_HI,
                        static_cast<uint32_t>((desc_phys >> 32) & 0xFFFFFFFF));
    virtio_write_common(t, VIRTIO_COMMON_QUEUE_DRIVER_LO,
                        static_cast<uint32_t>(avail_phys & 0xFFFFFFFF));
    virtio_write_common(t, VIRTIO_COMMON_QUEUE_DRIVER_HI,
                        static_cast<uint32_t>((avail_phys >> 32) & 0xFFFFFFFF));
    virtio_write_common(t, VIRTIO_COMMON_QUEUE_DEVICE_LO,
                        static_cast<uint32_t>(used_phys & 0xFFFFFFFF));
    virtio_write_common(t, VIRTIO_COMMON_QUEUE_DEVICE_HI,
                        static_cast<uint32_t>((used_phys >> 32) & 0xFFFFFFFF));

    virtio_write_common16(t, VIRTIO_COMMON_QUEUE_SIZE, queue_size);
    kernel::atomic_fence();
    virtio_write_common16(t, VIRTIO_COMMON_QUEUE_ENABLE, 1);
    kernel::atomic_fence();

    Logger::info("virtio: queue %d setup (size=%d)", queue_idx, queue_size);
    return true;
}

} // namespace arch
