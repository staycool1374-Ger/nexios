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
#include <kernel/sync/spinlock.hpp>
#include <kernel/arch/hal/pci.hpp>

namespace kernel {
struct TaskControlBlock;
} // namespace kernel

namespace kernel::block {

/// Maximum number of AHCI ports supported by this driver.
constexpr uint8_t AHCI_MAX_PORTS = 8;

/// Per-slot completion record (#64).  Statically embedded — the IRQ path
/// never allocates (drivers.md §7.1 binding invariant).  A record is armed
/// by wait_cmd (waiter + generation snapshot) and completed by the ISR
/// (done + error snapshot); the waiter is captured and woken outside the
/// completion lock (collect-under-lock / wake-outside-lock).
struct AhciComplRecord {
    bool done = false;                  ///< ISR observed CI clear for slot
    uint32_t error_is = 0;              ///< PORT_IS snapshot at completion
    TaskControlBlock *waiter = nullptr; ///< blocked wait_cmd task (or null)
    uint32_t waiter_gen = 0;            ///< TCB generation at arm time
};

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

    /// Destroy a probe()d driver: explicit destructor + PMM page release
    /// (mirrors probe()'s allocation path — never MemPool).
    static void destroy(AhciDriver *drv);

    /// ISR entry — scans HBA/PORT status, records per-slot completions and
    /// wakes armed waiters.  Runs in interrupt context: no blocking, no
    /// allocation, completions lock only, wakes applied outside the lock.
    /// @return true when the interrupt was consumed by this controller.
    bool handle_irq();

    /// Whether the MSI completion ISR is armed (test introspection).
    bool msi_armed() const {
        return msi_vector_ != 0;
    }

    /// Number of ISR-recorded completions (test introspection).
    uint64_t isr_completions() const {
        return isr_completions_;
    }

    /// Pure completion matcher (#64, unit-testable without hardware):
    /// which armed slots complete given CI/SACT/IS snapshots.  A slot
    /// completes when its CI bit cleared (non-NCQ) or both CI and SACT
    /// cleared (NCQ, SDBS path).  TFES snapshots mark the error bit.
    /// @param busy CI|SACT snapshot (bit set = slot still issued).
    /// @param armed_mask Bit set = slot has an armed waiter.
    /// @param is PORT_IS snapshot (TFES = error).
    /// @param out_done Mask of armed slots that completed (non-null).
    /// @param out_error Mask of completed slots with TFES set (non-null).
    static void match_completions(uint32_t busy, uint32_t armed_mask,
                                  uint32_t is, uint32_t *out_done,
                                  uint32_t *out_error);

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
    // M-1 interim bounded pause-wait: used when no ISR is armed (no MSI
    // vector or no task context) and as the wheel-arm-failure fallback.
    bool wait_cmd_poll(uint8_t port, uint8_t slot, uint64_t timeout_us);

    // MSI completion-ISR wiring (#64).  enable arms MSI + IDT handler +
    // PORT_IE + GHC_IE (in that order); disable reverses it and drains
    // armed waiters with an error wake so no BLOCKED task outlives the
    // driver.  Both are task-context only.
    bool enable_msi_irq(const arch::PciBdf &bdf);
    void disable_msi_irq();
    static void isr_entry(uint64_t vector, uint64_t error_code, uint64_t rip);

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

    // Per-slot completion records (#64): armed by wait_cmd, completed by
    // the ISR.  compl_lock_ is a leaf (ISR takes only this lock; task
    // paths take cmd_lock_ → compl_lock_ if both are needed, never the
    // reverse, and never hold it across a BLOCKED deschedule).
    AhciComplRecord compl_[AHCI_MAX_PORTS][ahci::AHCI_MAX_CMDS] = {};
    sync::SpinLock compl_lock_[AHCI_MAX_PORTS] = {};

    // MSI completion-ISR state (#64).  msi_vector_ == 0 means no ISR is
    // armed and wait_cmd uses the bounded polling fallback.
    uint8_t msi_vector_ = 0;
    uint64_t isr_completions_ = 0;
};

} // namespace kernel::block