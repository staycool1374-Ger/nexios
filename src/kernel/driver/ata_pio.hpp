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

/// @file ata_pio.hpp
/// @brief ATA PIO driver for legacy IDE controllers.

#include <kernel/driver/block_device.hpp>

namespace kernel {
namespace block {

/// @brief PIO-based ATA driver for legacy IDE controllers.
class AtaPioDriver final : public BlockDevice {
  public:
    /// @brief Construct an AtaPioDriver on a given IDE channel.
    /// @param port_base Base I/O port (0x1F0 primary, 0x170 secondary).
    /// @param drive_head Drive/head select register value.
    AtaPioDriver(uint16_t port_base = 0x1F0, uint8_t drive_head = 0xE0);
    ~AtaPioDriver() = default;

    /// @brief Probe and identify the drive.
    /// @return true if the drive responds and is valid.
    bool init();

    /// @brief Read one sector from the drive.
    bool read_sector(uint64_t lba, uint8_t *buffer) override;
    /// @brief Write one sector to the drive.
    bool write_sector(uint64_t lba, const uint8_t *buffer) override;
    /// @brief Number of 512-byte sectors on the drive.
    uint64_t sector_count() const override {
        return sector_count_;
    }
    /// @brief Sector size in bytes (always 512).
    uint64_t sector_size() const override {
        return BLOCK_SIZE;
    }
    /// @brief Whether the device is read-only (false for ATA PIO).
    bool is_read_only() const override {
        return false;
    }

    /// @brief Iterate all IDE channels and return the first responsive drive.
    /// @return A new AtaPioDriver instance, or nullptr if no drive found.
    static AtaPioDriver *probe_first_drive();

  private:
    /// @brief Poll the status register until BSY clears or ERR/DRQ is set.
    bool poll_status(bool wait_for_bsy);
    /// @brief Wait for DRQ (data request) to be set.
    bool wait_for_drq();
    /// @brief Send the IDENTIFY command and parse the result.
    bool identify();

    uint16_t port_base_;        ///< ATA I/O port base address.
    uint8_t drive_head_;        ///< Drive/head register value.
    uint64_t sector_count_ = 0; ///< Total sector count from IDENTIFY data.
    bool drive_present_ =
        false; ///< Whether the drive was successfully identified.
};

} // namespace block
} // namespace kernel
