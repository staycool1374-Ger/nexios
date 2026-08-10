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

/// @file ahci.cpp
/// @brief AHCI/SATA driver implementation.

#if defined(CONFIG_ARCH_X86_64)

#include <kernel/driver/ahci.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/driver/dma.hpp>
#include <kernel/arch/pci.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/vfs/fat32.hpp>
#include <logger.hpp>
#include <string.hpp>
#include <lib/atomic.hpp>

using namespace kernel;
using namespace kernel::ahci;
using namespace arch;

namespace kernel::block {

// ──────────────────────────────────────────────
//  MMIO Accessors
// ──────────────────────────────────────────────

inline uint32_t AhciDriver::hba_read(uint32_t reg) const {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return *reinterpret_cast<volatile uint32_t *>(abar_virt_ + reg);
}

inline void AhciDriver::hba_write(uint32_t reg, uint32_t val) const {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    *reinterpret_cast<volatile uint32_t *>(abar_virt_ + reg) = val;
}

inline uint32_t AhciDriver::port_read(uint8_t port, uint32_t reg) const {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return *reinterpret_cast<volatile uint32_t *>(
        abar_virt_ + PORT_BASE + static_cast<uint64_t>(port) * PORT_STRIDE +
        reg);
}

inline void AhciDriver::port_write(uint8_t port, uint32_t reg,
                                   uint32_t val) const {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    *reinterpret_cast<volatile uint32_t *>(
        abar_virt_ + PORT_BASE + static_cast<uint64_t>(port) * PORT_STRIDE +
        reg) = val;
}

// ──────────────────────────────────────────────
//  Constructor / Destructor
// ──────────────────────────────────────────────

AhciDriver::AhciDriver() {
}

AhciDriver::~AhciDriver() {
    if (!init_done_)
        return;

    // Stop ports and free memory
    for (uint8_t p = 0; p < port_count_; ++p) {
        port_write(p, PORT_CMD, 0);
        for (uint8_t s = 0; s < AHCI_MAX_CMDS; ++s) {
            if (ct_phys_[p][s]) {
                size_t pages =
                    (sizeof(ahci::CmdTable) + PAGE_SIZE - 1) / PAGE_SIZE;
                for (size_t i = 0; i < pages; ++i) {
                    VMM::unmap_page(ct_phys_[p][s] + i * PAGE_SIZE);
                    PMM::free_page(ct_phys_[p][s] + i * PAGE_SIZE);
                }
            }
            if (data_bufs_[p][s].phys_addr) {
                dma::free_buffer(data_bufs_[p][s]);
            }
        }
        if (cl_phys_[p]) {
            size_t pages =
                (sizeof(ahci::CmdHeader) * AHCI_MAX_CMDS + PAGE_SIZE - 1) /
                PAGE_SIZE;
            for (size_t i = 0; i < pages; ++i) {
                VMM::unmap_page(cl_phys_[p] + i * PAGE_SIZE);
                PMM::free_page(cl_phys_[p] + i * PAGE_SIZE);
            }
        }
        if (rfis_phys_[p]) {
            VMM::unmap_page(rfis_phys_[p]);
            PMM::free_page(rfis_phys_[p]);
        }
    }
}

// ──────────────────────────────────────────────
//  Port Helpers
// ──────────────────────────────────────────────

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool AhciDriver::port_wait_ready(uint8_t port, uint64_t timeout_us) {
    // Wait for port to clear BSY and DRQ
    for (uint64_t i = 0; i < timeout_us; ++i) {
        uint32_t tfd = port_read(port, PORT_TFD);
        if (!(tfd & (TFD_BSY | TFD_DRQ)))
            return true;
        arch::io_wait();
    }
    Logger::error("ahci: port %u timeout waiting for ready (TFD=0x%x)", port,
                  port_read(port, PORT_TFD));
    return false;
}

bool AhciDriver::port_init(uint8_t port) {
    // Check port status
    uint32_t ssts = port_read(port, PORT_SSTS);
    uint8_t det = ssts & SSTS_DET_MASK;
    if (det != SSTS_DET_ONLINE) {
        Logger::info("ahci: port %u no device (SSTS=0x%x)", port, ssts);
        return false;
    }

    uint32_t sig = port_read(port, PORT_SIG);
    Logger::info("ahci: port %u online sig=0x%x spd=%u", port, sig,
                 (ssts & SSTS_SPD_MASK) >> 4);

    // Stop port DMA engine
    uint32_t cmd = port_read(port, PORT_CMD);
    if (cmd & CMD_ST) {
        cmd &= ~CMD_ST;
        port_write(port, PORT_CMD, cmd);
    }
    if (cmd & CMD_FRE) {
        cmd &= ~CMD_FRE;
        port_write(port, PORT_CMD, cmd);
    }

    // Wait for port to stop
    for (int i = 0; i < 1000; ++i) {
        cmd = port_read(port, PORT_CMD);
        if (!(cmd & (CMD_CR | CMD_FR)))
            break;
        arch::io_wait();
    }

    // Clear error registers
    port_write(port, PORT_SERR, 0xFFFFFFFF);
    port_write(port, PORT_IS, 0xFFFFFFFF);

    // Allocate command list (32 entries * 32 bytes = 1024 bytes, page-aligned)
    cl_phys_[port] = PMM::alloc_contiguous(1);
    if (!cl_phys_[port]) {
        Logger::error("ahci: port %u failed to alloc command list", port);
        return false;
    }
    VMM::map_page(HHDM_OFFSET + cl_phys_[port], cl_phys_[port], false);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    cl_virt_[port] =
        reinterpret_cast<ahci::CmdHeader *>(HHDM_OFFSET + cl_phys_[port]);
    memset(cl_virt_[port], 0, sizeof(ahci::CmdHeader) * AHCI_MAX_CMDS);

    // Allocate received FIS area (256 bytes, page-aligned)
    rfis_phys_[port] = PMM::alloc_contiguous(1);
    if (!rfis_phys_[port]) {
        Logger::error("ahci: port %u failed to alloc RFIS", port);
        PMM::free_page(cl_phys_[port]);
        cl_phys_[port] = 0;
        return false;
    }
    VMM::map_page(HHDM_OFFSET + rfis_phys_[port], rfis_phys_[port], false);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    rfis_virt_[port] =
        reinterpret_cast<ahci::ReceivedFis *>(HHDM_OFFSET + rfis_phys_[port]);
    memset(rfis_virt_[port], 0, sizeof(ahci::ReceivedFis));

    // Allocate command tables (one per slot)
    size_t ct_size = sizeof(ahci::CmdTable);
    size_t ct_pages = (ct_size + PAGE_SIZE - 1) / PAGE_SIZE;
    // Roll back every slot allocated so far (incl. the current one) plus the
    // CL/RFIS pages on failure — ~AhciDriver early-returns when init_done_ is
    // false so it would never free these.  Maps were installed at
    // HHDM_OFFSET+phys, so unmap at the same VA.
    auto rollback = [&]() {
        for (uint8_t r = 0; r < AHCI_MAX_CMDS; ++r) {
            if (ct_phys_[port][r]) {
                for (size_t i = 0; i < ct_pages; ++i) {
                    VMM::unmap_page(HHDM_OFFSET + ct_phys_[port][r] +
                                    i * PAGE_SIZE);
                    PMM::free_page(ct_phys_[port][r] + i * PAGE_SIZE);
                }
                ct_phys_[port][r] = 0;
            }
            if (data_bufs_[port][r].phys_addr) {
                dma::free_buffer(data_bufs_[port][r]);
                data_bufs_[port][r] = {};
            }
        }
        if (cl_phys_[port]) {
            VMM::unmap_page(HHDM_OFFSET + cl_phys_[port]);
            PMM::free_page(cl_phys_[port]);
            cl_phys_[port] = 0;
        }
        if (rfis_phys_[port]) {
            VMM::unmap_page(HHDM_OFFSET + rfis_phys_[port]);
            PMM::free_page(rfis_phys_[port]);
            rfis_phys_[port] = 0;
        }
    };
    for (uint8_t s = 0; s < AHCI_MAX_CMDS; ++s) {
        ct_phys_[port][s] = PMM::alloc_contiguous(ct_pages);
        if (!ct_phys_[port][s]) {
            Logger::error("ahci: port %u failed to alloc cmd table slot %u",
                          port, s);
            rollback();
            return false;
        }
        for (size_t i = 0; i < ct_pages; ++i) {
            VMM::map_page(HHDM_OFFSET + ct_phys_[port][s] + i * PAGE_SIZE,
                          ct_phys_[port][s] + i * PAGE_SIZE, false);
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        ct_virt_[port][s] =
            reinterpret_cast<ahci::CmdTable *>(HHDM_OFFSET + ct_phys_[port][s]);
        memset(ct_virt_[port][s], 0, ct_size);

        // Allocate data buffer per slot
        data_bufs_[port][s] = dma::alloc_buffer(BLOCK_SIZE);
        if (!data_bufs_[port][s].phys_addr) {
            Logger::error("ahci: port %u failed to alloc data buf slot %u",
                          port, s);
            rollback();
            return false;
        }
    }

    // Program command list and FIS base addresses
    port_write(port, PORT_CLB,
               static_cast<uint32_t>(cl_phys_[port] & 0xFFFFFFFF));
    port_write(port, PORT_CLBU,
               static_cast<uint32_t>((cl_phys_[port] >> 32) & 0xFFFFFFFF));
    port_write(port, PORT_FB,
               static_cast<uint32_t>(rfis_phys_[port] & 0xFFFFFFFF));
    port_write(port, PORT_FBU,
               static_cast<uint32_t>((rfis_phys_[port] >> 32) & 0xFFFFFFFF));

    // Enable FIS receive and start port DMA engine
    cmd = port_read(port, PORT_CMD);
    cmd |= CMD_FRE | CMD_ST;
    port_write(port, PORT_CMD, cmd);

    Logger::info("ahci: port %u initialized (CL=0x%lx FB=0x%lx)", port,
                 cl_phys_[port], rfis_phys_[port]);
    return true;
}

// ──────────────────────────────────────────────
//  Command Submission
// ──────────────────────────────────────────────

uint8_t AhciDriver::alloc_slot(uint8_t port) {
    uint32_t ci = port_read(port, PORT_CI);
    uint32_t sact = port_read(port, PORT_SACT);
    uint32_t busy = ci | sact;
    for (uint8_t s = 0; s < AHCI_MAX_CMDS; ++s) {
        if (!(busy & (1U << s)))
            return s;
    }
    return 0xFF; // no free slot
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool AhciDriver::start_cmd(uint8_t port, uint8_t slot, uint8_t ata_cmd,
                           uint64_t lba, uint16_t count, uint64_t data_phys,
                           bool is_ncq, uint8_t ncq_tag) {
    if (slot >= AHCI_MAX_CMDS)
        return false;

    auto *ct = ct_virt_[port][slot];
    auto *ch = &cl_virt_[port][slot];
    memset(ct, 0, sizeof(ahci::CmdTable));
    memset(ch, 0, sizeof(ahci::CmdHeader));

    // Build command FIS
    auto &cfis = ct->cfis;
    cfis.fis_type = FIS_TYPE_REG_H2D;
    cfis.pm_port_c = 0x80; // bit 7 = command
    cfis.command = ata_cmd;
    cfis.device = ATA_DEV_LBA;

    if (is_ncq) {
        // NCQ: LBA and count are in features/lba fields; tag in pm_port_c
        cfis.lba_lo = static_cast<uint8_t>(lba & 0xFF);
        cfis.lba_mid = static_cast<uint8_t>((lba >> 8) & 0xFF);
        cfis.lba_hi = static_cast<uint8_t>((lba >> 16) & 0xFF);
        cfis.lba_lo_exp = static_cast<uint8_t>((lba >> 24) & 0xFF);
        cfis.lba_mid_exp = static_cast<uint8_t>((lba >> 32) & 0xFF);
        cfis.lba_hi_exp = static_cast<uint8_t>((lba >> 40) & 0xFF);
        cfis.count_lo = static_cast<uint8_t>(count & 0xFF);
        cfis.count_hi = static_cast<uint8_t>((count >> 8) & 0xFF);
        // NCQ tag in bits 3-7 of pm_port_c
        cfis.pm_port_c = 0x80 | ((ncq_tag & NCQ_TAG_MASK) << NCQ_TAG_SHIFT);
        // Features low contains NCQ priority / FUA
        cfis.features = 0;
    } else {
        // Non-NCQ (READ/WRITE DMA EXT)
        cfis.lba_lo = static_cast<uint8_t>(lba & 0xFF);
        cfis.lba_mid = static_cast<uint8_t>((lba >> 8) & 0xFF);
        cfis.lba_hi = static_cast<uint8_t>((lba >> 16) & 0xFF);
        cfis.lba_lo_exp = static_cast<uint8_t>((lba >> 24) & 0xFF);
        cfis.lba_mid_exp = static_cast<uint8_t>((lba >> 32) & 0xFF);
        cfis.lba_hi_exp = static_cast<uint8_t>((lba >> 40) & 0xFF);
        cfis.count_lo = static_cast<uint8_t>(count & 0xFF);
        cfis.count_hi = static_cast<uint8_t>((count >> 8) & 0xFF);
    }

    // Build PRD entry
    auto &prd = ct->prd[0];
    prd.dba = static_cast<uint32_t>(data_phys & 0xFFFFFFFF);
    prd.dbau = static_cast<uint32_t>((data_phys >> 32) & 0xFFFFFFFF);
    prd.byte_count = (BLOCK_SIZE - 1) | PRD_IOC; // IOC on last (and only) PRD
    prd.reserved = 0;

    // Build command header
    ch->cfl = sizeof(ahci::CmdFIS) / sizeof(uint32_t); // 5 DWORDS
    ch->attrs = 0;
    if (!is_ncq && ata_cmd == ATA_CMD_WRITE_DMA_EXT) {
        ch->attrs |= CMDHDR_WRITE;
    }
    if (is_ncq) {
        if (ata_cmd == ATA_CMD_WRITE_FPDMA_QUEUED) {
            ch->attrs |= CMDHDR_WRITE;
        }
        // NCQ: PRD byte count is 0 (data size set via count field)
    }
    ch->prdbc = 0;
    ch->ctba = static_cast<uint32_t>(ct_phys_[port][slot] & 0xFFFFFFFF);
    ch->ctbau =
        static_cast<uint32_t>((ct_phys_[port][slot] >> 32) & 0xFFFFFFFF);

    // Ensure writes are visible before issuing command
    kernel::atomic_fence();

    // Issue command
    port_write(port, PORT_CI, 1U << slot);
    return true;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool AhciDriver::wait_cmd(uint8_t port, uint8_t slot, uint64_t timeout_us) {
    for (uint64_t i = 0; i < timeout_us; ++i) {
        uint32_t ci = port_read(port, PORT_CI);
        if (!(ci & (1U << slot))) {
            // Command completed — check for errors
            uint32_t is = port_read(port, PORT_IS);
            if (is & PORT_IS_TFES) {
                Logger::error(
                    "ahci: cmd slot %u task file error (IS=0x%x SERR=0x%x)",
                    slot, is, port_read(port, PORT_SERR));
                port_write(port, PORT_IS, is); // acknowledge
                return false;
            }
            return true;
        }
        // Check for error while running
        uint32_t tfd = port_read(port, PORT_TFD);
        if (tfd & TFD_ERR) {
            Logger::error("ahci: cmd slot %u error (TFD=0x%x)", slot, tfd);
            port_write(port, PORT_CI, 1U << slot); // clear CI
            return false;
        }
        arch::io_wait();
    }
    Logger::error("ahci: cmd slot %u timeout", slot);
    port_write(port, PORT_CI, 1U << slot); // clear CI
    return false;
}

// ──────────────────────────────────────────────
//  BlockDevice Interface
// ──────────────────────────────────────────────

bool AhciDriver::read_sector(uint64_t lba, uint8_t *buffer) {
    if (!init_done_ || active_port_ >= AHCI_MAX_PORTS)
        return false;

    uint8_t slot = alloc_slot(active_port_);
    if (slot >= AHCI_MAX_CMDS) {
        Logger::error("ahci: no free slot for read");
        return false;
    }

    auto &dbuf = data_bufs_[active_port_][slot];
    uint8_t ata_cmd =
        ncq_supported_ ? ATA_CMD_READ_FPDMA_QUEUED : ATA_CMD_READ_DMA_EXT;

    if (!start_cmd(active_port_, slot, ata_cmd, lba, 1, dbuf.phys_addr,
                   ncq_supported_, slot)) {
        return false;
    }

    if (!wait_cmd(active_port_, slot, 5000000)) { // 5s timeout
        return false;
    }

    // Copy from DMA buffer to caller
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    memcpy(buffer, reinterpret_cast<void *>(dbuf.virt_addr), BLOCK_SIZE);
    return true;
}

bool AhciDriver::write_sector(uint64_t lba, const uint8_t *buffer) {
    if (!init_done_ || active_port_ >= AHCI_MAX_PORTS)
        return false;

    uint8_t slot = alloc_slot(active_port_);
    if (slot >= AHCI_MAX_CMDS) {
        Logger::error("ahci: no free slot for write");
        return false;
    }

    auto &dbuf = data_bufs_[active_port_][slot];
    // Copy data to DMA buffer first
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    memcpy(reinterpret_cast<void *>(dbuf.virt_addr), buffer, BLOCK_SIZE);

    uint8_t ata_cmd =
        ncq_supported_ ? ATA_CMD_WRITE_FPDMA_QUEUED : ATA_CMD_WRITE_DMA_EXT;

    if (!start_cmd(active_port_, slot, ata_cmd, lba, 1, dbuf.phys_addr,
                   ncq_supported_, slot)) {
        return false;
    }

    if (!wait_cmd(active_port_, slot, 5000000)) {
        return false;
    }

    return true;
}

// ──────────────────────────────────────────────
//  Initialization
// ──────────────────────────────────────────────

bool AhciDriver::init() {
    // Find AHCI controller via PCI
    const auto *dev = arch::pci_find_device(0x01, 0x06);
    if (!dev) {
        Logger::info("ahci: no AHCI controller found");
        return false;
    }

    Logger::info("ahci: found AHCI controller at %d:%d.%d", dev->bdf.bus,
                 dev->bdf.device, dev->bdf.function);

    // Read ABAR (BAR 5)
    if (dev->bar_count <= 5 || dev->bars[5].address == 0) {
        Logger::error("ahci: ABAR (BAR5) not valid");
        return false;
    }

    abar_phys_ = dev->bars[5].address;
    Logger::info("ahci: ABAR physical = 0x%lx", abar_phys_);

    // Map ABAR MMIO region (typically 8K or 16K)
    uint64_t abar_size = 0x2000; // standard AHCI MMIO size
    uint64_t abar_start = abar_phys_ & ~(PAGE_SIZE - 1);
    uint64_t abar_end =
        ((abar_phys_ + abar_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    for (uint64_t page = abar_start; page < abar_end; page += PAGE_SIZE) {
        VMM::map_page(HHDM_OFFSET + page, page, false);
    }
    abar_virt_ = HHDM_OFFSET + abar_phys_;

    // Enable bus mastering and memory space on the device
    dma::pci_set_bus_master(dev->bdf, true);
    uint32_t pci_addr = arch::pci_make_addr(dev->bdf, arch::PCI_COMMAND);
    uint16_t cmd = arch::pci_config_readw(pci_addr);
    cmd |= arch::PCI_CMD_MEM_SPACE;
    arch::pci_config_writel(pci_addr, cmd);

    // Check capabilities
    uint32_t cap = hba_read(HBA_CAP);
    port_count_ = static_cast<uint8_t>((cap & CAP_NP) + 1);
    ncq_supported_ = (cap & CAP_NCQ) != 0;
    Logger::info("ahci: %u ports%s", port_count_,
                 ncq_supported_ ? ", NCQ supported" : "");

    uint32_t pi = hba_read(HBA_PI);
    Logger::info("ahci: ports implemented = 0x%x", pi);

    // Reset HBA
    hba_write(HBA_GHC, hba_read(HBA_GHC) | GHC_HR);
    for (int i = 0; i < 10000; ++i) {
        if (!(hba_read(HBA_GHC) & GHC_HR))
            break;
        arch::io_wait();
    }
    Logger::info("ahci: HBA reset complete");

    // Enable AHCI
    hba_write(HBA_GHC, hba_read(HBA_GHC) | GHC_AE | GHC_IE);

    // Initialize ports with devices
    for (uint8_t p = 0; p < port_count_ && p < AHCI_MAX_PORTS; ++p) {
        if (!(pi & (1U << p)))
            continue;

        if (port_init(p)) {
            active_port_ = p;

            // Read sector count via IDENTIFY command
            uint8_t slot = alloc_slot(p);
            if (slot < AHCI_MAX_CMDS) {
                auto &dbuf = data_bufs_[p][slot];
                if (start_cmd(p, slot, ahci::ATA_CMD_IDENTIFY, 0, 0,
                              dbuf.phys_addr, false, 0)) {
                    if (wait_cmd(p, slot, 5000000)) {
                        // Parse IDENTIFY data to get sector count
                        // IDENTIFY data words 60-61 = 28-bit LBA sectors
                        // words 100-103 = 48-bit LBA sectors
                        // NOLINTNEXTLINE(performance-no-int-to-ptr)
                        auto *id_data =
                            reinterpret_cast<uint16_t *>(dbuf.virt_addr);
                        uint32_t sec_lo = id_data[60];
                        uint32_t sec_hi = id_data[61];
                        uint64_t sec_48_lo =
                            (static_cast<uint64_t>(id_data[100])) |
                            (static_cast<uint64_t>(id_data[101]) << 16);
                        uint64_t sec_48_hi =
                            (static_cast<uint64_t>(id_data[102])) |
                            (static_cast<uint64_t>(id_data[103]) << 16);
                        sector_count_ = sec_48_lo | (sec_48_hi << 32);
                        if (sector_count_ == 0) {
                            sector_count_ =
                                sec_lo | (static_cast<uint64_t>(sec_hi) << 16);
                        }
                        Logger::info("ahci: port %u %lu sectors", p,
                                     sector_count_);
                    }
                }
            }

            // Use first online port only
            break;
        }
    }

    if (active_port_ >= AHCI_MAX_PORTS) {
        Logger::error("ahci: no active port found");
        return false;
    }

    init_done_ = true;
    Logger::info("ahci: driver initialized (port %u, %lu sectors)",
                 active_port_, sector_count_);
    return true;
}

AhciDriver *AhciDriver::probe() {
#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-possible-null-dereference"
#endif
    auto *drv_mem = kernel::MemPool::alloc(sizeof(AhciDriver));
    if (!drv_mem) return nullptr;
    auto *drv = new (drv_mem) AhciDriver();
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
    if (!drv || !drv->init()) {
        drv->~AhciDriver(); kernel::MemPool::free(drv);
        return nullptr;
    }
    return drv;
}

} // namespace kernel::block

#endif // CONFIG_ARCH_X86_64