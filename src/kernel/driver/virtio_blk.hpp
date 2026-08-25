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

/// @file virtio_blk.hpp
/// @brief Virtio block driver — modern (Virtio 1.0) PCI transport.

#pragma once

#include <types.hpp>
#include <kernel/arch/virtio.hpp>
#include <kernel/driver/block_device.hpp>
#include <kernel/sync/mutex.hpp>

namespace kernel::block {

/// Virtio block I/O request types
enum VirtioBlkReqType : uint8_t {
    VIRTIO_BLK_T_IN = 0,
    VIRTIO_BLK_T_OUT = 1,
    VIRTIO_BLK_T_FLUSH = 4,
};

/// Virtio block request header (must be 16-byte aligned)
struct VirtioBlkReqHdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

/// Virtio block response status
enum VirtioBlkStatus : uint8_t {
    VIRTIO_BLK_S_OK = 0,
    VIRTIO_BLK_S_IOERR = 1,
    VIRTIO_BLK_S_UNSUP = 2,
};

/// Virtio block device driver (modern interface)
class VirtioBlkDriver final : public BlockDevice {
  public:
    VirtioBlkDriver(arch::VirtioTransport &transport);
    ~VirtioBlkDriver();

    bool init();
    bool read_sector(uint64_t lba, uint8_t *buffer) override;
    bool write_sector(uint64_t lba, const uint8_t *buffer) override;
    uint64_t sector_count() const override {
        return sector_count_;
    }
    uint64_t sector_size() const override {
        return BLOCK_SIZE;
    }
    bool is_read_only() const override {
        return false;
    }

    static VirtioBlkDriver *probe();

  private:
    bool submit_request(uint32_t type, uint64_t sector, uint8_t *data,
                        bool is_read);

    arch::VirtioTransport transport_;
    uint64_t sector_count_ = 0;

    // Queue memory (physically contiguous)
    uint64_t desc_phys_ = 0;
    uint64_t avail_phys_ = 0;
    uint64_t used_phys_ = 0;

    // Virtual addresses for queue memory
    arch::VirtqDesc *desc_ = nullptr;
    arch::VirtqAvail *avail_ = nullptr;
    arch::VirtqUsed *used_ = nullptr;
    uint16_t queue_size_ = 0;
    uint16_t avail_idx_ = 0;

    // DMA buffer physical address
    uint64_t dma_buf_phys_ = 0;
    uint8_t *dma_buf_ = nullptr;

    // H-3 (audit-drivers-vfs-net-v0.4.2): serializes the whole
    // descriptor-fill → avail-push → notify → poll critical section.  The
    // driver shares one DMA buffer and the avail/used rings; concurrent
    // submit_request calls would corrupt the chain or lose completions.
    sync::Mutex submit_lock_{};
};

} // namespace kernel::block
