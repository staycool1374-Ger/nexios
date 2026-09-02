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

/// @file dma.hpp
/// @brief DMA driver — buffer management, scatter-gather, PRD table
/// construction,
///        and portable DMA channel abstraction for kernel and userspace
///        drivers.

#pragma once

#include <types.hpp>
#include <constants.hpp>
#include <kernel/arch/pci.hpp>
#include <kernel/sync/irq_spinlock_guard.hpp>

namespace kernel::dma {

/// Maximum entries in a scatter-gather list or PRD table.
constexpr size_t DMA_MAX_SG_ENTRIES = 256;
constexpr size_t DMA_MAX_PRD_ENTRIES = 256;

/// DMA direction
enum class Direction : uint8_t {
    READ = 0,  // device reads from memory (memory → device)
    WRITE = 1, // device writes to memory (device → memory)
};

/// A single scatter-gather entry
struct SgEntry {
    uint64_t phys_addr;
    uint64_t length;
    uint64_t virt_addr;
};

/// Scatter-gather list
struct SgList {
    SgEntry entries[DMA_MAX_SG_ENTRIES];
    size_t count;
    uint64_t total_length;
};

/// Physical Region Descriptor (ATA bus master format)
struct PrdEntry {
    uint32_t phys_addr;
    uint16_t byte_count; // actual byte count - 1 (0 means 65536)
    uint16_t flags;      // bit 15 = EOT
} __attribute__((packed));

/// PRD table — fixed-size array of PrdEntry
struct PrdTable {
    PrdEntry entries[DMA_MAX_PRD_ENTRIES];
    size_t count;
};

// --- DMA Buffer ---

/// A physically contiguous DMA buffer.
struct DmaBuffer {
    uint64_t phys_addr;
    uint64_t virt_addr;
    size_t size;
    bool owned; // true if we allocated it and should free on destroy
};

/// Allocate a physically contiguous DMA buffer.
/// @param size  Size in bytes (will be rounded up to PAGE_SIZE).
/// @return DmaBuffer (phys_addr=0 on failure).
DmaBuffer alloc_buffer(size_t size);

/// Free a previously allocated DMA buffer.
void free_buffer(DmaBuffer &buf);

// --- Scatter-Gather ---

/// Build a scatter-gather list from a DMA buffer and optional offset/length.
/// For a single physically contiguous buffer, the SG list has one entry.
bool sg_from_buffer(SgList &sg, const DmaBuffer &buf, size_t offset = 0,
                    size_t length = 0);

/// Build a scatter-gather list from a virtual address range (which may cross
/// page boundaries). Translates virtual addresses to physical via VMM.
bool sg_from_virt(SgList &sg, uint64_t virt_addr, size_t length);

/// Reset a scatter-gather list.
void sg_reset(SgList &sg);

// --- PRD Table ---

/// Build a PRD table from a scatter-gather list.
/// ATA bus master PRD format: 32-bit phys addr, 16-bit byte count-1, 16-bit
/// flags. The table is suitable for programming a PCI bus master controller.
/// @param prd   Output PRD table (must have backing memory accessible by
/// device).
/// @param sg    Input scatter-gather list.
/// @param eot   If true, sets the EOT flag on the last entry.
/// @return Number of PRD entries written.
size_t prd_from_sg(PrdTable &prd, const SgList &sg, bool eot = true);

/// Reset a PRD table.
void prd_reset(PrdTable &prd);

// --- PCI Bus Master ---

/// Enable/disable PCI bus mastering for a device.
/// @param bdf     BDF address.
/// @param enable  True to enable bus mastering, false to disable.
void pci_set_bus_master(arch::PciBdf bdf, bool enable);

// --- DMA Channel Abstraction ---

/// BMDMA command register bits.
constexpr uint8_t BMCMD_START = 0x01;
constexpr uint8_t BMCMD_STOP = 0x00;
constexpr uint8_t BMCMD_READ = 0x08; // device → memory direction

/// BMDMA status register bits.
constexpr uint8_t BMSTAT_ACTIVE = 0x01;
constexpr uint8_t BMSTAT_ERROR = 0x02;
constexpr uint8_t BMSTAT_INTR = 0x04;

/// DMA completion callback.
/// @param context  User-provided context value.
/// @param success  True if transfer completed without error.
using DmaCallback = void (*)(uint64_t context, bool success);

/// Abstract DMA channel interface.
/// Implementations bridge hardware-specific DMA register access (port I/O,
/// MMIO, or IPC-mediated). Used by DmaEngine for portable driver code.
class DmaChannel {
  public:
    virtual ~DmaChannel() = default;

    /// Initialize the channel.
    /// @return true if the channel is ready.
    virtual bool init() = 0;

    /// Start a DMA transfer.
    /// @param prd  PRD table (must remain valid until transfer completes).
    /// @param dir  Transfer direction.
    /// @return true if the transfer was started.
    virtual bool start(PrdTable &prd, Direction dir) = 0;

