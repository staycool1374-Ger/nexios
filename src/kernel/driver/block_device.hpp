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

/// @file block_device.hpp
/// @brief Abstract block device interface and mock device.

#include <types.hpp>

namespace kernel {
namespace block {

/// @brief Standard block size (512 bytes).
static constexpr uint64_t BLOCK_SIZE = 512;

/// @brief Abstract base class for block devices.
class BlockDevice {
  public:
    virtual ~BlockDevice() = default;

    /// @brief Read a single sector.
    /// @param lba Logical block address.
    /// @param buffer Output buffer (must be at least sector_size() bytes).
    /// @return true on success.
    virtual bool read_sector(uint64_t lba, uint8_t *buffer) = 0;
    /// @brief Write a single sector.
    /// @param lba Logical block address.
    /// @param buffer Input buffer (must be at least sector_size() bytes).
    /// @return true on success.
    virtual bool write_sector(uint64_t lba, const uint8_t *buffer) = 0;
    /// @brief Total number of sectors.
    virtual uint64_t sector_count() const = 0;
    /// @brief Size of each sector in bytes.
    virtual uint64_t sector_size() const = 0;
    /// @brief Whether the device reports as read-only.
    virtual bool is_read_only() const = 0;
};

/// @brief Mock block device backed by physical memory or an external buffer.
class MockBlockDevice final : public BlockDevice {
  public:
    /// @brief Allocate writable storage via PMM.
    MockBlockDevice(uint64_t sector_count_);
    /// @brief Wrap an existing buffer (may be read-only).
    MockBlockDevice(uint8_t *external_data, uint64_t sector_count_,
                    bool read_only);
    ~MockBlockDevice();

    /// @brief Read a sector from the backing buffer.
    bool read_sector(uint64_t lba, uint8_t *buffer) override;
    /// @brief Write a sector to the backing buffer.
    bool write_sector(uint64_t lba, const uint8_t *buffer) override;
    uint64_t sector_count() const override {
        return sector_count_;
    }
    uint64_t sector_size() const override {
        return BLOCK_SIZE;
    }
    bool is_read_only() const override {
        return read_only_;
    }

    /// @brief Access the raw backing buffer (only when owned, i.e.
    /// PMM-allocated).
    uint8_t *raw_buffer() {
        return owns_data_ ? data_ : nullptr;
    }

  private:
    uint64_t sector_count_;  ///< Number of sectors.
    uint8_t *data_;          ///< Pointer to the data buffer.
    bool read_only_ = false; ///< Whether writes are rejected.
    bool owns_data_ = true;  ///< Whether the buffer is owned (PMM-allocated).
};

} // namespace block
} // namespace kernel
