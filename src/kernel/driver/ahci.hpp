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

/// @file ahci.hpp
/// @brief AHCI/SATA driver — extends BlockDevice with DMA and NCQ support.
///        Uses ahci_protocol.hpp for register layouts and DmaBuffer for data.

#pragma once

#include <types.hpp>
#include <kernel/driver/block_device.hpp>
#include <kernel/driver/ahci_protocol.hpp>
#include <kernel/driver/dma.hpp>
#include <kernel/sync/mutex.hpp>

namespace kernel::block {

/// Maximum number of AHCI ports supported by this driver.
constexpr uint8_t AHCI_MAX_PORTS = 8;

/// AHCI/SATA driver — PCI bus master DMA via native AHCI command list.
class AhciDriver final : public BlockDevice {
  public:
    AhciDriver();
    ~AhciDriver();

    /// Probe PCI for an AHCI controller and initialize.
    /// @return true if HBA found and at least one port has a device.
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

    /// Probe for the first AHCI controller with a drive, create driver, and
    /// return it. Returns nullptr if no AHCI drive found.
    static AhciDriver *probe();

  private:
    // MMIO helpers
    uint32_t hba_read(uint32_t reg) const;
    void hba_write(uint32_t reg, uint32_t val) const;
    uint32_t port_read(uint8_t port, uint32_t reg) const;
    void port_write(uint8_t port, uint32_t reg, uint32_t val) const;

    // Port management
    bool port_init(uint8_t port);
    bool port_wait_ready(uint8_t port, uint64_t timeout_us);

    // Command submission
    uint8_t alloc_slot(uint8_t port);
    bool start_cmd(uint8_t port, uint8_t slot, uint8_t ata_cmd, uint64_t lba,
                   uint16_t count, uint64_t data_phys, bool is_ncq,
                   uint8_t ncq_tag);
    bool wait_cmd(uint8_t port, uint8_t slot, uint64_t timeout_us);

    // HBA state
    uint64_t abar_phys_ = 0;
    uint64_t abar_virt_ = 0;
    uint8_t port_count_ = 0;
    uint8_t active_port_ = 0xFF;
    uint64_t sector_count_ = 0;
    bool ncq_supported_ = false;
    bool init_done_ = false;

    // Per-port memory (physically contiguous for device access)
    uint64_t cl_phys_[AHCI_MAX_PORTS] = {};
    ahci::CmdHeader *cl_virt_[AHCI_MAX_PORTS] = {};

    uint64_t rfis_phys_[AHCI_MAX_PORTS] = {};
    ahci::ReceivedFis *rfis_virt_[AHCI_MAX_PORTS] = {};

    // Command tables per slot
    uint64_t ct_phys_[AHCI_MAX_PORTS][ahci::AHCI_MAX_CMDS] = {};
    ahci::CmdTable *ct_virt_[AHCI_MAX_PORTS][ahci::AHCI_MAX_CMDS] = {};

    // DMA data buffers per slot
    dma::DmaBuffer data_bufs_[AHCI_MAX_PORTS][ahci::AHCI_MAX_CMDS] = {};

    // H-1 (audit-drivers-vfs-net-v0.4.2): serializes alloc_slot → start_cmd →
    // wait_cmd per port.  Without it two concurrent read_sector/write_sector
    // calls can acquire the same slot and the same data_bufs_ DMA buffer →
    // data corruption; start_cmd/wait_cmd are likewise unsynchronized.
    sync::Mutex cmd_lock_[AHCI_MAX_PORTS] = {};
};

} // namespace kernel::block