    /// Poll whether the channel is busy.
    virtual bool is_busy() = 0;

    /// Handle a DMA completion interrupt.
    /// Acknowledges the interrupt and returns completion status.
    /// @return true if the transfer completed successfully.
    virtual bool handle_irq() = 0;

    /// Abort any in-progress transfer.
    virtual void abort() = 0;
};

/// BMDMA channel implementation via PCI port I/O (kernel mode).
/// Accesses the BMDMA controller registers at the given I/O base.
class BmDmaChannel final : public DmaChannel {
  public:
    explicit BmDmaChannel(uint16_t bm_io_base);
    bool init() override;
    bool start(PrdTable &prd, Direction dir) override;
    bool is_busy() override;
    bool handle_irq() override;
    void abort() override;

  private:
    uint16_t bm_io_base_;
};

/// High-level DMA engine that manages a DmaChannel and completion callbacks.
/// Decouples transfer orchestration from hardware register access.
class DmaEngine {
  public:
    explicit DmaEngine(DmaChannel &channel);

    /// Start a DMA transfer with optional completion callback.
    /// @param prd  PRD table describing the transfer.
    /// @param dir  Transfer direction.
    /// @param cb   Optional callback invoked on completion.
    /// @param ctx  Context passed to callback.
    /// @return true if transfer started successfully.
    bool start_transfer(PrdTable &prd, Direction dir, DmaCallback cb = nullptr,
                        uint64_t ctx = 0);

    /// Check whether the engine is currently busy.
    bool is_busy();

    /// Handle a DMA completion interrupt.
    /// Delegates to channel, then invokes callback on success.
    /// @return true if the interrupt was consumed.
    bool handle_irq();

    /// Abort any in-progress transfer.
    void abort();

  private:
    DmaChannel &channel_;
    bool active_;
    DmaCallback callback_;
    uint64_t callback_ctx_;
    /// @brief Serializes ISR (handle_irq) vs task (start_transfer/abort/
    ///        is_busy) access.  Always taken via IrqSpinLockGuard (cli +
    ///        lock): the ISR runs on the same core, so a plain SpinLockGuard
    ///        would self-deadlock when the ISR preempts the lock holder.
    mutable sync::SpinLock lock_;
};

// --- Ping-Pong Double-Buffer ---

/// Ping-pong DMA double-buffer manager.
/// Provides two alternating DMA buffers with automatic chaining:
/// while buffer A transfers, buffer B is available for data preparation.
class PingPongDma {
  public:
    /// Type of the chained completion callback.
    /// @param ctx    User context.
    /// @param buf    The buffer that just completed.
    /// @param success  True if transfer completed without error.
    using ChainCallback = void (*)(uint64_t ctx, DmaBuffer *buf, bool success);

    /// Construct a ping-pong manager.
    /// @param engine  DmaEngine to use for transfers.
    /// @param buf_size  Size of each DMA buffer.
    PingPongDma(DmaEngine &engine, size_t buf_size);

    /// Allocate both DMA buffers.
    /// @return true if both buffers allocated.
    bool init();

    /// Get the buffer currently available for data preparation.
    DmaBuffer *prepare_buf();

    /// Get the buffer currently being transferred.
    DmaBuffer *xfer_buf();

    /// Start or chain the next transfer.
    /// Swaps buffers: the prepared buffer becomes the transfer buffer.
    /// @param dir  Transfer direction.
    /// @param cb   Chained completion callback (may be null).
    /// @param ctx  Context for callback.
    /// @return true if transfer started successfully.
    bool start_next(Direction dir, ChainCallback cb = nullptr,
                    uint64_t ctx = 0);

    /// Handle a DMA completion — invoked by DmaEngine callback.
    void on_completion(uint64_t ctx, bool success);

    /// Free both buffers.
    void shutdown();

    /// Check if a transfer is in progress.
    bool busy() const {
        sync::IrqSpinLockGuard g(lock_);
        return active_;
    }

    /// Get the number of completed transfers.
    uint64_t completed_count() const {
        sync::IrqSpinLockGuard g(lock_);
        return completed_;
    }

  private:
    DmaEngine &engine_;
    DmaBuffer bufs_[2];
    size_t buf_size_;
    uint8_t prepare_idx_; // index of buffer being filled
    uint8_t xfer_idx_;    // index of buffer being transferred
    bool active_;
    uint64_t completed_;
    ChainCallback chain_cb_;
    uint64_t chain_ctx_;
    /// @brief Serializes task (start_next/prepare_buf/xfer_buf/shutdown) vs
    ///        IRQ (on_completion) access.  IrqSpinLockGuard (cli + lock).
    ///        Lock order: PingPongDma::lock_ -> DmaEngine::lock_ (nested in
    ///        start_next); never reversed.
    mutable sync::SpinLock lock_;
};

} // namespace kernel::dma