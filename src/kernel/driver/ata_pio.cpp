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

/// @file ata_pio.cpp
/// @brief ATA PIO driver implementation.

#include <kernel/driver/ata_pio.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/arch/io.hpp>
#include <constants.hpp>
#include <string.hpp>

namespace kernel {
namespace block {

static constexpr uint64_t ATA_POLL_TIMEOUT = 100000;

using namespace arch;

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
AtaPioDriver::AtaPioDriver(uint16_t port_base, uint8_t drive_head)
    : port_base_(port_base), drive_head_(drive_head) {
}

/// @brief Send the IDENTIFY command and populate sector_count_ /
/// drive_present_.
/// @return true if a valid ATA drive responded.
bool AtaPioDriver::identify() {
    outb(static_cast<uint16_t>(port_base_ + 6), drive_head_);

    outb(static_cast<uint16_t>(port_base_ + 2), 0);
    outb(static_cast<uint16_t>(port_base_ + 3), 0);
    outb(static_cast<uint16_t>(port_base_ + 4), 0);
    outb(static_cast<uint16_t>(port_base_ + 5), 0);

    outb(static_cast<uint16_t>(port_base_ + 7), ATA_CMD_IDENTIFY);

    uint8_t status = inb(static_cast<uint16_t>(port_base_ + 7));
    if (status == 0 || status == 0xFF)
        return false;

    if (!poll_status(true))
        return false;

    status = inb(static_cast<uint16_t>(port_base_ + 7));
    if (status & ATA_SR_ERR)
        return false;

    if (!wait_for_drq())
        return false;

    if (!wait_for_drq())
        return false;

    uint16_t buf[256];
    for (int i = 0; i < 256; ++i) {
        buf[i] = inw(port_base_);
    }

    if (buf[0] & (1 << 15))
        return false;

    sector_count_ = (static_cast<uint64_t>(buf[61]) << 16) | buf[60];
    if (sector_count_ == 0)
        return false;

    drive_present_ = true;
    return true;
}

/// @brief Initialize the drive (alias for identify()).
/// @return true if the drive is present and ready.
bool AtaPioDriver::init() {
    return identify();
}

/// @brief Poll the ATA status register with a timeout.
/// @param wait_for_bsy If true, wait only for BSY to clear.
///                     If false, wait for DRQ (or ERR).
/// @return true on success, false on timeout.
bool AtaPioDriver::poll_status(bool wait_for_bsy) {
    for (uint64_t i = 0; i < ATA_POLL_TIMEOUT; ++i) {
        uint8_t status = inb(static_cast<uint16_t>(port_base_ + 7));
        if (wait_for_bsy) {
            if (!(status & ATA_SR_BSY))
                return true;
        } else {
            if (status & ATA_SR_BSY)
                continue;
            if (status & ATA_SR_ERR)
                return false;
            if (status & ATA_SR_DRQ)
                return true;
        }
        io_wait();
        pause();
    }
    return false;
}

/// @brief Convenience wrapper — wait until DRQ (data request) is set.
/// @return true if DRQ asserted within timeout.
bool AtaPioDriver::wait_for_drq() {
    return poll_status(false);
}

/// @brief Read a single 512-byte sector via PIO.
/// @param lba Logical block address.
/// @param buffer Output buffer (must be at least 512 bytes).
/// @return true on success.
bool AtaPioDriver::read_sector(uint64_t lba, uint8_t *buffer) {
    if (!drive_present_ || !buffer || lba >= sector_count_)
        return false;

    if (!poll_status(true))
        return false;

    outb(static_cast<uint16_t>(port_base_ + 6),
         drive_head_ | static_cast<uint8_t>((lba >> 24) & 0x0F));
    outb(static_cast<uint16_t>(port_base_ + 2), 1);
    outb(static_cast<uint16_t>(port_base_ + 3),
         static_cast<uint8_t>(lba & 0xFF));
    outb(static_cast<uint16_t>(port_base_ + 4),
         static_cast<uint8_t>((lba >> 8) & 0xFF));
    outb(static_cast<uint16_t>(port_base_ + 5),
         static_cast<uint8_t>((lba >> 16) & 0xFF));
    outb(static_cast<uint16_t>(port_base_ + 7), ATA_CMD_READ_SECTORS);

    if (!wait_for_drq())
        return false;

    for (int i = 0; i < 256; ++i) {
        uint16_t word = inw(port_base_);
        buffer[static_cast<size_t>(i) * 2] = static_cast<uint8_t>(word & 0xFF);
        buffer[static_cast<size_t>(i) * 2 + 1] =
            static_cast<uint8_t>((word >> 8) & 0xFF);
    }

    return true;
}

/// @brief Write a single 512-byte sector via PIO.
/// @param lba Logical block address.
/// @param buffer Input buffer (must be at least 512 bytes).
/// @return true on success.
bool AtaPioDriver::write_sector(uint64_t lba, const uint8_t *buffer) {
    if (!drive_present_ || !buffer || lba >= sector_count_)
        return false;

    if (!poll_status(true))
        return false;

    outb(static_cast<uint16_t>(port_base_ + 6),
         drive_head_ | static_cast<uint8_t>((lba >> 24) & 0x0F));
    outb(static_cast<uint16_t>(port_base_ + 2), 1);
    outb(static_cast<uint16_t>(port_base_ + 3),
         static_cast<uint8_t>(lba & 0xFF));
    outb(static_cast<uint16_t>(port_base_ + 4),
         static_cast<uint8_t>((lba >> 8) & 0xFF));
    outb(static_cast<uint16_t>(port_base_ + 5),
         static_cast<uint8_t>((lba >> 16) & 0xFF));
    outb(static_cast<uint16_t>(port_base_ + 7), ATA_CMD_WRITE_SECTORS);

    if (!wait_for_drq())
        return false;

    for (int i = 0; i < 256; ++i) {
        uint16_t word =
            static_cast<uint16_t>(buffer[static_cast<size_t>(i) * 2]) |
            (static_cast<uint16_t>(buffer[static_cast<size_t>(i) * 2 + 1])
             << 8);
        outw(port_base_, word);
    }

    if (!poll_status(true))
        return false;

    return true;
}

/// @brief Probe primary and secondary IDE channels, master and slave,
///        and return the first drive found.
/// @return A heap-allocated AtaPioDriver, or nullptr.
AtaPioDriver *AtaPioDriver::probe_first_drive() {
    struct Channel {
        uint16_t port_base;
        uint8_t drive_head;
    };
    static constexpr Channel channels[] = {
        {0x1F0, ATA_DRIVE_MASTER},
        {0x1F0, ATA_DRIVE_SLAVE},
        {0x170, ATA_DRIVE_MASTER},
        {0x170, ATA_DRIVE_SLAVE},
    };

    for (auto &ch : channels) {
#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-possible-null-dereference"
#endif
        auto *drv_mem = kernel::MemPool::alloc(sizeof(AtaPioDriver));
        if (!drv_mem) continue;
        auto *drv = new (drv_mem) AtaPioDriver(ch.port_base, ch.drive_head);
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
        if (drv->init())
            return drv;
        drv->~AtaPioDriver(); kernel::MemPool::free(drv);
    }
    return nullptr;
}

} // namespace block
} // namespace kernel
